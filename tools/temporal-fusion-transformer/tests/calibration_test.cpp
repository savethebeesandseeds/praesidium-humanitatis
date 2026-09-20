#include "praesidium-humanitatis/calibration.h"

#include <torch/cuda.h>

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace praesidium_humanitatis;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void close(double actual, double expected, const std::string& message) {
  require(std::abs(actual - expected) < 1e-6, message);
}

void close(const torch::Tensor& actual, const torch::Tensor& expected,
           const std::string& message) {
  require(actual.sizes() == expected.sizes() && actual.device() == expected.device() &&
              actual.scalar_type() == expected.scalar_type() &&
              torch::allclose(actual, expected, 1e-6, 1e-6), message);
}

template <typename Function>
void rejects(Function&& function, const std::string& message) {
  bool threw = false;
  try { function(); } catch (const std::exception&) { threw = true; }
  require(threw, message);
}

void order_statistics(const torch::Device& device) {
  for (auto dtype : {torch::kFloat32, torch::kFloat64}) {
    const auto options = torch::TensorOptions().device(device).dtype(dtype);
    const PredictionInterval point{torch::zeros({5, 2}, options), torch::zeros({5, 2}, options)};
    const auto targets = torch::tensor({1., 100., 2., 200., 3., 300., 4., 400., 5., 500.}, options)
                             .reshape({5, 2});
    const auto calibration = fit_cqr(point, targets, 0.5);
    close(calibration.correction, torch::tensor({3., 300.}, options),
          "rank ceil(6 * 0.5)=3 is selected separately at each horizon");
    require(calibration.sample_count == 5 && calibration.miscoverage == 0.5,
            "calibration metadata describes independent windows");
    // A common incorrect implementation uses an interpolated 80th percentile
    // or omits N+1. Both differ from this order statistic on five examples.
    close(fit_cqr(point, targets, 0.2).correction, torch::tensor({5., 500.}, options),
          "finite sample N+1 adjustment and no percentile interpolation");
    const PredictionInterval unseen{torch::ones({2, 2}, options), torch::full({2, 2}, 2., options)};
    const auto adjusted = apply_cqr(unseen, calibration);
    close(adjusted.lower, torch::tensor({-2., -299., -2., -299.}, options).reshape({2, 2}),
          "lower bounds expand by horizon-specific correction");
    close(adjusted.upper, torch::tensor({5., 302., 5., 302.}, options).reshape({2, 2}),
          "upper bounds expand on a different test batch size");
    const auto minimum = fit_cqr({torch::zeros({1, 1}, options), torch::zeros({1, 1}, options)},
                                 torch::ones({1, 1}, options), 0.5);
    close(minimum.correction, torch::ones({1}, options), "one example is valid at 50 percent coverage");
    rejects([&] { fit_cqr(point, targets, 0.1); }, "rank N+1 must be rejected, not clipped to N");
  }
}

void outward_only(const torch::Device& device) {
  const auto options = torch::TensorOptions().device(device).dtype(torch::kFloat64);
  const PredictionInterval wide{torch::full({5, 2}, -10., options), torch::full({5, 2}, 10., options)};
  const auto targets = torch::tensor({0., 0., 1., 1., 2., 2., 3., 3., 4., 4.}, options).reshape({5, 2});
  const auto calibration = fit_cqr(wide, targets, 0.5);
  close(calibration.correction, torch::zeros({2}, options), "negative CQR order statistics clamp to zero");
  const auto result = apply_cqr(wide, calibration);
  close(result.lower, wide.lower, "wide intervals are not shrunk");
  close(result.upper, wide.upper, "wide intervals are not shrunk");
  require(torch::equal(result.lower, wide.lower) && torch::equal(result.upper, wide.upper),
          "zero corrections preserve exact endpoints");
}

void adjacent_miscoverage_ranks(const torch::Device& device) {
  const auto options = torch::TensorOptions().device(device).dtype(torch::kFloat64);
  const PredictionInterval one{torch::zeros({1, 1}, options), torch::zeros({1, 1}, options)};
  const auto target = torch::ones({1, 1}, options);
  rejects([&] { fit_cqr(one, target, std::nextafter(0.5, 0.0)); },
          "coverage immediately above one half requires rank N+1 with N=1");
  fit_cqr(one, target, 0.5);
  fit_cqr(one, target, std::nextafter(0.5, 1.0));

  for (const auto& boundary : std::vector<std::pair<int64_t, double>>{{3, 0.5}, {7, 0.25}}) {
    const auto count = boundary.first;
    const auto alpha = boundary.second;
    const auto values = torch::arange(1, count + 1, options).unsqueeze(1);
    const PredictionInterval point{torch::zeros_like(values), torch::zeros_like(values)};
    const auto at_rank = static_cast<double>(count + 1) * (1.0 - alpha);
    close(fit_cqr(point, values, std::nextafter(alpha, 0.0)).correction,
          torch::full({1}, at_rank + 1., options), "rank changes below an exact alpha boundary");
    close(fit_cqr(point, values, alpha).correction,
          torch::full({1}, at_rank, options), "rank at an exact alpha boundary");
    close(fit_cqr(point, values, std::nextafter(alpha, 1.0)).correction,
          torch::full({1}, at_rank, options), "rank above an exact alpha boundary");
  }
  // 10 * double(.3) rounds to 3, but its exact product is just below 3.
  // Thus the exact requested double value requires rank 8, not rank 7.
  const auto values = torch::arange(1, 10, options).unsqueeze(1);
  const PredictionInterval point{torch::zeros_like(values), torch::zeros_like(values)};
  close(fit_cqr(point, values, 0.3).correction, torch::full({1}, 8., options),
        "FMA detects a product rounded up to an integer");
  close(fit_cqr(point, values, std::nextafter(0.3, 1.0)).correction,
        torch::full({1}, 7., options), "rank decreases after the exact product crosses an integer");
}

void floating_point_outward_rounding(const torch::Device& device) {
  for (auto dtype : {torch::kFloat32, torch::kFloat64}) {
    const auto options = torch::TensorOptions().device(device).dtype(dtype);
    const double large = dtype == torch::kFloat32 ? 1e8 : 1e16;
    // Exact required corrections exceed a large representable value by one,
    // which ordinary subtraction rounds away in both tested precisions.
    for (double sign : {-1., 1.}) {
      const auto point = torch::full({5, 1}, sign * large, options);
      const auto targets = torch::full({5, 1}, -sign, options);
      const auto calibration = fit_cqr({point, point}, targets, 0.2);
      require(calibration.correction.item<double>() > large,
              "positive scores round upward when cancellation loses the target offset");
      const auto interval = apply_cqr({point, point}, calibration);
      close(evaluate_intervals(interval, targets).coverage, 1.0,
            "identical exchangeable calibration and test examples remain covered");
    }
    const auto large_point = torch::full({1, 1}, large, options);
    const CQRCalibration small{torch::ones({1}, options), 0.5, 1};
    const auto expanded = apply_cqr({large_point, large_point}, small);
    require(expanded.lower.item<double>() < large && expanded.upper.item<double>() > large,
            "positive correction cannot disappear through rounded endpoint arithmetic");

    const double maximum = dtype == torch::kFloat32
        ? static_cast<double>(std::numeric_limits<float>::max())
        : std::numeric_limits<double>::max();
    const auto extreme = torch::full({1, 1}, maximum, options);
    const CQRCalibration zero{torch::zeros({1}, options), 0.5, 1};
    const auto unchanged = apply_cqr({extreme, extreme}, zero);
    require(torch::equal(unchanged.lower, extreme) && torch::equal(unchanged.upper, extreme),
            "zero correction preserves finite-limit endpoints without overflow");
    rejects([&] { apply_cqr({extreme, extreme}, small); },
            "outward rounding beyond the finite representable limit is rejected");
    const auto origin = torch::zeros({1, 1}, options);
    rejects([&] { fit_cqr({origin, origin}, extreme, 0.5); },
            "positive score rounding beyond the finite limit is rejected");
  }
}

void interval_metrics(const torch::Device& device) {
  const auto options = torch::TensorOptions().device(device).dtype(torch::kFloat64);
  const PredictionInterval bounds{torch::tensor({0., 0., -1., 2.}, options).reshape({2, 2}),
                                  torch::tensor({2., 3., 1., 2.}, options).reshape({2, 2})};
  const auto targets = torch::tensor({0., 4., 1., 2.}, options).reshape({2, 2});
  const auto metrics = evaluate_intervals(bounds, targets);
  close(metrics.coverage, 0.75, "coverage includes both endpoints and degenerate interval");
  close(metrics.mean_width, 1.75, "interval mean width");
  close(metrics.coverage_by_horizon, torch::tensor({1., 0.5}, options), "coverage by horizon");
  close(metrics.width_by_horizon, torch::tensor({2., 1.5}, options), "width by horizon");
}

void exchangeable_rank_example(const torch::Device& device) {
  const auto options = torch::TensorOptions().device(device).dtype(torch::kFloat64);
  // Enumerate every possible test assignment from seven exchangeable distinct
  // scores. N=6, alpha=.3 gives rank 5 and exactly 5/7 marginal coverage.
  // Both horizons are perfectly dependent; they still use N=6, not N*H=12.
  int covered = 0;
  for (int64_t held_out = 1; held_out <= 7; ++held_out) {
    std::vector<double> values;
    for (int64_t value = 1; value <= 7; ++value) {
      if (value != held_out) {
        values.push_back(static_cast<double>(value));
        values.push_back(static_cast<double>(value * 100));
      }
    }
    const PredictionInterval point{torch::zeros({6, 2}, options), torch::zeros({6, 2}, options)};
    const auto fit = fit_cqr(point, torch::tensor(values, options).reshape({6, 2}), 0.3);
    const auto prediction = apply_cqr({torch::zeros({1, 2}, options), torch::zeros({1, 2}, options)}, fit);
    const auto test = torch::tensor({static_cast<double>(held_out), static_cast<double>(100 * held_out)}, options)
                          .reshape({1, 2});
    covered += evaluate_intervals(prediction, test).coverage == 1.0 ? 1 : 0;
  }
  require(covered == 5, "exhaustive exchangeable assignment has the expected finite-sample coverage");
}

void preservation_and_noncontiguous(const torch::Device& device) {
  const auto options = torch::TensorOptions().device(device).dtype(torch::kFloat64);
  auto lower = torch::zeros({2, 6}, options).transpose(0, 1);
  auto upper = torch::ones({2, 6}, options).transpose(0, 1);
  auto targets = torch::full({2, 6}, 3., options).transpose(0, 1);
  lower.set_requires_grad(true);
  upper.set_requires_grad(true);
  targets.set_requires_grad(true);
  require(!lower.is_contiguous(), "fixture uses noncontiguous bounds");
  auto calibration = fit_cqr({lower, upper}, targets, 0.2);
  require(!calibration.correction.requires_grad(), "fit has no autograd history");
  calibration.correction.set_requires_grad(true);
  auto applied = apply_cqr({lower, upper}, calibration);
  auto metrics = evaluate_intervals({lower, upper}, targets);
  for (const auto& value : {applied.lower, applied.upper, metrics.coverage_by_horizon,
                            metrics.width_by_horizon}) {
    require(!value.requires_grad(), "calibration and metrics do not retain graphs");
  }
  require(!lower.grad().defined() && !upper.grad().defined() && !targets.grad().defined() &&
              !calibration.correction.grad().defined(), "no input gradients are populated");
  applied.lower.fill_(99.);
  applied.upper.fill_(99.);
  metrics.coverage_by_horizon.fill_(99.);
  close(lower.detach(), torch::zeros_like(lower), "bounds are unchanged and do not alias results");
  close(upper.detach(), torch::ones_like(upper), "upper bounds are preserved");
  close(targets.detach(), torch::full_like(targets, 3.), "targets are preserved");
  close(calibration.correction.detach(), torch::full({2}, 2., options), "correction is preserved");
  {
    torch::NoGradGuard no_grad;
    calibration.correction.fill_(50.);
  }
  close(lower.detach(), torch::zeros_like(lower), "fit correction does not alias inputs");
}

void invalid_inputs(const torch::Device& device) {
  const auto options = torch::TensorOptions().device(device).dtype(torch::kFloat32);
  const auto lower = torch::zeros({5, 2}, options), upper = torch::ones({5, 2}, options);
  const auto targets = torch::zeros({5, 2}, options);
  const auto fit = [&](const PredictionInterval& interval, const torch::Tensor& target) {
    fit_cqr(interval, target, 0.2);
  };
  for (const auto& invalid : std::vector<torch::Tensor>{
           {}, lower.flatten(), torch::zeros({0, 2}, options), torch::zeros({5, 0}, options),
           lower.to(torch::kInt64), lower.to(torch::kFloat16), lower.to_sparse(),
           torch::full_like(lower, std::numeric_limits<double>::quiet_NaN()),
           torch::full_like(lower, std::numeric_limits<double>::infinity())}) {
    rejects([&] { fit({invalid, upper}, targets); }, "invalid lower bounds rejected");
    rejects([&] { fit({lower, invalid}, targets); }, "invalid upper bounds rejected");
    rejects([&] { fit({lower, upper}, invalid); }, "invalid targets rejected");
  }
  for (const auto& mismatch : std::vector<torch::Tensor>{
           torch::zeros({1, 2}, options), torch::zeros({5, 1}, options), lower.to(torch::kFloat64)}) {
    rejects([&] { fit({lower, mismatch}, targets); }, "no bound shape or dtype broadcasting");
    rejects([&] { fit({lower, upper}, mismatch); }, "no target shape or dtype broadcasting");
  }
  for (double alpha : {-1., 0., 1., 2., std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::quiet_NaN()}) {
    rejects([&] { fit_cqr({lower, upper}, targets, alpha); }, "invalid miscoverage rejected");
  }
  const auto calibration = fit_cqr({lower, upper}, targets, 0.2);
  const PredictionInterval crossed{upper + 1., upper};
  rejects([&] { fit(crossed, targets); }, "fit rejects inverted endpoints without sorting");
  rejects([&] { apply_cqr(crossed, calibration); }, "apply rejects inverted endpoints without sorting");
  rejects([&] { evaluate_intervals(crossed, targets); }, "metrics reject inverted endpoints");
  for (const auto& bad_correction : std::vector<torch::Tensor>{
           {}, torch::zeros({1, 2}, options), torch::zeros({0}, options), torch::zeros({3}, options),
           torch::zeros({2}, options.dtype(torch::kInt64)), torch::zeros({2}, options.dtype(torch::kFloat64)),
           torch::zeros({2}, options).to_sparse(), torch::full({2}, -1., options),
           torch::full({2}, std::numeric_limits<double>::infinity(), options),
           torch::full({2}, std::numeric_limits<double>::quiet_NaN(), options)}) {
    rejects([&] { apply_cqr({lower, upper}, {bad_correction, 0.2, 5}); }, "malformed correction rejected");
  }
  for (int64_t count : {int64_t{0}, int64_t{-1}, std::numeric_limits<int64_t>::max()}) {
    rejects([&] { apply_cqr({lower, upper}, {calibration.correction, 0.2, count}); },
            "invalid calibration sample count rejected");
  }
  rejects([&] { apply_cqr({lower, upper}, {calibration.correction, 0.01, 5}); },
          "invalid finite-sample metadata rejected when applying");
  rejects([&] { apply_cqr({lower, upper}, {calibration.correction, 1.0, 5}); },
          "invalid miscoverage metadata rejected when applying");
  if (device.is_cuda()) {
    rejects([&] { fit({lower, upper.cpu()}, targets); }, "mismatched endpoint device rejected");
    rejects([&] { fit({lower, upper}, targets.cpu()); }, "mismatched target device rejected");
    rejects([&] { apply_cqr({lower, upper}, {calibration.correction.cpu(), 0.2, 5}); },
            "mismatched correction device rejected");
  }
}

void overflow(const torch::Device& device) {
  const auto options = torch::TensorOptions().device(device).dtype(torch::kFloat32);
  const auto negative = torch::full({5, 1}, -3e38, options);
  const auto positive = torch::full({5, 1}, 3e38, options);
  rejects([&] { fit_cqr({negative, negative}, positive, 0.2); }, "overflowed scores rejected");
  rejects([&] { apply_cqr({positive, positive}, {torch::full({1}, 3e38, options), 0.2, 5}); },
          "overflowed calibrated endpoints rejected");
  rejects([&] { evaluate_intervals({negative, positive}, torch::zeros_like(negative)); },
          "overflowed widths rejected");
}
}  // namespace

int main(int argc, char** argv) {
  torch::Device device(torch::kCPU);
  if (argc == 3 && std::string(argv[1]) == "--device") {
    const std::string requested(argv[2]);
    if (requested == "cuda") {
      if (!torch::cuda::is_available()) {
        std::cout << "SKIP: CUDA requested, but no CUDA device is available\n";
        return 77;
      }
      device = torch::Device(torch::kCUDA, 0);
    } else if (requested != "cpu") {
      std::cerr << "Usage: calibration_test [--device cpu|cuda]\n";
      return 2;
    }
  } else if (argc != 1) {
    std::cerr << "Usage: calibration_test [--device cpu|cuda]\n";
    return 2;
  }
  torch::set_num_threads(1);
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"finite-sample order statistics by horizon", [&] { order_statistics(device); }},
      {"outward-only calibration", [&] { outward_only(device); }},
      {"adjacent miscoverage rank boundaries", [&] { adjacent_miscoverage_ranks(device); }},
      {"floating point outward rounding", [&] { floating_point_outward_rounding(device); }},
      {"separate interval metrics", [&] { interval_metrics(device); }},
      {"exhaustive exchangeable rank example", [&] { exchangeable_rank_example(device); }},
      {"input preservation and noncontiguous tensors", [&] { preservation_and_noncontiguous(device); }},
      {"input and calibration validation", [&] { invalid_inputs(device); }},
      {"arithmetic overflow validation", [&] { overflow(device); }},
  };
  int failures = 0;
  for (const auto& test : tests) {
    try {
      test.second();
      std::cout << "PASS: " << test.first << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL: " << test.first << ": " << error.what() << '\n';
    }
  }
  std::cout << tests.size() - failures << '/' << tests.size() << " tests passed on " << device << '\n';
  return failures == 0 ? 0 : 1;
}
