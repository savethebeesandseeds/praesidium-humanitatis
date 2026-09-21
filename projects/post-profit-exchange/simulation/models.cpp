// SPDX-License-Identifier: MIT
#include "models.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace ph::exchange::models {
namespace {
constexpr Amount bps = 10000;
constexpr Amount ppm = 1000000;
constexpr Amount exp_scale = 1000000000;
[[noreturn]] void bad(const std::string& message) { throw std::invalid_argument(message); }
void require(bool condition, const std::string& message) { if (!condition) bad(message); }
void fields(const Json& object, std::initializer_list<const char*> keys, const char* label) {
  require(object.is_object() && object.size() == keys.size(), std::string(label) + " has missing or unknown fields");
  for (const auto* key : keys) require(object.contains(key), std::string(label) + " missing " + key);
}
Amount integer(const Json& value, Amount lo, Amount hi, const char* label) {
  require(value.is_number_integer(), std::string(label) + " must be an integer");
  require(!value.is_number_unsigned() || value.get<std::uint64_t>() <= static_cast<std::uint64_t>(hi),
          std::string(label) + " exceeds its bound");
  const auto n = value.get<Amount>();
  require(n >= lo && n <= hi, std::string(label) + " outside supported bounds");
  return n;
}
bool valid_sku(const std::string& sku) {
  return !sku.empty() && sku.size() <= 64 && std::all_of(sku.begin(), sku.end(),
      [](unsigned char c) { return c >= 32 && c != 127; });
}
double opportunity_units(const ConsumerConfig& c) {
  return static_cast<double>(c.potential_visitors) * c.visit_probability_bps / bps *
      c.need_probability_bps / bps * (c.max_units_per_product + 1) / 2;
}
Amount reference_probability(const ConsumerConfig& c, const ProductPrior& p) {
  const Amount denominator = static_cast<Amount>(c.potential_visitors) * c.visit_probability_bps *
      c.need_probability_bps * (c.max_units_per_product + 1);
  if (p.base_demand == 0 || denominator == 0) return 0;
  const Amount numerator = static_cast<Amount>(p.base_demand) * 2 * bps * bps * ppm;
  return std::clamp<Amount>((numerator + denominator / 2) / denominator, 1, ppm - 1);
}
Amount ratio_ppm(Amount numerator, Amount denominator) {
  Amount value = numerator / denominator, remainder = numerator % denominator;
  // Six base-ten long-division digits keep intermediates below 1e16 rather
  // than multiplying a possibly 1e15 numerator by a million.
  for (int k = 0; k < 6; ++k) {
    remainder *= 10; value = value * 10 + remainder / denominator; remainder %= denominator;
  }
  return value + (remainder * 2 >= denominator ? 1 : 0);
}
// Integer approximation of exp(-x), x in millionths and limited to [0,20].
// Taylor-expand at x/32 <= 0.625, then square five times. All products fit
// int64_t, and rounding is independent of native/WASM transcendental libraries.
Amount negative_exp(Amount x) {
  x = std::min<Amount>(x, 20 * ppm);
  const Amount reduced = (x * 1000 + 16) / 32;
  Amount term = exp_scale, sum = exp_scale;
  for (Amount k = 1; k <= 12; ++k) {
    term = -(term * reduced) / (exp_scale * k);
    sum += term;
  }
  sum = std::clamp<Amount>(sum, 0, exp_scale);
  for (int k = 0; k < 5; ++k) sum = (sum * sum + exp_scale / 2) / exp_scale;
  return sum;
}
Amount choice_probability(const ConsumerConfig& c, const ProductPrior& p, Amount price) {
  const Amount prior = reference_probability(c, p);
  if (!prior) return 0;
  const Amount change = price - p.reference_price;
  const Amount x = std::min<Amount>(20 * ppm,
      std::abs(change) * p.elasticity_bps * ppm / (p.reference_price * c.choice_scale_bps));
  const Amount decay = negative_exp(x);
  // logit(P) = logit(P_reference) - slope/scale * (price/reference - 1).
  // Work with odds to avoid a logarithm and preserve paired discrete choices.
  const Amount numerator = change >= 0 ? prior * decay : prior * exp_scale;
  const Amount denominator = change >= 0
      ? numerator + (ppm - prior) * exp_scale
      : numerator + (ppm - prior) * decay;
  return std::clamp<Amount>(ratio_ppm(numerator, denominator), 0, ppm);
}
std::uint32_t mix(std::uint32_t value) {
  value ^= value >> 16; value *= 0x7feb352du;
  value ^= value >> 15; value *= 0x846ca68bu;
  return value ^ (value >> 16);
}
std::uint32_t sku_hash(const std::string& sku) {
  std::uint32_t hash = 2166136261u;
  for (const unsigned char c : sku) { hash ^= c; hash *= 16777619u; }
  return hash;
}
std::uint32_t draw(std::uint32_t seed, int day, int customer, std::uint32_t sku, std::uint32_t event) {
  // Ordered, domain-separated mixing avoids a day/event exchange producing
  // the same key, as would happen with a commutative XOR of independent hashes.
  auto state = mix(seed ^ 0x9e3779b9u);
  state = mix(state ^ static_cast<std::uint32_t>(day) ^ 0x243f6a88u);
  state = mix(state ^ static_cast<std::uint32_t>(customer) ^ 0x85a308d3u);
  state = mix(state ^ sku ^ 0x13198a2eu);
  return mix(state ^ event ^ 0x03707344u);
}
bool yes(std::uint32_t random, Amount numerator, Amount denominator) {
  return static_cast<std::uint64_t>(random) * static_cast<std::uint64_t>(denominator) <
      static_cast<std::uint64_t>(numerator) * (std::uint64_t{1} << 32);
}
Amount bounded(std::uint32_t random, Amount width) {
  return static_cast<Amount>((static_cast<std::uint64_t>(random) * static_cast<std::uint64_t>(width)) >> 32);
}
void validate_response(double response) {
  require(std::isfinite(response) && response > 0 && response <= 1000000,
          "price response must be finite in (0,1000000]");
}
ph::exponential_smoothing::EwmaConfig forecasting_config(const ForecastConfig& config) {
  validate(config);
  return {config.alpha_bps, config.sigma_multiplier_bps, config.min_history, config.prior_sigma_units};
}
template<class Value> Json optional_json(const std::optional<Value>& value) {
  return value ? Json(*value) : Json(nullptr);
}
Json metrics_json(const ph::exponential_smoothing::ErrorMetrics& metrics) {
  return {{"evaluations", metrics.evaluations}, {"mae", optional_json(metrics.mae)},
      {"rmse", optional_json(metrics.rmse)}, {"band_coverage", optional_json(metrics.band_coverage)}};
}
}  // namespace

void validate(const ConsumerConfig& c) {
  require(c.potential_visitors >= 0 && c.potential_visitors <= 10000, "potential_visitors must be 0..10000");
  require(c.visit_probability_bps >= 0 && c.visit_probability_bps <= bps, "visit_probability_bps must be 0..10000");
  require(c.budget_min >= 0 && c.budget_max >= c.budget_min && c.budget_max <= 100000000,
          "consumer budgets must be ordered in 0..100000000 minor units");
  require(c.need_probability_bps >= 0 && c.need_probability_bps <= bps, "need_probability_bps must be 0..10000");
  require(c.max_units_per_product >= 1 && c.max_units_per_product <= 10, "max_units_per_product must be 1..10");
  require(c.choice_scale_bps >= 1 && c.choice_scale_bps <= 30000, "choice_scale_bps must be 1..30000");
}
void validate(const ForecastConfig& c) {
  require(c.model == "ewma", "forecast.model must be ewma");
  require(c.alpha_bps >= 1 && c.alpha_bps <= bps, "alpha_bps must be 1..10000");
  require(c.sigma_multiplier_bps >= 0 && c.sigma_multiplier_bps <= 100000,
          "sigma_multiplier_bps must be 0..100000");
  require(c.min_history >= 1 && c.min_history <= 3650, "min_history must be 1..3650");
  require(std::isfinite(c.prior_sigma_units) && c.prior_sigma_units >= 0 && c.prior_sigma_units <= 1000000,
          "prior_sigma_units must be finite in 0..1000000");
  require(c.reserve_horizon_days >= 1 && c.reserve_horizon_days <= 365,
          "reserve_horizon_days must be 1..365");
}
void validate(const ProductPrior& p) {
  require(valid_sku(p.sku), "SKU requires 1..64 bytes without controls");
  require(p.reference_price >= 1 && p.reference_price <= 1000000, "reference_price must be 1..1000000");
  require(p.base_demand >= 0 && p.base_demand <= 10000, "base_demand must be 0..10000");
  require(p.elasticity_bps >= 0 && p.elasticity_bps <= 30000, "elasticity_bps must be 0..30000");
}
ConsumerConfig consumer_config(const Json& object) {
  fields(object, {"potential_visitors", "visit_probability_bps", "budget_min", "budget_max",
      "need_probability_bps", "max_units_per_product", "choice_scale_bps"}, "consumers");
  ConsumerConfig c;
  c.potential_visitors = static_cast<int>(integer(object.at("potential_visitors"), 0, 10000, "potential_visitors"));
  c.visit_probability_bps = static_cast<int>(integer(object.at("visit_probability_bps"), 0, 10000, "visit_probability_bps"));
  c.budget_min = integer(object.at("budget_min"), 0, 100000000, "budget_min");
  c.budget_max = integer(object.at("budget_max"), 0, 100000000, "budget_max");
  c.need_probability_bps = static_cast<int>(integer(object.at("need_probability_bps"), 0, 10000, "need_probability_bps"));
  c.max_units_per_product = static_cast<int>(integer(object.at("max_units_per_product"), 1, 10, "max_units_per_product"));
  c.choice_scale_bps = static_cast<int>(integer(object.at("choice_scale_bps"), 1, 30000, "choice_scale_bps"));
  validate(c); return c;
}
ForecastConfig forecast_config(const Json& object) {
  fields(object, {"model", "alpha_bps", "sigma_multiplier_bps", "min_history", "prior_sigma_units",
      "reserve_horizon_days"}, "forecast");
  require(object.at("model").is_string(), "forecast.model must be a string");
  require(object.at("prior_sigma_units").is_number(), "prior_sigma_units must be a number");
  ForecastConfig c;
  c.model = object.at("model").get<std::string>();
  c.alpha_bps = static_cast<int>(integer(object.at("alpha_bps"), 1, 10000, "alpha_bps"));
  c.sigma_multiplier_bps = static_cast<int>(integer(object.at("sigma_multiplier_bps"), 0, 100000, "sigma_multiplier_bps"));
  c.min_history = static_cast<int>(integer(object.at("min_history"), 1, 3650, "min_history"));
  c.prior_sigma_units = object.at("prior_sigma_units").get<double>();
  c.reserve_horizon_days = static_cast<int>(integer(object.at("reserve_horizon_days"), 1, 365, "reserve_horizon_days"));
  validate(c); return c;
}
Json to_json(const ConsumerConfig& c) {
  return {{"potential_visitors", c.potential_visitors}, {"visit_probability_bps", c.visit_probability_bps},
      {"budget_min", c.budget_min}, {"budget_max", c.budget_max}, {"need_probability_bps", c.need_probability_bps},
      {"max_units_per_product", c.max_units_per_product}, {"choice_scale_bps", c.choice_scale_bps}};
}
Json to_json(const ForecastConfig& c) {
  return {{"model", c.model}, {"alpha_bps", c.alpha_bps}, {"sigma_multiplier_bps", c.sigma_multiplier_bps},
      {"min_history", c.min_history}, {"prior_sigma_units", c.prior_sigma_units}, {"reserve_horizon_days", c.reserve_horizon_days}};
}
double purchase_probability(const ConsumerConfig& c, const ProductPrior& p, Amount price) {
  validate(c); validate(p); require(price > 0 && price <= 1000000, "price must be 1..1000000");
  return static_cast<double>(choice_probability(c, p, price)) / ppm;
}
double price_response_factor(const ConsumerConfig& c, const ProductPrior& p, Amount price) {
  validate(c); validate(p); require(price > 0 && price <= 1000000, "price must be 1..1000000");
  const Amount reference = reference_probability(c, p);
  // No positive demand can be identified from a zero-opportunity prior. Retain
  // a neutral normalization in that case; prior diagnostics disclose it.
  if (!reference) return 1;
  return static_cast<double>(std::max<Amount>(1, choice_probability(c, p, price))) / reference;
}
Json consumer_prior(const ConsumerConfig& c, const ProductPrior& p) {
  validate(c); validate(p);
  const double probability = static_cast<double>(reference_probability(c, p)) / ppm;
  Json valuation = nullptr;
  if (probability > 0 && probability < 1 && p.elasticity_bps > 0)
    valuation = p.reference_price + static_cast<double>(p.reference_price) * c.choice_scale_bps /
        p.elasticity_bps * std::log(probability / (1 - probability));
  return {{"sku", p.sku}, {"source", "declared_cold_start_prior_no_empirical_fit"},
      {"reference_purchase_probability", probability}, {"opportunity_units_before_choice", opportunity_units(c)},
      {"implied_reference_units_before_budget_and_stock", opportunity_units(c) * probability},
      {"requested_reference_units", p.base_demand},
      {"prior_exceeds_opportunities", p.base_demand > opportunity_units(c)},
      {"implied_valuation_minor_units", valuation},
      {"probability_resolution", "one_millionth"}, {"normalization_probability_floor", 0.000001}};
}

ConsumerDay consumers_day(const ConsumerConfig& c, const std::vector<ProductPrior>& products,
                          const std::vector<Amount>& prices, const std::vector<Amount>& stock,
                          std::uint32_t seed, int day, int demand_factor_bps) {
  validate(c);
  require(!products.empty() && products.size() <= 12 && products.size() == prices.size() && products.size() == stock.size(),
          "consumer products/prices/stock require matching lengths in 1..12");
  require(day >= -1000000 && day <= 1000000, "consumer day outside supported bounds");
  require(demand_factor_bps >= 0 && demand_factor_bps <= 30000, "demand factor must be 0..30000");
  ConsumerDay result; result.potential_visitors = c.potential_visitors;
  std::set<std::string> skus;
  std::vector<std::uint32_t> hashes;
  std::vector<Amount> probabilities;
  std::vector<Amount> remaining = stock;
  for (std::size_t i = 0; i < products.size(); ++i) {
    validate(products[i]); require(skus.insert(products[i].sku).second, "duplicate consumer SKU");
    require(prices[i] > 0 && prices[i] <= 1000000 && stock[i] >= 0 && stock[i] <= 1000000,
            "consumer price or stock outside supported bounds");
    ProductConsumption entry; entry.sku = products[i].sku;
    entry.purchase_probability_ppm = static_cast<int>(choice_probability(c, products[i], prices[i]));
    result.products.push_back(entry);
    hashes.push_back(sku_hash(products[i].sku)); probabilities.push_back(entry.purchase_probability_ppm);
  }
  const Amount visits = std::min<Amount>(bps, (static_cast<Amount>(c.visit_probability_bps) * demand_factor_bps + bps / 2) / bps);
  result.effective_visit_probability_bps = static_cast<int>(visits);
  for (int customer = 0; customer < c.potential_visitors; ++customer) {
    if (!yes(draw(seed, day, customer, 0, 1), visits, bps)) { ++result.no_visit; continue; }
    ++result.visitors;
    Amount budget = c.budget_min + bounded(draw(seed, day, customer, 0, 2), c.budget_max - c.budget_min + 1);
    result.opening_budget += budget;
    bool purchased = false;
    for (std::size_t i = 0; i < products.size(); ++i) {
      auto& row = result.products[i];
      if (!yes(draw(seed, day, customer, hashes[i], 3), c.need_probability_bps, bps)) continue;
      ++row.need_customers;
      if (!yes(draw(seed, day, customer, hashes[i], 4), probabilities[i], ppm)) { ++row.no_purchase_customers; continue; }
      const Amount wanted = 1 + bounded(draw(seed, day, customer, hashes[i], 5), c.max_units_per_product);
      const Amount affordable = std::min(wanted, budget / prices[i]);
      const Amount sold = std::min(affordable, remaining[i]);
      row.desired_units += wanted; row.budget_rejected_units += wanted - affordable;
      row.requested_units += affordable; row.sales_units += sold; row.stock_lost_units += affordable - sold;
      const Amount paid = sold * prices[i];
      row.revenue += paid; remaining[i] -= sold; budget -= paid; result.spending += paid;
      purchased = purchased || sold > 0;
    }
    if (!purchased) ++result.no_purchase;
    result.remaining_budget += budget;
  }
  return result;
}
Json to_json(const ConsumerDay& day) {
  Json rows = Json::array();
  for (const auto& p : day.products) rows.push_back({{"sku", p.sku}, {"purchase_probability_ppm", p.purchase_probability_ppm}, {"need_customers", p.need_customers},
      {"no_purchase_customers", p.no_purchase_customers}, {"desired_units", p.desired_units},
      {"budget_rejected_units", p.budget_rejected_units}, {"requested_units", p.requested_units},
      {"sales_units", p.sales_units}, {"stock_lost_units", p.stock_lost_units}, {"revenue", p.revenue}});
  return {{"model", "seeded_visit_need_binary_logit"}, {"effective_visit_probability_bps", day.effective_visit_probability_bps},
      {"potential_visitors", day.potential_visitors},
      {"visitors", day.visitors}, {"no_visit", day.no_visit}, {"no_purchase", day.no_purchase},
      {"opening_budget", day.opening_budget}, {"remaining_budget", day.remaining_budget},
      {"spending", day.spending}, {"products", rows}};
}

EwmaForecaster::EwmaForecaster(ForecastConfig config, double prior_level)
    : forecaster_(forecasting_config(config), prior_level) {}
ForecastPoint EwmaForecaster::predict(double response, int horizon_days) const {
  validate_response(response);
  return forecaster_.predict(response, horizon_days);
}
Json to_json(const ForecastPoint& p) {
  return {{"mean", p.mean}, {"sigma", p.sigma}, {"lower", p.lower}, {"upper", p.upper},
      {"horizon_days", p.horizon_days}, {"horizon_mean", p.horizon_mean}, {"horizon_sigma", p.horizon_sigma},
      {"horizon_lower", p.horizon_lower}, {"horizon_upper", p.horizon_upper},
      {"eligible_history", p.eligible_history}, {"warmed_up", p.warmed_up},
      {"interval_interpretation", "configured_sigma_stress_band_not_guaranteed_coverage"}};
}
Json EwmaForecaster::observe(int day, Amount sales, Amount stock, double response, bool trading) {
  const auto last_day = forecaster_.diagnostics().last_day;
  require((!last_day || day > *last_day) && day >= -1000000 && day <= 1000000,
          "observation days must strictly increase within bounds");
  require(sales >= 0 && sales <= 1000000 && stock >= 0 && stock <= 1000001 && sales <= stock,
          "observed sales must be 0..1000000 and within available stock (uncensored sentinel maximum 1000001)");
  require(trading || sales == 0, "closed observation cannot contain sales");
  validate_response(response);
  require(std::isfinite(static_cast<double>(sales) / response) && static_cast<double>(sales) / response <= 1e12,
          "price-normalized observation exceeds supported units");
  const bool censored = trading && sales == stock;
  const bool eligible = trading && !censored;
  using Status = ph::exponential_smoothing::ObservationStatus;
  const auto update = forecaster_.observe({day, static_cast<double>(sales), response,
      !trading ? Status::closed : censored ? Status::censored : Status::usable});
  return {{"day", day}, {"sales", sales}, {"available_stock", stock}, {"price_response_factor", response},
      {"pre_update_forecast", to_json(update.pre_update_forecast)}, {"eligible", eligible}, {"stock_censored", censored},
      {"reason", !trading ? "closed_no_learning" : censored ? "stock_censored_no_learning" : "uncensored_sales"},
      {"residual", optional_json(update.residual)}, {"covered", optional_json(update.covered)},
      {"post_update_level", update.post_update_level}};
}
Json EwmaForecaster::diagnostics() const {
  const auto d = forecaster_.diagnostics();
  return {{"model", "ewma_price_normalized_sales"}, {"cold_start_source", "declared_prior_no_empirical_fit"},
      {"prior_level", d.prior_level}, {"level", d.level}, {"sigma_normalized_units", d.sigma_normalized_units},
      {"observations", d.observations}, {"eligible_history", d.eligible_history}, {"stock_censored", d.censored}, {"closed", d.closed},
      {"last_day", optional_json(d.last_day)}, {"warmed_up", d.warmed_up},
      {"walk_forward", metrics_json(d.walk_forward)},
      {"post_warmup_walk_forward", metrics_json(d.post_warmup_walk_forward)},
      {"diagnostic_scope", "uncensored_open_sales_only_selection_bias_possible"}};
}
}  // namespace ph::exchange::models
