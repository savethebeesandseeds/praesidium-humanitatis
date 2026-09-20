#pragma once

#include <torch/torch.h>

#include <cstdint>

namespace praesidium_humanitatis {

struct PredictionInterval {
  torch::Tensor lower;  // [batch, horizon]
  torch::Tensor upper;  // [batch, horizon]
};

struct CQRCalibration {
  torch::Tensor correction;  // [horizon], nonnegative outward adjustment
  double miscoverage;
  int64_t sample_count;
};

struct IntervalMetrics {
  double coverage;
  double mean_width;
  torch::Tensor coverage_by_horizon;  // [horizon]
  torch::Tensor width_by_horizon;     // [horizon]
};

// Outward-only split conformalized quantile regression (CQR). For each horizon,
// sort max(lower - target, target - upper) across calibration examples and use
// order statistic ceil((N + 1) * (1 - miscoverage)), clamped below at zero.
// Rank N+1 is rejected: finite bounds cannot provide that finite-sample level.
// This conservative variant never shrinks the model's original intervals.
// Rank calculation respects adjacent representable miscoverage values without
// rounding 1-miscoverage first. Positive scores are rounded toward +infinity;
// positive-correction bounds are rounded outward, each by one representable
// step, to prevent floating-point cancellation from losing covered targets.
// Zero corrections preserve endpoints exactly. Nonfinite results are rejected.
//
// The model and all preprocessing/model selection must be fixed before observing
// calibration labels; evaluate on separate held-out examples. The finite-sample
// guarantee is MARGINAL coverage at each fixed horizon under exchangeability of
// calibration and test examples. Horizons within one example may be dependent.
// This is not simultaneous path coverage, conditional coverage, or a guarantee
// for dependent overlapping windows from a single time series or distribution
// shift. Do not pool horizons as if N * horizon were independent examples.
//
// All bounds/targets must have the same nonempty [batch, horizon] shape, be
// finite dense float32/float64, and have matching dtype and CPU/CUDA device.
// Lower bounds must not exceed upper bounds; no sorting repairs predictions.
// Calibration requires enough examples for the requested level. The caller
// must associate calibration with the unchanged model, features and horizon.
// Results own their storage, retain no autograd graph, and never modify inputs.
CQRCalibration fit_cqr(const PredictionInterval& calibration_predictions,
                       const torch::Tensor& targets, double miscoverage);

// Reuse calibration only for the same fixed predictor and horizon meanings.
// Prediction batch size may differ from the calibration batch size; horizon,
// device and dtype must match the calibration correction. No targets are used.
PredictionInterval apply_cqr(const PredictionInterval& predictions,
                             const CQRCalibration& calibration);

// Inclusive coverage and interval width, separately from raw quantile metrics:
// calibrated endpoints are prediction bounds, not calibrated individual quantiles.
IntervalMetrics evaluate_intervals(const PredictionInterval& predictions,
                                   const torch::Tensor& targets);

}  // namespace praesidium_humanitatis
