#include "praesidium-humanitatis/evaluation.h"

#include "praesidium-humanitatis/tft.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace praesidium_humanitatis {

ForecastMetrics evaluate_forecasts(const torch::Tensor& predictions,
                                   const torch::Tensor& targets,
                                   const std::vector<double>& quantiles) {
  torch::NoGradGuard no_grad;
  // Reuse the training loss's shape, layout, type, device and finite validation.
  const auto loss = praesidium_humanitatis::quantile_loss(predictions, targets, quantiles);
  const auto median = std::find(quantiles.begin(), quantiles.end(), 0.5);
  TORCH_CHECK(median != quantiles.end(),
              "Forecast evaluation requires an explicit 0.5 quantile");
  TORCH_CHECK(quantiles.front() < 0.5 && quantiles.back() > 0.5,
              "Forecast evaluation requires quantiles below and above 0.5");
  const auto median_index = static_cast<int64_t>(median - quantiles.begin());
  const auto count = predictions.size(2);

  const auto q = torch::tensor(quantiles, torch::TensorOptions().dtype(torch::kFloat64))
                     .to(predictions.options());
  const auto error = targets.unsqueeze(-1) - predictions;
  const auto pinball = torch::maximum(q * error, (q - 1.0) * error).sum(-1);
  const auto absolute_error = (predictions.select(-1, median_index) - targets).abs();
  const auto lower = predictions.select(-1, 0);
  const auto upper = predictions.select(-1, count - 1);
  const auto covered = ((targets >= lower) & (targets <= upper)).to(predictions.scalar_type());
  const auto crossed = (predictions.narrow(-1, 0, count - 1) >
                        predictions.narrow(-1, 1, count - 1))
                           .any(-1).to(predictions.scalar_type());
  ForecastMetrics metrics{loss.item<double>(),
                          absolute_error.mean().item<double>(),
                          covered.mean().item<double>(),
                          (upper - lower).mean().item<double>(),
                          crossed.mean().item<double>(),
                          pinball.mean(0),
                          absolute_error.mean(0),
                          covered.mean(0)};
  TORCH_CHECK(std::isfinite(metrics.quantile_loss) &&
                  std::isfinite(metrics.median_mae) &&
                  std::isfinite(metrics.interval_coverage) &&
                  std::isfinite(metrics.interval_width) &&
                  std::isfinite(metrics.crossing_rate) &&
                  torch::isfinite(metrics.quantile_loss_by_horizon).all().item<bool>() &&
                  torch::isfinite(metrics.median_mae_by_horizon).all().item<bool>() &&
                  torch::isfinite(metrics.interval_coverage_by_horizon).all().item<bool>(),
              "Forecast metrics overflowed; rescale inputs or use float64 precision");
  return metrics;
}

torch::Tensor persistence_forecast(const torch::Tensor& last_observation,
                                   int64_t horizon, int64_t quantile_count) {
  torch::NoGradGuard no_grad;
  TORCH_CHECK(last_observation.defined(), "Last observations must be defined");
  TORCH_CHECK(last_observation.layout() == torch::kStrided,
              "Last observations must use dense strided layout");
  TORCH_CHECK(last_observation.dim() == 1 && last_observation.size(0) > 0,
              "Last observations must have nonempty shape [batch]");
  TORCH_CHECK(last_observation.scalar_type() == torch::kFloat32 ||
                  last_observation.scalar_type() == torch::kFloat64,
              "Last observations must have float32 or float64 dtype");
  TORCH_CHECK(last_observation.device().is_cpu() || last_observation.device().is_cuda(),
              "Last observations must use a CPU or CUDA device");
  TORCH_CHECK(torch::isfinite(last_observation).all().item<bool>(),
              "Last observations must be finite");
  TORCH_CHECK(horizon > 0 && quantile_count > 0,
              "Persistence horizon and quantile count must be positive");
  TORCH_CHECK(horizon <= std::numeric_limits<int64_t>::max() / quantile_count &&
                  last_observation.size(0) <=
                      std::numeric_limits<int64_t>::max() / (horizon * quantile_count),
              "Persistence forecast dimensions exceed supported range");
  return last_observation.view({-1, 1, 1}).expand({-1, horizon, quantile_count}).clone();
}

}  // namespace praesidium_humanitatis
