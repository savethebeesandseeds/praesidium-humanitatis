// SPDX-License-Identifier: MIT
#include "simulation.hpp"
#include "models.hpp"
#include "default_config.hpp"
#include "ph/price/engine.hpp"
#include "ph/price/enumeration.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace ph::exchange {
namespace {
using Json = nlohmann::json;
using Amount = std::int64_t;
constexpr const char* schema = "exchange.sim.v3";
constexpr Amount scale = 10000;
[[noreturn]] void bad(const std::string& message) { throw std::invalid_argument(message); }
void fields(const Json& object, std::initializer_list<const char*> names, const std::string& path) {
  if (!object.is_object() || object.size() != names.size()) bad(path + " has missing or unknown fields");
  for (const auto* name : names) if (!object.contains(name)) bad(path + " missing " + name);
}
Amount number(const Json& value, Amount minimum, Amount maximum, const std::string& path) {
  if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() >
      static_cast<std::uint64_t>(std::numeric_limits<Amount>::max()))) bad(path + " must be an integer");
  const auto result = value.get<Amount>();
  if (result < minimum || result > maximum) bad(path + " must be " + std::to_string(minimum) + ".." + std::to_string(maximum));
  return result;
}
std::string label(const Json& value, const std::string& path) {
  if (!value.is_string()) bad(path + " must be a string");
  const auto result = value.get<std::string>();
  if (result.empty() || result.size() > 64 || !std::all_of(result.begin(), result.end(),
      [](unsigned char c) { return c >= 32 && c != 127; })) bad(path + " needs 1..64 bytes without controls");
  return result;
}
Json parse(const std::string& input) {
  if (input.size() > 8 * 1024 * 1024) bad("command exceeds 8 MiB");
  std::vector<std::set<std::string>> keys;
  auto callback = [&](int, Json::parse_event_t event, Json& value) {
    if (event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start) {
      keys.emplace_back(); if (keys.size() > 24) bad("JSON nesting exceeds 24");
    } else if (event == Json::parse_event_t::key) {
      if (!keys.back().insert(value.get<std::string>()).second) bad("duplicate JSON key");
    } else if (event == Json::parse_event_t::object_end || event == Json::parse_event_t::array_end) keys.pop_back();
    return true;
  };
  return Json::parse(input, callback);
}
Json defaults() { return parse(kExampleRunConfiguration).at("simulation"); }
void validate(const Json& config) {
  fields(config, {"periods", "seed", "currency", "initial_cash", "initial_reserve", "procurement_budget",
    "worker_wages", "operating_cost", "reserve_contribution", "reserve_target", "feedback_recovery_days", "max_price_combinations", "consumers", "forecast", "assurance", "shock", "scenarios", "products"}, "config");
  number(config.at("periods"), 1, 365, "periods"); number(config.at("seed"), 0, 4294967295LL, "seed");
  const auto currency = label(config.at("currency"), "currency");
  if (currency.size() != 3 || !std::all_of(currency.begin(), currency.end(), [](char c) { return c >= 'A' && c <= 'Z'; })) bad("currency needs three uppercase letters");
  for (const auto* key : {"initial_cash", "initial_reserve", "procurement_budget", "operating_cost", "reserve_contribution", "reserve_target"})
    number(config.at(key), 0, 100000000, key);
  number(config.at("worker_wages"), 1, 100000000, "worker_wages");
  number(config.at("max_price_combinations"), 1, 200000, "max_price_combinations");
  (void)models::consumer_config(config.at("consumers"));
  (void)models::forecast_config(config.at("forecast"));
  fields(config.at("assurance"), {"provider", "trigger_buffer_days"}, "assurance");
  if (config.at("assurance").at("provider") != "post_profit_continuity_assurance") bad("unsupported assurance provider contract");
  number(config.at("assurance").at("trigger_buffer_days"), 1, 365, "assurance.trigger_buffer_days");
  number(config.at("feedback_recovery_days"), 1, 30, "feedback_recovery_days");
  if (config.at("initial_reserve") > config.at("initial_cash")) bad("initial_reserve cannot exceed initial_cash");
  const auto& shock = config.at("shock");
  fields(shock, {"start_day", "end_day", "demand_factor_bps", "cost_factor_bps"}, "shock");
  const auto start = number(shock.at("start_day"), 0, 365, "shock.start_day");
  const auto end = number(shock.at("end_day"), 0, 365, "shock.end_day");
  if ((start == 0) != (end == 0) || (start && end < start)) bad("shock uses 0/0 to disable, otherwise ordered inclusive days");
  number(shock.at("demand_factor_bps"), 0, 30000, "shock.demand_factor_bps");
  number(shock.at("cost_factor_bps"), 0, 30000, "shock.cost_factor_bps");
  const auto& scenarios = config.at("scenarios");
  if (!scenarios.is_array() || scenarios.size() != 3) bad("exactly three scenarios required");
  std::set<std::string> scenario_ids;
  double probability_sum = 0;
  for (const auto& scenario : scenarios) {
    fields(scenario, {"id", "sigma_offset_bps", "probability"}, "scenario");
    if (!scenario_ids.insert(label(scenario.at("id"), "scenario.id")).second) bad("duplicate scenario id");
    number(scenario.at("sigma_offset_bps"), -10000, 10000, "scenario.sigma_offset_bps");
    if (!scenario.at("probability").is_number()) bad("probability must be a number");
    const double probability = scenario.at("probability").get<double>();
    if (!std::isfinite(probability) || probability <= 0 || probability > 1) bad("probability must be finite in (0,1]");
    probability_sum += probability;
  }
  if (std::abs(probability_sum - 1) > 1e-9) bad("scenario probabilities must sum to one");
  const auto& products = config.at("products");
  if (!products.is_array() || products.empty() || products.size() > 12) bad("one through twelve products required");
  std::set<std::string> skus;
  for (const auto& product : products) {
    fields(product, {"sku", "label", "unit_cost", "reference_price", "fixed_price", "initial_stock", "target_stock",
      "base_demand", "elasticity_bps", "spoilage_bps", "affordability_ceiling", "max_change_bps", "candidate_prices"}, "product");
    if (!skus.insert(label(product.at("sku"), "sku")).second) bad("duplicate sku");
    label(product.at("label"), "label");
    number(product.at("unit_cost"), 0, 1000000, "unit_cost");
    for (const auto* key : {"reference_price", "fixed_price", "affordability_ceiling"}) number(product.at(key), 1, 1000000, key);
    for (const auto* key : {"initial_stock", "target_stock"}) number(product.at(key), 0, 100000, key);
    number(product.at("base_demand"), 0, 10000, "base_demand");
    number(product.at("elasticity_bps"), 0, 30000, "elasticity_bps");
    number(product.at("spoilage_bps"), 0, 10000, "spoilage_bps");
    number(product.at("max_change_bps"), 0, 10000, "max_change_bps");
    const auto& candidates = product.at("candidate_prices");
    if (!candidates.is_array() || candidates.empty() || candidates.size() > 16) bad("one through sixteen candidate prices required");
    std::set<Amount> prices;
    for (const auto& price : candidates) if (!prices.insert(number(price, 1, 1000000, "candidate_price")).second) bad("duplicate candidate price");
    if (!prices.count(product.at("reference_price").get<Amount>())) bad("candidate_prices must include reference_price for the initial hold decision");
    if (product.at("reference_price") > product.at("affordability_ceiling") || product.at("fixed_price") > product.at("affordability_ceiling"))
      bad("initial and fixed public prices must respect the affordability ceiling");
  }
}
Amount value(const Json& object, const char* name) { return object.at(name).get<Amount>(); }
Amount rounded(Amount numerator, Amount denominator = scale) { return (numerator + denominator / 2) / denominator; }
Amount feedback_adjustment(Amount balance, Amount days) {
  if (!balance) return 0;
  const Amount adjusted = std::max<Amount>(1, rounded(std::abs(balance), days));
  return balance < 0 ? -adjusted : adjusted;
}
struct Lot { Amount units; Amount cost; Amount acquired_day; };
struct Stock {
  std::deque<Lot> lots;
  Amount units() const { Amount n = 0; for (const auto& lot : lots) n += lot.units; return n; }
  Amount worth() const { Amount n = 0; for (const auto& lot : lots) n += lot.units * lot.cost; return n; }
  void add(Amount units, Amount cost, Amount day = 0) { if (units) lots.push_back({units, cost, day}); }
  Json records() const {
    Json result = Json::array();
    for (const auto& lot : lots) result.push_back({{"units", lot.units}, {"unit_cost", lot.cost}, {"acquired_day", lot.acquired_day}});
    return result;
  }
  Amount remove(Amount units) {
    Amount cost = 0;
    while (units) {
      if (lots.empty()) throw std::logic_error("inventory underflow");
      auto& lot = lots.front(); const auto count = std::min(units, lot.units);
      cost += count * lot.cost; units -= count; lot.units -= count;
      if (!lot.units) lots.pop_front();
    }
    return cost;
  }
};
std::string status(ph::price::SolveStatus value) {
  using S = ph::price::SolveStatus;
  switch (value) {
    case S::recommended: return "recommended";
    case S::infeasible: return "infeasible";
    case S::invalid_input: return "invalid_input";
    case S::unavailable: return "unavailable";
    case S::rejected_solution: return "rejected_solution";
    case S::solver_failed: return "solver_failed";
  }
  return "solver_failed";
}
models::ProductPrior prior(const Json& product) {
  return {product.at("sku").get<std::string>(), value(product, "reference_price"),
      static_cast<int>(value(product, "base_demand")), static_cast<int>(value(product, "elasticity_bps"))};
}
Json empty_history() { return {{"schema_version", "exchange.history.v1"}, {"observations", Json::array()}}; }
// Explain one product's candidate at a time against the published basket.
// This is forecast evidence, including for infeasible/continuity prices; it
// neither selects prices nor changes any accounting or optimizer score.
void explain_prices(const ph::price::Request& request, const std::vector<Amount>& prices,
                    Json& product_rows) {
  std::vector<std::size_t> published_indices;
  std::vector<Amount> basket_contribution(request.scenarios.size(), 0);
  for (std::size_t i = 0; i < request.products.size(); ++i) {
    const auto& product = request.products[i];
    const auto published = std::find_if(product.candidates.begin(), product.candidates.end(),
        [&](const ph::price::Candidate& candidate) { return candidate.price == prices[i]; });
    if (published == product.candidates.end()) throw std::logic_error("published price has no forecast candidate");
    published_indices.push_back(static_cast<std::size_t>(published - product.candidates.begin()));
    for (std::size_t s = 0; s < request.scenarios.size(); ++s)
      basket_contribution[s] += (published->price - product.unit_cost) * published->forecast_units[s];
  }
  const Amount required = request.worker_wage_floor + request.operating_cost + request.reserve_floor;
  for (std::size_t i = 0; i < request.products.size(); ++i) {
    const auto& product = request.products[i];
    const auto& published = product.candidates[published_indices[i]];
    auto indices = published_indices;
    for (std::size_t k = 0; k < product.candidates.size(); ++k) {
      const auto& candidate = product.candidates[k];
      indices[i] = k;
      // Reuse the authoritative exact checks, including the affordable-offer
      // guard and whole-exchange coverage; the UI must not reimplement policy.
      const auto checked = ph::price::evaluate_selection(request, indices, request.as_of);
      const auto cheaper = ph::price::affordable_alternative(request, product, k);
      std::vector<Amount> contributions, balances;
      long double units = 0, contribution = 0, balance = 0, score = 0;
      Amount minimum_coverage = std::numeric_limits<Amount>::max();
      for (std::size_t s = 0; s < request.scenarios.size(); ++s) {
        const Amount candidate_contribution = (candidate.price - product.unit_cost) * candidate.forecast_units[s];
        const Amount total = basket_contribution[s] -
            (published.price - product.unit_cost) * published.forecast_units[s] + candidate_contribution;
        const Amount surplus = total - required;
        const Amount remaining = request.funding_balance + surplus;
        const long double probability = request.scenarios[s].probability;
        contributions.push_back(candidate_contribution); balances.push_back(remaining);
        units += probability * candidate.forecast_units[s];
        contribution += probability * candidate_contribution;
        balance += probability * remaining; score += probability * std::abs(remaining);
        minimum_coverage = std::min(minimum_coverage, surplus + request.coverage_credit + request.liquidity_buffer);
      }
      product_rows[i]["candidate_forecasts"][k]["price_comparison"] = {
          {"expected_units", static_cast<double>(units)}, {"expected_contribution", static_cast<double>(contribution)},
          {"scenario_contribution", contributions}, {"scenario_funding_balance", balances},
          {"expected_funding_balance", static_cast<double>(balance)}, {"hypothetical_balance_score", static_cast<double>(score)},
          {"minimum_coverage_slack", minimum_coverage}, {"admissible", checked.recommendation.has_value()},
          {"exclusion_reasons", checked.validation.errors},
          {"affordable_alternative_price", cheaper ? Json(product.candidates[*cheaper].price) : Json(nullptr)},
          {"published", k == published_indices[i]}};
    }
  }
}
void validate_history(const Json& history, const Json& config) {
  fields(history, {"schema_version", "observations"}, "history");
  if (history.at("schema_version") != "exchange.history.v1") bad("unsupported history schema");
  if (!history.at("observations").is_array() || history.at("observations").size() > 12000) bad("history needs at most 12000 observations");
  std::map<std::string, Amount> last;
  for (const auto& product : config.at("products")) last[product.at("sku").get<std::string>()] = -1000001;
  for (const auto& item : history.at("observations")) {
    fields(item, {"day", "sku", "price", "sales_units", "stockout"}, "observation");
    const auto sku = label(item.at("sku"), "observation.sku");
    const auto day = number(item.at("day"), -1000000, -1, "observation.day");
    if (!last.count(sku) || day <= last[sku]) bad("history must have known SKUs and strictly increasing negative days per SKU");
    last[sku] = day;
    number(item.at("price"), 1, 1000000, "observation.price");
    number(item.at("sales_units"), 0, 1000000, "observation.sales_units");
    if (!item.at("stockout").is_boolean()) bad("observation.stockout must be boolean");
  }
}
Json path(const Json& config, bool optimized, const Json& history) {
  using namespace ph::price;
  const auto consumers = models::consumer_config(config.at("consumers"));
  const auto forecast_config = models::forecast_config(config.at("forecast"));
  const Amount physical_demand_cap = static_cast<Amount>(consumers.potential_visitors) * consumers.max_units_per_product;
  std::vector<Stock> stocks(config.at("products").size());
  std::vector<Amount> previous;
  std::vector<models::ProductPrior> priors;
  std::vector<models::EwmaForecaster> forecasts;
  Amount initial_inventory = 0;
  for (std::size_t i = 0; i < stocks.size(); ++i) {
    const auto& p = config.at("products")[i];
    stocks[i].add(value(p, "initial_stock"), value(p, "unit_cost"));
    initial_inventory += stocks[i].worth();
    previous.push_back(value(p, optimized ? "reference_price" : "fixed_price"));
    priors.push_back(prior(p));
    forecasts.emplace_back(forecast_config, static_cast<double>(value(p, "base_demand")));
    for (const auto& item : history.at("observations")) if (item.at("sku") == p.at("sku")) {
      const auto sales = value(item, "sales_units");
      forecasts.back().observe(static_cast<int>(value(item, "day")), sales,
          sales + (item.at("stockout").get<bool>() ? 0 : 1),
          models::price_response_factor(consumers, priors.back(), value(item, "price")), true);
    }
  }
  Amount cash = value(config, "initial_cash"), reserve = value(config, "initial_reserve"), wage_arrears = 0, operating_arrears = 0;
  Amount cumulative_result = 0, successful = 0, continuity_days = 0, funding_balance = 0, scheduled_reserve = 0;
  Amount target = value(config, "reserve_target"), terminal_day = 0;
  std::string terminal_status = "completed";
  Json totals = {{"revenue", 0}, {"procurement", 0}, {"cost_of_goods_sold", 0}, {"waste_cost", 0},
      {"waste_units", 0}, {"sales_units", 0}, {"unmet_demand", 0}, {"worker_wages_due", 0},
      {"worker_wages_paid", 0}, {"operating_cost_due", 0}, {"operating_cost_paid", 0},
      {"visitors", 0}, {"no_visit", 0}, {"no_purchase", 0}};
  Json rows = Json::array(), events = Json::array();
  const std::string policy = optimized ? "optimized" : "fixed";
  const auto event = [&](Amount day, const std::string& type, const std::string& reason, Amount support, const Json& evidence) {
    Amount stock_value = 0; for (const auto& stock : stocks) stock_value += stock.worth();
    events.push_back({{"schema_version", "exchange.assurance.v1"}, {"id", policy + "-" + std::to_string(day) + "-" + type},
        {"policy", policy}, {"type", type}, {"provider", config.at("assurance").at("provider")}, {"day", day},
        {"reason", reason}, {"currency", config.at("currency")}, {"required_support", std::max<Amount>(0, support)},
        {"cash", cash}, {"reserve", reserve}, {"inventory_value", stock_value}, {"funding_balance", funding_balance},
        {"wage_arrears", wage_arrears}, {"operating_arrears", operating_arrears}, {"forecast", evidence},
        {"settlement_status", "unfunded_request"}});
  };
  auto backend = make_enumeration_backend(static_cast<std::size_t>(value(config, "max_price_combinations")));
  if (cash == 0) {
    terminal_status = "insolvent";
    event(0, "assurance_requested", "opening_cash_exhausted", value(config, "worker_wages") + value(config, "operating_cost"), Json::object());
    event(0, "insolvency_declared", "opening_cash_exhausted", value(config, "worker_wages") + value(config, "operating_cost"), Json::object());
  }
  for (Amount day = 1; day <= value(config, "periods") && terminal_status == "completed"; ++day) {
    const auto& shock = config.at("shock");
    const bool shock_active = value(shock, "start_day") > 0 && day >= value(shock, "start_day") && day <= value(shock, "end_day");
    const Amount demand_factor = shock_active ? value(shock, "demand_factor_bps") : scale;
    const Amount cost_factor = shock_active ? value(shock, "cost_factor_bps") : scale;
    const Amount opening_cash = cash, opening_reserve = reserve, funding_before = funding_balance;
    const auto opening_stocks = stocks;
    const Amount adjustment = feedback_adjustment(funding_before, value(config, "feedback_recovery_days"));
    const Amount wages_due = value(config, "worker_wages"), operations_due = value(config, "operating_cost");
    const Amount fixed_cost = wages_due + operations_due;
    // Reserve planning uses history-only expected demand at the currently posted prices.
    // All product/time errors are stressed together: H * sum(|margin| sigma), not a claimed tail guarantee.
    double mean_contribution = 0, contribution_sigma = 0;
    for (std::size_t i = 0; i < stocks.size(); ++i) {
      const auto& p = config.at("products")[i];
      const auto projection = forecasts[i].predict(models::price_response_factor(consumers, priors[i], previous[i]));
      const Amount cost = rounded(value(p, "unit_cost") * cost_factor);
      const double limit = static_cast<double>(std::max(stocks[i].units(), value(p, "target_stock")));
      mean_contribution += (previous[i] - cost) * std::min({projection.mean, limit, static_cast<double>(physical_demand_cap)});
      contribution_sigma += std::abs(previous[i] - cost) * std::min(projection.sigma, static_cast<double>(physical_demand_cap));
    }
    const double horizon = forecast_config.reserve_horizon_days;
    const double reserve_estimate = horizon * std::max(0.0, static_cast<double>(fixed_cost) - mean_contribution) +
        horizon * forecast_config.sigma_multiplier_bps / 10000.0 * contribution_sigma;
    if (!std::isfinite(reserve_estimate) || reserve_estimate > static_cast<double>(Amount{1} << 50)) bad("forecast reserve exceeds exact arithmetic bound");
    target = std::max(value(config, "reserve_target"), static_cast<Amount>(std::ceil(reserve_estimate)));
    const Amount reserve_requirement = std::min(value(config, "reserve_contribution"), std::max<Amount>(0,
        target - value(config, "initial_reserve") - scheduled_reserve));
    scheduled_reserve += reserve_requirement;
    Amount budget = std::min(value(config, "procurement_budget"), std::max<Amount>(0,
        cash - reserve - wage_arrears - operating_arrears - fixed_cost));
    Request request;
    request.request_id = policy + "-" + std::to_string(day);
    request.currency = config.at("currency").get<std::string>();
    request.forecast_source = "history-only EWMA with declared logit price response and sigma stress";
    request.worker_policy_reference = "configured exchange research policy; unauthenticated";
    request.as_of = 1800000000 + day * 86400; request.valid_until = request.as_of + 3600; request.max_input_age_seconds = 3600;
    request.worker_wage_floor = wages_due; request.operating_cost = operations_due;
    request.reserve_floor = reserve_requirement; request.funding_balance = adjustment;
    for (const auto& s : config.at("scenarios")) request.scenarios.push_back({s.at("id").get<std::string>(), s.at("probability").get<double>()});
    Json product_rows = Json::array();
    Amount purchases = 0, opening_inventory = 0;
    std::vector<Amount> available;
    for (std::size_t i = 0; i < stocks.size(); ++i) {
      const auto& p = config.at("products")[i]; auto& stock = stocks[i];
      const Amount before_units = stock.units(), before_worth = stock.worth(); opening_inventory += before_worth;
      const Amount cost = rounded(value(p, "unit_cost") * cost_factor);
      const Amount requested = std::max<Amount>(0, value(p, "target_stock") - before_units);
      const Amount purchased = cost ? std::min(requested, budget / cost) : requested;
      const Amount spending = purchased * cost;
      cash -= spending; budget -= spending; purchases += spending; stock.add(purchased, cost, day); available.push_back(stock.units());
      Product product;
      product.sku = priors[i].sku; product.unit_cost = cost; product.previous_price = previous[i];
      product.affordability_ceiling = value(p, "affordability_ceiling");
      product.max_change_basis_points = static_cast<int>(value(p, "max_change_bps")); product.inventory = stock.units();
      Json candidate_forecasts = Json::array();
      const auto add_candidate = [&](Amount price) {
        const auto projection = forecasts[i].predict(models::price_response_factor(consumers, priors[i], price));
        Candidate candidate; candidate.price = price; Json latent = Json::array();
        for (const auto& scenario : config.at("scenarios")) {
          const double point = std::max(0.0, projection.mean + projection.sigma * forecast_config.sigma_multiplier_bps / 10000.0 *
              value(scenario, "sigma_offset_bps") / 10000.0);
          const Amount quantity = static_cast<Amount>(std::llround(std::min(point, static_cast<double>(physical_demand_cap))));
          latent.push_back(quantity);
          // The tool receives saleable units; uncapped demand is preserved in the application record.
          candidate.forecast_units.push_back(std::min(quantity, stock.units()));
        }
        candidate_forecasts.push_back({{"price", price}, {"forecast", models::to_json(projection)},
            {"physical_demand_cap", physical_demand_cap}, {"latent_scenario_units", latent}, {"saleable_scenario_units", candidate.forecast_units}});
        product.candidates.push_back(std::move(candidate));
      };
      if (optimized) for (const auto& price : p.at("candidate_prices")) add_candidate(price.get<Amount>());
      else add_candidate(previous[i]);
      request.products.push_back(std::move(product));
      product_rows.push_back({{"sku", p.at("sku")}, {"label", p.at("label")}, {"unit_cost", cost},
          {"opening_stock", before_units}, {"opening_inventory_value", before_worth}, {"requested_units", requested},
          {"purchased_units", purchased}, {"purchase_cost", spending}, {"previous_price", previous[i]},
          {"candidate_forecasts", candidate_forecasts}, {"consumer_prior", models::consumer_prior(consumers, priors[i])}});
    }
    const Amount liquid = std::max<Amount>(0, cash - wage_arrears - operating_arrears);
    request.coverage_credit = adjustment > 0 ? std::min(std::max<Amount>(0, funding_before), std::max<Amount>(0, liquid - reserve)) : 0;
    request.liquidity_buffer = liquid - request.coverage_credit;
    SolveResult solved;
    if (optimized) solved = optimize(request, *backend, [&] { return request.as_of; });
    else {
      const auto evaluated = evaluate_selection(request, std::vector<std::size_t>(stocks.size(), 0), request.as_of);
      solved = {evaluated.recommendation ? SolveStatus::recommended : SolveStatus::infeasible,
          evaluated.recommendation ? "fixed prices independently checked" : "fixed prices require continuity funding", evaluated.recommendation};
    }
    const bool recommended = solved.status == SolveStatus::recommended && solved.recommendation;
    const bool continuity = solved.status == SolveStatus::infeasible;
    const Json forecast_evidence = {{"expected_contribution_at_previous_prices", mean_contribution},
        {"contribution_sigma_bound", contribution_sigma}, {"reserve_target", target},
        {"sigma_multiplier_bps", forecast_config.sigma_multiplier_bps}, {"reserve_horizon_days", forecast_config.reserve_horizon_days}};
    if (!recommended && !continuity) {
      cash = opening_cash; stocks = opening_stocks; scheduled_reserve -= reserve_requirement;
      terminal_status = "model_error"; terminal_day = day;
      event(day, "assurance_requested", "pricing_model_error: " + solved.detail, 0, forecast_evidence);
      break;
    }
    std::vector<Amount> prices = recommended ? solved.recommendation->public_prices : previous;
    explain_prices(request, prices, product_rows);
    // A declared application continuity rule keeps valid public prices operating.
    // This is not a solver recommendation and does not claim that forecast coverage was satisfied.
    if (recommended) ++successful; else ++continuity_days;
    const auto consumption = models::consumers_day(consumers, priors, prices, available,
        config.at("seed").get<std::uint32_t>(), static_cast<int>(day), demand_factor);
    Amount revenue = 0, cogs = 0, waste_cost = 0, waste_units = 0, sales_units = 0, unmet = 0;
    Amount inventory_error = 0, valuation_error = 0;
    for (std::size_t i = 0; i < stocks.size(); ++i) {
      const auto& p = config.at("products")[i]; auto& stock = stocks[i]; auto& row = product_rows[i];
      const auto& chosen = consumption.products[i]; const Amount price = prices[i], sold = chosen.sales_units;
      const Amount sales_cost = stock.remove(sold);
      const Amount wasted = rounded(stock.units() * value(p, "spoilage_bps"));
      const Amount discarded_cost = stock.remove(wasted); const Amount income = price * sold;
      revenue += income; cogs += sales_cost; waste_cost += discarded_cost;
      waste_units += wasted; sales_units += sold; unmet += chosen.stock_lost_units;
      row["selected_price"] = price;
      row["price_adjustment"] = price < previous[i] ? "decreased" : price > previous[i] ? "increased" : "held";
      row["price_adjustment_reason"] = !optimized ? "fixed_price_baseline" : continuity ? "continuity_price_assurance_requested" :
          price == previous[i] ? "hold_best_feasible_funding_balance" : funding_before > 0 ? "earned_surplus_reduces_prices_when_feasible" :
          "recover_realized_gap_without_harming_forecast_contribution";
      row["forecast_units"] = recommended ? Json(request.products[i].candidates[solved.recommendation->candidate_indices[i]].forecast_units) : Json(nullptr);
      row["forecast_before_sale"] = models::to_json(forecasts[i].predict(models::price_response_factor(consumers, priors[i], price)));
      row["forecast_update"] = forecasts[i].observe(static_cast<int>(day), sold, available[i], models::price_response_factor(consumers, priors[i], price), true);
      row["demand_basis"] = "consumer_choices_at_public_price"; row["demand_reference_price"] = price;
      row["actual_demand"] = chosen.requested_units; row["sales_units"] = sold; row["unmet_demand"] = chosen.stock_lost_units;
      row["budget_rejected_units"] = chosen.budget_rejected_units; row["desired_units"] = chosen.desired_units;
      row["revenue"] = income; row["cost_of_goods_sold"] = sales_cost; row["waste_units"] = wasted;
      row["waste_cost"] = discarded_cost; row["closing_stock"] = stock.units(); row["closing_inventory_value"] = stock.worth();
      row["closing_lots"] = stock.records(); row["operating_contribution"] = income - sales_cost - discarded_cost;
      row["average_acquisition_cost"] = stock.units() ? Json(static_cast<double>(stock.worth()) / stock.units()) : Json(nullptr);
      const Amount quantity_error = value(row, "opening_stock") + value(row, "purchased_units") - sold - wasted - stock.units();
      const Amount cost_error = value(row, "opening_inventory_value") + value(row, "purchase_cost") - sales_cost - discarded_cost - stock.worth();
      row["inventory_reconciliation_error"] = quantity_error; row["valuation_reconciliation_error"] = cost_error;
      inventory_error += std::abs(quantity_error); valuation_error += std::abs(cost_error); previous[i] = price;
    }
    cash += revenue; wage_arrears += wages_due; operating_arrears += operations_due;
    const Amount wages_paid = std::min(cash, wage_arrears); cash -= wages_paid; wage_arrears -= wages_paid;
    const Amount operations_paid = std::min(cash, operating_arrears); cash -= operations_paid; operating_arrears -= operations_paid;
    const Amount reserve_released = std::max<Amount>(0, reserve - cash); reserve -= reserve_released;
    const Amount economic_result = revenue - cogs - waste_cost - fixed_cost;
    cumulative_result += economic_result; funding_balance += economic_result - reserve_requirement;
    const Amount allocation = wage_arrears || operating_arrears ? 0 : std::min({value(config, "reserve_contribution"),
        std::max<Amount>(0, target - reserve), cash - reserve});
    reserve += allocation;
    Amount closing_inventory = 0; for (const auto& stock : stocks) closing_inventory += stock.worth();
    const Amount cash_error = opening_cash - purchases + revenue - wages_paid - operations_paid - cash;
    const Amount equity_error = value(config, "initial_cash") + initial_inventory + cumulative_result -
        (cash + closing_inventory - wage_arrears - operating_arrears);
    if (cash_error || equity_error || inventory_error || valuation_error || cash < 0 || reserve < 0 || reserve > cash || revenue != consumption.spending)
      throw std::logic_error("accounting reconciliation failed");
    const Amount support = std::max<Amount>(0, fixed_cost * value(config.at("assurance"), "trigger_buffer_days") + wage_arrears + operating_arrears - cash);
    if (continuity || support > 0 || cash == 0)
      event(day, "assurance_requested", continuity ? "forecast_coverage_shortfall" : "operating_cash_buffer_breach", support, forecast_evidence);
    if (cash == 0 || wage_arrears || operating_arrears) {
      terminal_status = "insolvent"; terminal_day = day;
      event(day, "insolvency_declared", "cash_exhausted_after_fixed_obligations", support, forecast_evidence);
    }
    Json row = {{"day", day}, {"status", recommended ? "recommended" : "continuity"}, {"optimization_status", status(solved.status)},
        {"detail", recommended ? solved.detail : "Continuity policy retains public prices; assurance requested; forecast coverage is not certified."},
        {"terminal_status", terminal_status}, {"shock_active", shock_active}, {"demand_factor_bps", demand_factor}, {"cost_factor_bps", cost_factor},
        {"opening_cash", opening_cash}, {"opening_inventory_value", opening_inventory}, {"procurement", purchases},
        {"revenue", revenue}, {"cost_of_goods_sold", cogs}, {"waste_cost", waste_cost}, {"waste_units", waste_units},
        {"sales_units", sales_units}, {"unmet_demand", unmet}, {"worker_wages_due", wages_due}, {"worker_wages_paid", wages_paid},
        {"operating_cost_due", operations_due}, {"operating_cost_paid", operations_paid}, {"wage_arrears", wage_arrears},
        {"operating_arrears", operating_arrears}, {"economic_result", economic_result}, {"cumulative_economic_result", cumulative_result},
        {"funding_balance_before", funding_before}, {"funding_balance_after", funding_balance}, {"feedback_adjustment", adjustment},
        {"coverage_credit", request.coverage_credit}, {"liquidity_buffer", request.liquidity_buffer}, {"reserve_requirement", reserve_requirement},
        {"reserve_target", target}, {"reserve_forecast", forecast_evidence}, {"cumulative_reserve_requirement", scheduled_reserve},
        {"actual_contribution", revenue - cogs - waste_cost}, {"actual_required", fixed_cost + reserve_requirement},
        {"actual_funding_result", economic_result - reserve_requirement}, {"closing_cash", cash}, {"available_cash", cash - reserve},
        {"reserve_balance", reserve}, {"reserve_allocated", allocation}, {"reserve_released", reserve_released}, {"opening_reserve", opening_reserve},
        {"closing_inventory_value", closing_inventory}, {"cash_reconciliation_error", cash_error}, {"equity_reconciliation_error", equity_error},
        {"inventory_reconciliation_error", inventory_error}, {"valuation_reconciliation_error", valuation_error}, {"products", product_rows},
        {"consumers", models::to_json(consumption)}, {"visitors", consumption.visitors}, {"no_visit", consumption.no_visit}, {"no_purchase", consumption.no_purchase}};
    row["expected_worker_surplus"] = recommended ? Json(solved.recommendation->expected_worker_surplus) : Json(nullptr);
    row["scenario_worker_surplus"] = recommended ? Json(solved.recommendation->scenario_worker_surplus) : Json(nullptr);
    row["scenario_funding_balance"] = recommended ? Json(solved.recommendation->scenario_funding_balance) : Json(nullptr);
    row["expected_absolute_balance"] = recommended ? Json(solved.recommendation->expected_absolute_balance) : Json(nullptr);
    long double expected_balance = 0;
    if (recommended) for (std::size_t s = 0; s < request.scenarios.size(); ++s)
      expected_balance += static_cast<long double>(request.scenarios[s].probability) * solved.recommendation->scenario_funding_balance[s];
    row["expected_funding_balance"] = recommended ? Json(static_cast<double>(expected_balance)) : Json(nullptr);
    for (auto it = totals.begin(); it != totals.end(); ++it) it.value() = it.value().get<Amount>() + value(row, it.key().c_str());
    rows.push_back(std::move(row));
  }
  Json summary = totals;
  Amount final_inventory = 0; for (const auto& stock : stocks) final_inventory += stock.worth();
  summary["economic_result"] = cumulative_result; summary["closing_cash"] = cash; summary["reserve_balance"] = reserve;
  summary["available_cash"] = cash - reserve; summary["wage_arrears"] = wage_arrears; summary["operating_arrears"] = operating_arrears;
  summary["closing_inventory_value"] = final_inventory; summary["initial_inventory_value"] = initial_inventory;
  summary["successful_periods"] = successful + continuity_days; summary["optimized_periods"] = successful;
  summary["continuity_periods"] = continuity_days; summary["failed_periods"] = 0; summary["accounting_ok"] = true;
  summary["funding_balance"] = funding_balance; summary["cumulative_reserve_requirement"] = scheduled_reserve;
  summary["terminal_status"] = terminal_status; summary["terminal_day"] = terminal_status == "completed" ? Json(nullptr) : Json(terminal_day);
  summary["assurance_requests"] = std::count_if(events.begin(), events.end(), [](const Json& e) { return e.at("type") == "assurance_requested"; });
  Json product_summary = Json::array();
  for (std::size_t i = 0; i < stocks.size(); ++i) {
    Json item = {{"sku", priors[i].sku}, {"forecast_diagnostics", forecasts[i].diagnostics()}, {"closing_lots", stocks[i].records()}};
    for (const auto* key : {"sales_units", "unmet_demand", "waste_units", "revenue", "cost_of_goods_sold", "waste_cost", "operating_contribution"}) {
      Amount sum = 0; for (const auto& row : rows) sum += value(row.at("products")[i], key); item[key] = sum;
    }
    item["average_daily_sales"] = rows.empty() ? 0.0 : static_cast<double>(value(item, "sales_units")) / rows.size();
    item["current_public_price"] = previous[i]; item["closing_stock"] = stocks[i].units(); item["closing_inventory_value"] = stocks[i].worth();
    product_summary.push_back(std::move(item));
  }
  summary["products"] = product_summary;
  return {{"mode", policy}, {"summary", summary}, {"rows", rows}, {"events", events}};
}
Json simulate(const Json& config, const Json& history) {
  validate(config); validate_history(history, config);
  const auto optimized = path(config, true, history), fixed = path(config, false, history);
  Json comparison;
  for (const auto* key : {"economic_result", "closing_cash", "worker_wages_paid", "unmet_demand", "waste_units", "funding_balance"})
    comparison[key] = value(optimized.at("summary"), key) - value(fixed.at("summary"), key);
  return {{"schema_version", schema}, {"status", "ok"}, {"config", config}, {"history", history},
      {"engine", "bounded_enumeration_of_feedback_price_model"}, {"objective", "operating_balance_tracking"},
      {"objective_version", ph::price::kObjectiveVersion},
      {"forecast_cost_basis", "current_replacement_cost"}, {"realized_cost_basis", "FIFO"},
      {"optimized", optimized}, {"fixed", fixed}, {"comparison", {{"optimized_minus_fixed", comparison},
        {"equal_observed_horizons", optimized.at("rows").size() == fixed.at("rows").size()}}},
      {"limitations", Json::array({"Research simulation: consumer coefficients and initial forecasts are declared priors, not empirical fits.",
          "EWMA uses past uncensored sales only. Forecast error, warmup and interval coverage are recorded; no TFT is implicitly trained.",
          "Sigma scenarios and reserve stress are conditional assumptions, not absolute worst-case guarantees or calibrated insurance probabilities.",
          "The demand shock changes customer arrivals and is not revealed to the forecaster in advance. Replacement costs are observed before pricing.",
          "Browser pricing uses explicit bounded enumeration; AMPL remains a separate native optimizer interface.",
          "Continuity retains existing public prices when financial coverage is infeasible and emits an unfunded assurance request, not a solver recommendation.",
          "Cash exhaustion after fixed obligations ends that policy path. No assurance payout or invisible external funding is simulated.",
          "Consumer budgets are allocated in configured product order; substitution, travel and persistent household behavior are not modeled.",
          "Reserve earmarks and endowed cash do not count as earned feedback; liquidity supporting continuity is reported separately.",
          "Each policy uses identical keyed consumer draws but its own prices, stock, sales history and terminal date."})}};
}
}  // namespace
std::string run_json(const std::string& command) {
  try {
    const auto input = parse(command);
    if (!input.is_object() || !input.contains("op") || !input.at("op").is_string()) bad("command requires op");
    const auto op = input.at("op").get<std::string>();
    if (op == "defaults") { fields(input, {"op"}, "command"); return Json{{"schema_version", schema}, {"status", "ok"}, {"config", defaults()}}.dump(); }
    if (op == "simulate") {
      if (input.contains("history")) fields(input, {"op", "config", "history"}, "command");
      else fields(input, {"op", "config"}, "command");
      return simulate(input.at("config"), input.value("history", empty_history())).dump();
    }
    bad("unsupported op");
  } catch (const std::exception& error) {
    return Json{{"schema_version", schema}, {"status", "error"}, {"error", {{"code", "invalid_simulation"}, {"message", error.what()}}}}
        .dump(-1, ' ', false, Json::error_handler_t::replace);
  }
}
}  // namespace ph::exchange
extern "C" const char* exchange_run(const char* command) {
  static std::string response;
  response = ph::exchange::run_json(command ? command : "");
  return response.c_str();
}
