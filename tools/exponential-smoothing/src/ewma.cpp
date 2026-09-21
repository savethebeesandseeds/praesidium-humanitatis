// SPDX-License-Identifier: MIT
#include <ph/exponential_smoothing/ewma.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ph::exponential_smoothing {
namespace {
constexpr int bps = 10000;
void require(bool condition, const char* message) {
  if (!condition) throw std::invalid_argument(message);
}
void validate_response(double response) {
  require(std::isfinite(response) && response > 0 && response <= 1000000,
          "response factor must be finite in (0,1000000]");
}
ErrorMetrics metrics(double absolute_error_sum, double squared_error_sum,
                     std::size_t covered, std::size_t evaluations) {
  ErrorMetrics result;
  result.evaluations = evaluations;
  if (evaluations) {
    result.mae = absolute_error_sum / evaluations;
    result.rmse = std::sqrt(squared_error_sum / evaluations);
    result.band_coverage = static_cast<double>(covered) / evaluations;
  }
  return result;
}
}  // namespace

void validate(const EwmaConfig& c) {
  require(c.alpha_bps >= 1 && c.alpha_bps <= bps, "alpha_bps must be 1..10000");
  require(c.sigma_multiplier_bps >= 0 && c.sigma_multiplier_bps <= 100000,
          "sigma_multiplier_bps must be 0..100000");
  require(c.min_history >= 1 && c.min_history <= 3650, "min_history must be 1..3650");
  require(std::isfinite(c.prior_sigma_units) && c.prior_sigma_units >= 0 && c.prior_sigma_units <= 1000000,
          "prior_sigma_units must be finite in 0..1000000");
}
EwmaForecaster::EwmaForecaster(EwmaConfig config, double prior_level)
    : config_(config), prior_level_(prior_level), level_(prior_level),
      error_square_(config_.prior_sigma_units * config_.prior_sigma_units) {
  validate(config_);
  require(std::isfinite(prior_level) && prior_level >= 0 && prior_level <= 1000000,
          "prior forecast level must be finite in 0..1000000 units");
}
ForecastPoint EwmaForecaster::predict(double response, int horizon_days) const {
  validate_response(response);
  require(horizon_days >= 1 && horizon_days <= 365, "forecast horizon must be 1..365");
  const double alpha = static_cast<double>(config_.alpha_bps) / bps;
  const double z = static_cast<double>(config_.sigma_multiplier_bps) / bps;
  const double sigma = std::max(config_.prior_sigma_units, std::sqrt(error_square_)) * response;
  ForecastPoint result;
  result.mean = level_ * response; result.sigma = sigma;
  result.lower = std::max(0.0, result.mean - z * sigma); result.upper = result.mean + z * sigma;
  result.horizon_days = horizon_days; result.horizon_mean = result.mean * horizon_days;
  double innovation_weight_sum = 0;
  for (int j = 0; j < horizon_days; ++j) {
    const double weight = 1 + alpha * j; innovation_weight_sum += weight * weight;
  }
  // Additive-innovation SES: cumulative H-step forecast error has coefficients
  // 1,1+alpha,...,1+alpha*(H-1), assuming uncorrelated equal-variance innovations.
  result.horizon_sigma = sigma * std::sqrt(innovation_weight_sum);
  result.horizon_lower = std::max(0.0, result.horizon_mean - z * result.horizon_sigma);
  result.horizon_upper = result.horizon_mean + z * result.horizon_sigma;
  result.eligible_history = eligible_; result.warmed_up = eligible_ >= static_cast<std::size_t>(config_.min_history);
  return result;
}
UpdateResult EwmaForecaster::observe(const Observation& observation) {
  const int day = observation.day;
  const double value = observation.value, response = observation.response_factor;
  const auto status = observation.status;
  require(day > last_day_ && day >= -1000000 && day <= 1000000,
          "observation days must strictly increase within bounds");
  require(std::isfinite(value) && value >= 0 && value <= 1000000,
          "observed value must be finite in 0..1000000");
  require(status == ObservationStatus::usable || status == ObservationStatus::censored || status == ObservationStatus::closed,
          "invalid observation status");
  require(status != ObservationStatus::closed || value == 0, "closed observation cannot contain a nonzero value");
  validate_response(response);
  require(std::isfinite(value / response) && value / response <= 1e12,
          "normalized observation exceeds supported units");
  const auto prediction = predict(response);
  const bool eligible = status == ObservationStatus::usable;
  const double residual = value - prediction.mean;
  const bool covered = value >= prediction.lower && value <= prediction.upper;
  UpdateResult result;
  result.pre_update_forecast = prediction; result.status = status;
  if (eligible) { result.residual = residual; result.covered = covered; }
  ++observations_; last_day_ = day;
  if (status == ObservationStatus::closed) ++closed_;
  else if (status == ObservationStatus::censored) ++censored_;
  else {
    const double normalized = value / response;
    const double normalized_residual = normalized - level_;
    const double alpha = static_cast<double>(config_.alpha_bps) / bps;
    absolute_error_sum_ += std::abs(residual); squared_error_sum_ += residual * residual;
    covered_ += covered ? 1 : 0;
    if (prediction.warmed_up) {
      ++warm_evaluations_; warm_covered_ += covered ? 1 : 0;
      warm_absolute_error_sum_ += std::abs(residual); warm_squared_error_sum_ += residual * residual;
    }
    error_square_ = (1 - alpha) * error_square_ + alpha * normalized_residual * normalized_residual;
    level_ = (1 - alpha) * level_ + alpha * normalized;
    ++eligible_;
  }
  result.post_update_level = level_;
  return result;
}
Diagnostics EwmaForecaster::diagnostics() const {
  Diagnostics result;
  result.prior_level = prior_level_; result.level = level_;
  result.sigma_normalized_units = std::max(config_.prior_sigma_units, std::sqrt(error_square_));
  result.observations = observations_; result.eligible_history = eligible_;
  result.censored = censored_; result.closed = closed_;
  if (observations_) result.last_day = last_day_;
  result.warmed_up = eligible_ >= static_cast<std::size_t>(config_.min_history);
  result.walk_forward = metrics(absolute_error_sum_, squared_error_sum_, covered_, eligible_);
  result.post_warmup_walk_forward = metrics(warm_absolute_error_sum_, warm_squared_error_sum_, warm_covered_, warm_evaluations_);
  return result;
}
}  // namespace ph::exponential_smoothing
