#pragma once

#include <torch/torch.h>

#include <cstdint>
#include <vector>

namespace praesidium_humanitatis {

struct ForecastMetrics {
  // Pinball loss summed over quantiles, averaged over batch and horizon.
  double quantile_loss;
  double median_mae;
  // Inclusive coverage between the first and last predicted quantiles.
  double interval_coverage;
  // Signed upper minus lower width. Crossed endpoints can make this negative.
  double interval_width;
  // Fraction of batch/horizon positions with any adjacent quantile inversion.
  double crossing_rate;
  // [horizon], averaged over batch, on the prediction device and dtype.
  torch::Tensor quantile_loss_by_horizon;
  torch::Tensor median_mae_by_horizon;
  torch::Tensor interval_coverage_by_horizon;
};

// Predictions [batch, horizon, quantiles], targets [batch, horizon]. Both must
// be finite, dense float32/float64 tensors with matching dtype and CPU/CUDA
// device; all dimensions must be nonempty. Quantile levels must be strictly
// increasing in (0, 1), include 0.5 exactly, and span both sides of 0.5.
// Predictions are never sorted: crossing and coverage describe raw outputs.
// Nonfinite derived metrics (including arithmetic overflow) are rejected.
// Evaluation does not construct an autograd graph or modify its inputs.
ForecastMetrics evaluate_forecasts(const torch::Tensor& predictions,
                                   const torch::Tensor& targets,
                                   const std::vector<double>& quantiles);

// Repeat each last observation [batch] over positive horizon and quantile_count.
// This point forecast has degenerate intervals; it is not an uncertainty model.
// Input must be finite, nonempty, dense float32/float64 on CPU or CUDA. The
// returned [batch, horizon, quantile_count] tensor has the same device/dtype,
// owns its storage, and has no autograd history.
torch::Tensor persistence_forecast(const torch::Tensor& last_observation,
                                   int64_t horizon, int64_t quantile_count);

}  // namespace praesidium_humanitatis
