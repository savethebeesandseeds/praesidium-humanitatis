// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <limits>
#include <optional>

namespace ph::exponential_smoothing {

// Parameters describe assumptions. They are not fitted from generated data.
struct EwmaConfig {
  int alpha_bps = 2500;
  int sigma_multiplier_bps = 30000;
  int min_history = 7;
  double prior_sigma_units = 3;
};
void validate(const EwmaConfig& config);

struct ForecastPoint {
  double mean = 0;
  double sigma = 0;
  double lower = 0;
  double upper = 0;
  int horizon_days = 1;
  double horizon_mean = 0;
  double horizon_sigma = 0;
  double horizon_lower = 0;
  double horizon_upper = 0;
  std::size_t eligible_history = 0;
  bool warmed_up = false;
};

// The caller decides whether the measured value is fully observed. A stockout,
// for example, makes sales a censored demand observation. The tool does not
// inspect inventory, create missing observations, or consume latent demand.
enum class ObservationStatus { usable, censored, closed };
struct Observation {
  int day = 0;
  double value = 0;
  double response_factor = 1;
  ObservationStatus status = ObservationStatus::usable;
};
struct UpdateResult {
  ForecastPoint pre_update_forecast;
  ObservationStatus status = ObservationStatus::usable;
  std::optional<double> residual;
  std::optional<bool> covered;
  double post_update_level = 0;
};
struct ErrorMetrics {
  std::size_t evaluations = 0;
  std::optional<double> mae;
  std::optional<double> rmse;
  std::optional<double> band_coverage;
};
struct Diagnostics {
  double prior_level = 0;
  double level = 0;
  double sigma_normalized_units = 0;
  std::size_t observations = 0;
  std::size_t eligible_history = 0;
  std::size_t censored = 0;
  std::size_t closed = 0;
  std::optional<int> last_day;
  bool warmed_up = false;
  ErrorMetrics walk_forward;
  ErrorMetrics post_warmup_walk_forward;
};

class EwmaForecaster {
 public:
  EwmaForecaster(EwmaConfig config, double prior_level);
  // Pure read, based only on the declared prior and observations supplied so
  // far. The response multiplier is supplied by the application, not learned.
  ForecastPoint predict(double response_factor = 1, int horizon_days = 1) const;
  // Score the pre-update forecast before learning this observation. Days must
  // increase strictly, including skipped observations; negative history days
  // are supported. Censored and closed observations do not update accuracy.
  UpdateResult observe(const Observation& observation);
  Diagnostics diagnostics() const;

 private:
  EwmaConfig config_;
  double prior_level_ = 0;
  double level_ = 0;
  double error_square_ = 0;
  std::size_t observations_ = 0, eligible_ = 0, censored_ = 0, closed_ = 0;
  std::size_t covered_ = 0, warm_evaluations_ = 0, warm_covered_ = 0;
  double absolute_error_sum_ = 0, squared_error_sum_ = 0;
  double warm_absolute_error_sum_ = 0, warm_squared_error_sum_ = 0;
  int last_day_ = std::numeric_limits<int>::min();
};
}  // namespace ph::exponential_smoothing
