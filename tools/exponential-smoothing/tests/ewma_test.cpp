// SPDX-License-Identifier: MIT
#include <ph/exponential_smoothing/ewma.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace ph::exponential_smoothing;
int checks = 0;
void check(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
void near(double actual, double expected, const char* message) {
  check(std::abs(actual - expected) <= 1e-12 * std::max(1.0, std::abs(expected)), message);
}
template<class Function> void rejects(Function function, const char* message) {
  bool rejected = false;
  try { function(); } catch (const std::invalid_argument&) { rejected = true; }
  check(rejected, message);
}
void model_checks() {
  // Hand-computable algebra fixtures, not a dataset or claimed evidence.
  EwmaConfig config{5000, 20000, 2, 2};
  EwmaForecaster model(config, 10);
  const auto initial = model.predict();
  near(initial.mean, 10, "cold start is the declared prior");
  near(initial.sigma, 2, "cold start uses declared sigma");
  check(!initial.warmed_up && initial.eligible_history == 0, "no invented cold-start observations");
  auto d = model.diagnostics();
  check(!d.last_day && d.observations == 0, "no history is distinct from zero sales");
  check(!d.walk_forward.mae && !d.walk_forward.rmse && !d.walk_forward.band_coverage,
        "accuracy is absent before observations");
  check(!d.post_warmup_walk_forward.mae, "no invented post-warmup accuracy");

  const auto first = model.observe({-2, 14, 1, ObservationStatus::usable});
  near(first.pre_update_forecast.mean, 10, "score before learning");
  near(*first.residual, 4, "residual compares to the issued forecast");
  near(first.post_update_level, 12, "level is the weighted observation and prior");
  near(model.predict().sigma, std::sqrt(10.0), "sigma updates from pre-update error");
  const auto issued = model.predict(2);
  near(issued.mean, 24, "declared response scales the forecast");
  model.observe({-1, 20, 2, ObservationStatus::usable});
  near(model.predict().mean, 11, "normalize observation before learning");
  near(model.predict().sigma, std::sqrt(7.0), "uncertainty uses normalized errors");
  near(*model.diagnostics().walk_forward.mae, 4, "diagnostics use observed-scale residuals");
  check(model.diagnostics().post_warmup_walk_forward.evaluations == 0,
        "warmup threshold does not retroactively score history");

  model.observe({1, 11, 1, ObservationStatus::usable});
  near(model.predict().sigma, 2, "declared sigma is a floor");
  const auto horizon = model.predict(1, 3);
  near(horizon.horizon_mean, 33, "cumulative horizon mean");
  near(horizon.horizon_sigma, 2 * std::sqrt(7.25), "SES innovation weights determine horizon sigma");
  check(horizon.warmed_up, "eligible observations establish warmup");
  check(model.diagnostics().post_warmup_walk_forward.evaluations == 1,
        "post-warmup score uses pre-observation warmup status");
  const auto before_skip = model.predict();
  const auto censored = model.observe({2, 100, 1, ObservationStatus::censored});
  check(!censored.residual && !censored.covered, "censored value has no accuracy claim");
  model.observe({3, 0, 1, ObservationStatus::closed});
  near(model.predict().mean, before_skip.mean, "skips do not change level");
  near(model.predict().sigma, before_skip.sigma, "skips do not change errors");
  d = model.diagnostics();
  check(d.observations == 5 && d.eligible_history == 3 && d.censored == 1 && d.closed == 1,
        "all observed and skipped records remain inspectable");
  rejects([&] { model.observe({3, 0}); }, "duplicate day rejected");
  rejects([&] { model.observe({0, 0}); }, "earlier day cannot follow future history");
  rejects([&] { model.observe({4, 1, 1, ObservationStatus::closed}); }, "closed period cannot contain a value");
  rejects([&] { model.observe({4, std::numeric_limits<double>::quiet_NaN()}); }, "NaN observation rejected");
  rejects([&] { model.observe({4, 1000001}); }, "observation bound preserved");
  rejects([&] { model.observe({4, 1, 1e-13}); }, "normalization bound preserved");
  rejects([&] { model.predict(0); }, "zero response rejected");
  rejects([&] { model.predict(1, 366); }, "horizon bound preserved");
  check(model.diagnostics().observations == 5 && model.diagnostics().last_day == 3,
        "rejected input does not consume a day or mutate counts");
  model.observe({4, 90});
  near(issued.mean, 24, "future observations cannot alter issued forecasts");
  check(model.predict().mean > before_skip.mean, "new observations affect only subsequent predictions");

  EwmaForecaster zero(config, 0);
  zero.observe({0, 0});
  check(zero.diagnostics().eligible_history == 1, "observed zero is evidence");
  near(zero.predict().lower, 0, "stress band truncates at zero");
}
}  // namespace

int main() {
  try {
    model_checks();
    std::cout << "exponential smoothing EWMA: " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "exponential smoothing EWMA: " << error.what() << '\n';
    return 1;
  }
}
