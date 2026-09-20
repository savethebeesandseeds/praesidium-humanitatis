#include "praesidium-humanitatis/calibration.h"

#include <cmath>
#include <limits>
#include <string>
#include <tuple>

namespace praesidium_humanitatis {
namespace {

void validate_tensor(const torch::Tensor& value, const char* name, int64_t rank) {
  TORCH_CHECK(value.defined(), name, " must be defined");
  TORCH_CHECK(value.layout() == torch::kStrided, name, " must use dense strided layout");
  TORCH_CHECK(value.dim() == rank, name, " has an invalid rank");
  TORCH_CHECK(value.scalar_type() == torch::kFloat32 ||
                  value.scalar_type() == torch::kFloat64,
              name, " must use float32 or float64");
  TORCH_CHECK(value.device().is_cpu() || value.device().is_cuda(),
              name, " must use a CPU or CUDA device");
  for (int64_t axis = 0; axis < rank; ++axis) {
    TORCH_CHECK(value.size(axis) > 0, name, " dimensions must be nonempty");
  }
  TORCH_CHECK(torch::isfinite(value).all().item<bool>(), name, " must be finite");
}

void validate_matching(const torch::Tensor& value, const torch::Tensor& reference,
                       const char* name) {
  TORCH_CHECK(value.sizes() == reference.sizes(), name, " shape must match bounds");
  TORCH_CHECK(value.scalar_type() == reference.scalar_type(), name, " dtype must match bounds");
  TORCH_CHECK(value.device() == reference.device(), name, " device must match bounds");
}

void validate_interval(const PredictionInterval& predictions) {
  validate_tensor(predictions.lower, "Lower bounds", 2);
  validate_tensor(predictions.upper, "Upper bounds", 2);
  validate_matching(predictions.upper, predictions.lower, "Upper bounds");
  TORCH_CHECK((predictions.lower <= predictions.upper).all().item<bool>(),
              "Interval endpoints must not cross; predictions are never sorted");
}

void validate_targets(const torch::Tensor& targets, const torch::Tensor& lower) {
  validate_tensor(targets, "Targets", 2);
  validate_matching(targets, lower, "Targets");
}

int64_t conformal_rank(int64_t count, double miscoverage) {
  TORCH_CHECK(std::isfinite(miscoverage) && miscoverage > 0.0 && miscoverage < 1.0,
              "Miscoverage must be finite and in (0, 1)");
  // Ensure N+1 can be represented exactly for the floating-point rank formula.
  constexpr int64_t max_exact_count = 9007199254740991LL;
  TORCH_CHECK(count > 0 && count <= max_exact_count,
              "Calibration sample_count is outside the supported positive range");
  // ceil(M * (1-alpha)) = M - floor(M * alpha). Computing 1-alpha first
  // can erase the difference between adjacent representable alpha values.
  // If the rounded product is an integer, FMA recovers the sign of its exact
  // residual and detects a true product just below that integer. Otherwise
  // rounding cannot have crossed an integer boundary in this supported range.
  const double total = static_cast<double>(count) + 1.0;
  const double product = total * miscoverage;
  double floored = std::floor(product);
  if (product == floored && std::fma(total, miscoverage, -product) < 0.0) {
    floored -= 1.0;
  }
  const auto rank = count + 1 - static_cast<int64_t>(floored);
  TORCH_CHECK(rank >= 1 && rank <= count,
              "Insufficient calibration examples: requested coverage requires rank N+1; "
              "use more calibration examples or a larger miscoverage");
  return rank;
}

void require_finite(const torch::Tensor& value, const char* name) {
  TORCH_CHECK(torch::isfinite(value).all().item<bool>(), name,
              " overflowed; rescale inputs or use float64 precision");
}

}  // namespace

CQRCalibration fit_cqr(const PredictionInterval& calibration_predictions,
                       const torch::Tensor& targets, double miscoverage) {
  torch::NoGradGuard no_grad;
  validate_interval(calibration_predictions);
  validate_targets(targets, calibration_predictions.lower);
  const auto count = targets.size(0);
  const auto rank = conformal_rank(count, miscoverage);
  auto scores = torch::maximum(calibration_predictions.lower - targets,
                               targets - calibration_predictions.upper);
  require_finite(scores, "Calibration scores");
  // Each subtraction may round down. One step toward +infinity bounds the
  // exact positive score from above, including large-offset cancellation.
  // Nonpositive scores will be clamped to zero and need no adjustment.
  scores = torch::where(scores > 0.0,
                        torch::nextafter(scores, torch::full_like(
                            scores, std::numeric_limits<double>::infinity())), scores);
  require_finite(scores, "Outward-rounded calibration scores");
  // kthvalue is one-based, and dim 0 consists of independent examples for each
  // horizon. No interpolated percentile or flattening across horizons is used.
  auto correction = std::get<0>(scores.kthvalue(rank, 0)).clamp_min(0.0);
  require_finite(correction, "Calibration correction");
  return {correction, miscoverage, count};
}

PredictionInterval apply_cqr(const PredictionInterval& predictions,
                             const CQRCalibration& calibration) {
  torch::NoGradGuard no_grad;
  validate_interval(predictions);
  conformal_rank(calibration.sample_count, calibration.miscoverage);
  validate_tensor(calibration.correction, "Calibration correction", 1);
  TORCH_CHECK(calibration.correction.size(0) == predictions.lower.size(1),
              "Calibration correction horizon must match bounds");
  TORCH_CHECK(calibration.correction.scalar_type() == predictions.lower.scalar_type(),
              "Calibration correction dtype must match bounds");
  TORCH_CHECK(calibration.correction.device() == predictions.lower.device(),
              "Calibration correction device must match bounds");
  TORCH_CHECK((calibration.correction >= 0.0).all().item<bool>(),
              "Calibration correction must be nonnegative");
  PredictionInterval result{predictions.lower - calibration.correction.unsqueeze(0),
                            predictions.upper + calibration.correction.unsqueeze(0)};
  // Bound the exact additions/subtractions outward. Preserve zero-correction
  // intervals exactly, including endpoints at the finite representable limit.
  const auto expand = calibration.correction.unsqueeze(0) > 0.0;
  result.lower = torch::where(expand, torch::nextafter(result.lower, torch::full_like(
      result.lower, -std::numeric_limits<double>::infinity())), predictions.lower);
  result.upper = torch::where(expand, torch::nextafter(result.upper, torch::full_like(
      result.upper, std::numeric_limits<double>::infinity())), predictions.upper);
  require_finite(result.lower, "Calibrated lower bounds");
  require_finite(result.upper, "Calibrated upper bounds");
  return result;
}

IntervalMetrics evaluate_intervals(const PredictionInterval& predictions,
                                   const torch::Tensor& targets) {
  torch::NoGradGuard no_grad;
  validate_interval(predictions);
  validate_targets(targets, predictions.lower);
  const auto covered = ((targets >= predictions.lower) & (targets <= predictions.upper))
                           .to(targets.scalar_type());
  const auto widths = predictions.upper - predictions.lower;
  require_finite(widths, "Interval widths");
  IntervalMetrics result{covered.mean().item<double>(), widths.mean().item<double>(),
                          covered.mean(0), widths.mean(0)};
  TORCH_CHECK(std::isfinite(result.coverage) && std::isfinite(result.mean_width),
              "Interval metrics overflowed; rescale inputs or use float64 precision");
  require_finite(result.coverage_by_horizon, "Interval coverage metrics");
  require_finite(result.width_by_horizon, "Interval width metrics");
  return result;
}

}  // namespace praesidium_humanitatis
