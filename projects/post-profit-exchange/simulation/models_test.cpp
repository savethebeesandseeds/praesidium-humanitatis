// SPDX-License-Identifier: MIT
#include "models.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace ph::exchange::models;
int checks = 0;
void check(bool condition, const std::string& message) {
  ++checks; if (!condition) throw std::runtime_error(message);
}
void near(double a, double b, const std::string& message, double tolerance = 1e-9) {
  check(std::abs(a - b) <= tolerance * std::max(1.0, std::abs(b)), message);
}
template<class Function> void rejects(Function function, const std::string& message) {
  bool rejected = false;
  try { function(); } catch (const std::invalid_argument&) { rejected = true; }
  check(rejected, message);
}
void forecast_checks() {
  ForecastConfig c; c.alpha_bps = 5000; c.min_history = 2; c.prior_sigma_units = 2; c.sigma_multiplier_bps = 20000;
  EwmaForecaster forecast(c, 10);
  const auto initial = forecast.predict();
  near(initial.mean, 10, "cold start uses declared prior");
  near(initial.sigma, 2, "cold-start sigma prior");
  near(initial.lower, 6, "configured two-sigma lower band");
  check(!initial.warmed_up && initial.eligible_history == 0, "warmup is explicit");
  check(forecast.diagnostics().at("walk_forward").at("mae").is_null(), "no fabricated cold-start accuracy");
  auto first = forecast.observe(-2, 14, 100);
  near(first.at("pre_update_forecast").at("mean"), 10, "negative history day scored before learning");
  near(first.at("residual"), 4, "first residual is one-step out-of-sample");
  near(forecast.predict().mean, 12, "EWMA level updates from actual prior observation");
  near(forecast.predict().sigma, std::sqrt(10.0), "sigma learns squared one-step residual");
  const auto before_second = forecast.predict(2);
  near(before_second.mean, 24, "price response scales prior-only forecast");
  forecast.observe(-1, 20, 100, 2);
  near(forecast.predict().mean, 11, "sales are normalized by supplied price response before level update");
  near(forecast.predict().sigma, std::sqrt(7.0), "sigma is on normalized response scale");
  auto diagnostics = forecast.diagnostics();
  near(diagnostics.at("walk_forward").at("mae"), 4, "walk-forward MAE uses actual units at each price");
  near(diagnostics.at("walk_forward").at("rmse"), 4, "walk-forward RMSE independently expected");
  near(diagnostics.at("walk_forward").at("band_coverage"), 1, "coverage uses pre-update interval");
  check(diagnostics.at("post_warmup_walk_forward").at("evaluations") == 0,
        "warmup observations excluded from post-warmup diagnostics");
  forecast.observe(1, 11, 100);
  near(forecast.predict().sigma, 2, "prior sigma is an explicit uncertainty floor");
  const auto horizon = forecast.predict(1, 3);
  near(horizon.horizon_mean, 33, "SES cumulative horizon mean");
  near(horizon.horizon_sigma, 2 * std::sqrt(7.25), "horizon uncertainty includes future level innovations");
  check(horizon.warmed_up, "warmup requires eligible observations");
  const auto before_skips = forecast.predict();
  const auto censored = forecast.observe(2, 100, 100);
  check(censored.at("stock_censored") == true && censored.at("residual").is_null(), "sellout is conservatively censored");
  forecast.observe(3, 0, 0);
  forecast.observe(4, 0, 100, 1, false);
  near(forecast.predict().mean, before_skips.mean, "stockouts/closed periods cannot depress learned demand");
  near(forecast.predict().sigma, before_skips.sigma, "censored values cannot contaminate forecast errors");
  diagnostics = forecast.diagnostics();
  check(diagnostics.at("eligible_history") == 3 && diagnostics.at("stock_censored") == 2 && diagnostics.at("closed") == 1,
        "all skipped observations have inspectable counts");
  check(diagnostics.at("post_warmup_walk_forward").at("evaluations") == 1, "only eligible warmed forecasts enter warm metrics");
  rejects([&] { forecast.observe(4, 0, 100); }, "same day cannot be learned twice");
  rejects([&] { forecast.observe(0, 0, 100); }, "past observation cannot be inserted after later history");
  rejects([&] { forecast.observe(5, 101, 100); }, "sales cannot exceed available stock");
  rejects([&] { forecast.observe(5, 1, 100, 1, false); }, "closed period cannot report sales");
  rejects([&] { forecast.predict(0); }, "zero normalization rejected");
  rejects([&] { forecast.predict(std::numeric_limits<double>::infinity()); }, "infinite response rejected");
  rejects([&] { forecast.predict(1, 366); }, "unbounded horizon rejected");
  const auto frozen = to_json(forecast.predict());
  forecast.observe(5, 90, 100);
  check(frozen.at("mean") == 11, "later observation never mutates an issued forecast");
  check(forecast.predict().mean > frozen.at("mean").get<double>(), "new evidence affects only subsequent forecasts");
  EwmaForecaster zero(c, 0);
  zero.observe(0, 0, 10);
  near(zero.predict().lower, 0, "nonnegative interval truncation");
  check(zero.diagnostics().at("eligible_history") == 1, "uncensored zero sales are learned");
  EwmaForecaster imported(c, 0);
  check(imported.observe(-1, 1000000, 1000001).at("eligible") == true,
        "external uncensored maximum sales permit the internal stock sentinel");
  rejects([&] { imported.observe(0, 1000001, 1000001); }, "stock sentinel does not raise the sales limit");
}
void verify_consumers(const ConsumerDay& day, const std::vector<Amount>& price, const std::vector<Amount>& stock) {
  check(day.visitors + day.no_visit == day.potential_visitors, "visitor conservation");
  check(day.spending + day.remaining_budget == day.opening_budget && day.remaining_budget >= 0, "all consumer budgets reconcile exactly");
  check(day.no_purchase >= 0 && day.no_purchase <= day.visitors, "no-purchase counts only visitors");
  Amount total_revenue = 0;
  for (std::size_t i = 0; i < day.products.size(); ++i) {
    const auto& p = day.products[i];
    check(p.sales_units <= stock[i] && p.sales_units >= 0, "no sale exceeds inventory");
    check(p.sales_units + p.stock_lost_units == p.requested_units, "affordable demand splits into sale and stock loss");
    check(p.requested_units + p.budget_rejected_units == p.desired_units, "desired units split at budget gate");
    check(p.revenue == p.sales_units * price[i], "consumer payments use exact posted integer price");
    check(p.need_customers <= day.visitors && p.no_purchase_customers <= p.need_customers, "need and no-purchase gates conserve customers");
    total_revenue += p.revenue;
  }
  check(total_revenue == day.spending, "supplier revenue equals consumer spending");
}
void consumer_checks() {
  ConsumerConfig c;
  std::vector<ProductPrior> products{{"bread", 200, 20, 12000}, {"beans", 300, 14, 10000}};
  const std::vector<Amount> prices{200, 300}, stock{1000, 1000};
  const auto first = consumers_day(c, products, prices, stock, 42, 1);
  check(to_json(first) == to_json(consumers_day(c, products, prices, stock, 42, 1)), "seed/day replay is exact");
  check(to_json(first) != to_json(consumers_day(c, products, prices, stock, 43, 1)), "different seed changes synthetic consumers");
  check(to_json(first) != to_json(consumers_day(c, products, prices, stock, 42, 2)), "different day uses independent indexed draws");
  near(purchase_probability(c, products[0], 200), 1.0 / 3, "reference prior calibrated before budget/stock", 1e-6);
  near(price_response_factor(c, products[0], 200), 1, "reference normalization is one");
  check(purchase_probability(c, products[0], 100) > purchase_probability(c, products[0], 200) &&
        purchase_probability(c, products[0], 200) > purchase_probability(c, products[0], 300),
        "positive logit slope decreases choice probability with price");
  for (Amount price = 50; price <= 500; price += 10) {
    const double prior = purchase_probability(c, products[0], 200);
    const double expected = 1 / (1 + (1 - prior) / prior * std::exp(
        static_cast<double>(products[0].elasticity_bps) / c.choice_scale_bps * (static_cast<double>(price) / 200 - 1)));
    near(purchase_probability(c, products[0], price), expected, "integer logit approximation matches binary-logit probability", 2e-6);
  }
  for (int day = 1; day <= 50; ++day) {
    auto a = consumers_day(c, products, prices, {11, 7}, 91, day);
    verify_consumers(a, prices, {11, 7});
    auto cheaper = consumers_day(c, products, {150, 250}, {10000, 10000}, 91, day);
    auto dearer = consumers_day(c, products, {250, 350}, {10000, 10000}, 91, day);
    check(cheaper.visitors == dearer.visitors && cheaper.opening_budget == dearer.opening_budget,
          "paired policy prices cannot perturb visitation or budget draws");
    for (std::size_t i = 0; i < products.size(); ++i) {
      check(cheaper.products[i].need_customers == dearer.products[i].need_customers,
            "paired policy prices cannot perturb product needs");
      check(cheaper.products[i].desired_units >= dearer.products[i].desired_units,
            "paired binary choices are monotone before budget competition");
    }
  }
  auto absent = c; absent.visit_probability_bps = 0;
  const auto no_visitors = consumers_day(absent, products, prices, stock, 3, 1);
  check(no_visitors.visitors == 0 && no_visitors.no_visit == c.potential_visitors && no_visitors.spending == 0, "zero visit probability means no customers");
  auto no_need = c; no_need.need_probability_bps = 0; no_need.visit_probability_bps = 10000;
  const auto none = consumers_day(no_need, products, prices, stock, 3, 1);
  check(none.no_purchase == no_need.potential_visitors && none.spending == 0, "outside no-purchase option includes no need");
  auto poor = c; poor.budget_min = 0; poor.budget_max = 0; poor.visit_probability_bps = 10000; poor.need_probability_bps = 10000;
  const auto no_budget = consumers_day(poor, products, prices, stock, 9, 1);
  check(no_budget.spending == 0 && no_budget.products[0].budget_rejected_units > 0, "zero budgets reject desired purchases without debt");
  const auto sold_out = consumers_day(c, products, prices, {0, 0}, 42, 1);
  check(sold_out.spending == 0 && sold_out.products[0].stock_lost_units > 0, "stock losses remain visible without spending money");
  auto flat = products[0]; flat.elasticity_bps = 0;
  near(purchase_probability(c, flat, 1), purchase_probability(c, flat, 1000000), "zero declared slope has price-independent utility before budget");
  auto zero = products[0]; zero.base_demand = 0;
  near(purchase_probability(c, zero, 200), 0, "zero prior base demand explicitly gives no product choice");
  auto impossible = products[0]; impossible.base_demand = 10000;
  check(consumer_prior(c, impossible).at("prior_exceeds_opportunities") == true, "unattainable cold-start demand calibration is disclosed");
  // A new irrelevant SKU does not shift draws of an existing product.
  auto unused = products[1]; unused.base_demand = 0;
  const auto alone = consumers_day(c, {products[0]}, {200}, {1000}, 42, 1);
  const auto prefixed = consumers_day(c, {unused, products[0]}, {300, 200}, {1000, 1000}, 42, 1);
  check(to_json(alone).at("products")[0] == to_json(prefixed).at("products")[1], "SKU-keyed random draws survive insertion of a nonbuying product");
  rejects([&] { consumers_day(c, products, {200}, stock, 42, 1); }, "mismatched price vectors rejected");
  rejects([&] { consumers_day(c, {products[0], products[0]}, prices, stock, 42, 1); }, "duplicate SKU random identity rejected");
  rejects([&] { consumers_day(c, products, {0, 300}, stock, 42, 1); }, "zero selling price rejected");
  rejects([&] { consumers_day(c, products, prices, {-1, 20}, 42, 1); }, "negative stock rejected");
  std::vector<ProductPrior> many;
  for (int i = 0; i < 12; ++i) many.push_back({"item-" + std::to_string(i), 200, 10, 10000});
  const auto twelve = consumers_day(c, many, std::vector<Amount>(12, 200), std::vector<Amount>(12, 10), 8, 1);
  verify_consumers(twelve, std::vector<Amount>(12, 200), std::vector<Amount>(12, 10));
  check(twelve.products.size() == 12, "configured twelve-product application bound supported");
  many.push_back({"item-12", 200, 10, 10000});
  rejects([&] { consumers_day(c, many, std::vector<Amount>(13, 200), std::vector<Amount>(13, 10), 8, 1); },
          "consumer model enforces twelve-product bound");
}
void validation_checks() {
  ConsumerConfig c; ForecastConfig f;
  check(to_json(consumer_config(to_json(c))) == to_json(c), "consumer config round trip");
  check(to_json(forecast_config(to_json(f))) == to_json(f), "forecast config round trip");
  auto json = to_json(c); json["unknown"] = 1;
  rejects([&] { consumer_config(json); }, "unknown consumer configuration rejected");
  json = to_json(c); json["potential_visitors"] = 2.5;
  rejects([&] { consumer_config(json); }, "noninteger visitor count rejected");
  json = to_json(c); json["choice_scale_bps"] = 0;
  rejects([&] { consumer_config(json); }, "zero utility scale rejected");
  json = to_json(c); json["budget_min"] = 3000;
  rejects([&] { consumer_config(json); }, "unordered budget rejected");
  auto fj = to_json(f); fj["alpha_bps"] = 0;
  rejects([&] { forecast_config(fj); }, "zero learning rate rejected");
  fj = to_json(f); fj["model"] = "clairvoyant";
  rejects([&] { forecast_config(fj); }, "unsupported forecasting model rejected");
  fj = to_json(f); fj["prior_sigma_units"] = -1;
  rejects([&] { forecast_config(fj); }, "negative sigma rejected");
  f.prior_sigma_units = std::numeric_limits<double>::quiet_NaN();
  rejects([&] { EwmaForecaster bad_forecast(f, 10); }, "nonfinite direct typed sigma rejected");
}
}  // namespace

int main() {
  try {
    validation_checks(); forecast_checks(); consumer_checks();
    std::cout << "forecast/consumer model checks passed: " << checks << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "model test failure after " << checks << " checks: " << error.what() << '\n';
    return 1;
  }
}
