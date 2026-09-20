// SPDX-License-Identifier: MIT
#include "simulation.hpp"
#include "ph/price/engine.hpp"
#include "ph/price/enumeration.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace ph::exchange {
namespace {
using Json = nlohmann::json;
using Amount = std::int64_t;
constexpr const char* schema = "exchange.sim.v1";
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
  if (input.size() > 262144) bad("command exceeds 256 KiB");
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
Json defaults() {
  return {{"periods", 30}, {"seed", 42}, {"currency", "EUR"}, {"initial_cash", 100000},
      {"initial_reserve", 5000}, {"procurement_budget", 15000}, {"worker_wages", 800},
      {"operating_cost", 200}, {"reserve_contribution", 200}, {"reserve_target", 20000},
      {"demand_noise_bps", 2000},
      {"shock", {{"start_day", 15}, {"end_day", 20}, {"demand_factor_bps", 6500}, {"cost_factor_bps", 12000}}},
      {"scenarios", Json::array({{{"id", "low"}, {"factor_bps", 8000}, {"probability", 0.25}},
          {{"id", "central"}, {"factor_bps", 10000}, {"probability", 0.5}},
          {{"id", "high"}, {"factor_bps", 12000}, {"probability", 0.25}}})},
      {"products", Json::array({
          {{"sku", "bread"}, {"label", "Bread"}, {"unit_cost", 100}, {"reference_price", 200}, {"fixed_price", 200},
           {"initial_stock", 45}, {"target_stock", 45}, {"base_demand", 20}, {"elasticity_bps", 12000},
           {"spoilage_bps", 800}, {"affordability_ceiling", 240}, {"max_change_bps", 2000},
           {"candidate_prices", {160, 180, 200, 220, 240}}},
          {{"sku", "beans"}, {"label", "Beans"}, {"unit_cost", 150}, {"reference_price", 300}, {"fixed_price", 300},
           {"initial_stock", 35}, {"target_stock", 35}, {"base_demand", 14}, {"elasticity_bps", 10000},
           {"spoilage_bps", 100}, {"affordability_ceiling", 360}, {"max_change_bps", 2000},
           {"candidate_prices", {240, 270, 300, 330, 360}}}})}};
}
void validate(const Json& config) {
  fields(config, {"periods", "seed", "currency", "initial_cash", "initial_reserve", "procurement_budget",
    "worker_wages", "operating_cost", "reserve_contribution", "reserve_target", "demand_noise_bps", "shock", "scenarios", "products"}, "config");
  number(config.at("periods"), 1, 365, "periods"); number(config.at("seed"), 0, 4294967295LL, "seed");
  const auto currency = label(config.at("currency"), "currency");
  if (currency.size() != 3 || !std::all_of(currency.begin(), currency.end(), [](char c) { return c >= 'A' && c <= 'Z'; })) bad("currency needs three uppercase letters");
  for (const auto* key : {"initial_cash", "initial_reserve", "procurement_budget", "operating_cost", "reserve_contribution", "reserve_target"})
    number(config.at(key), 0, 100000000, key);
  number(config.at("worker_wages"), 1, 100000000, "worker_wages");
  number(config.at("demand_noise_bps"), 0, 10000, "demand_noise_bps");
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
    fields(scenario, {"id", "factor_bps", "probability"}, "scenario");
    if (!scenario_ids.insert(label(scenario.at("id"), "scenario.id")).second) bad("duplicate scenario id");
    number(scenario.at("factor_bps"), 0, 30000, "scenario.factor_bps");
    if (!scenario.at("probability").is_number()) bad("probability must be a number");
    const double probability = scenario.at("probability").get<double>();
    if (!std::isfinite(probability) || probability <= 0 || probability > 1) bad("probability must be finite in (0,1]");
    probability_sum += probability;
  }
  if (std::abs(probability_sum - 1) > 1e-9) bad("scenario probabilities must sum to one");
  const auto& products = config.at("products");
  if (!products.is_array() || products.empty() || products.size() > 3) bad("one through three products required");
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
    if (!candidates.is_array() || candidates.empty() || candidates.size() > 9) bad("one through nine candidate prices required");
    std::set<Amount> prices;
    for (const auto& price : candidates) if (!prices.insert(number(price, 1, 1000000, "candidate_price")).second) bad("duplicate candidate price");
  }
}
Amount value(const Json& object, const char* name) { return object.at(name).get<Amount>(); }
Amount rounded(Amount numerator, Amount denominator = scale) { return (numerator + denominator / 2) / denominator; }
Amount demand(const Json& product, Amount price, Amount shock, Amount variation) {
  const Amount reference = value(product, "reference_price");
  // A deliberately explicit synthetic linear response, not an empirical model.
  const Amount elasticity = std::max<Amount>(0, scale - value(product, "elasticity_bps") * (price - reference) / reference);
  Amount units = value(product, "base_demand") * elasticity;
  units = rounded(units * shock); // keep units scaled until the last rounding
  units = rounded(units * variation);
  return rounded(units);
}
std::uint32_t random_step(std::uint32_t& state) {
  state = state * 1664525u + 1013904223u;
  return state;
}
struct Lot { Amount units; Amount cost; };
struct Stock {
  std::deque<Lot> lots;
  Amount units() const { Amount n = 0; for (const auto& lot : lots) n += lot.units; return n; }
  Amount worth() const { Amount n = 0; for (const auto& lot : lots) n += lot.units * lot.cost; return n; }
  void add(Amount units, Amount cost) { if (units) lots.push_back({units, cost}); }
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
Json path(const Json& config, bool optimized, const std::vector<std::vector<Amount>>& draws) {
  using namespace ph::price;
  std::vector<Stock> stocks(config.at("products").size());
  std::vector<Amount> previous;
  Amount initial_inventory = 0;
  for (std::size_t i = 0; i < stocks.size(); ++i) {
    const auto& p = config.at("products")[i];
    stocks[i].add(value(p, "initial_stock"), value(p, "unit_cost"));
    initial_inventory += stocks[i].worth(); previous.push_back(value(p, "reference_price"));
  }
  Amount cash = value(config, "initial_cash"), reserve = value(config, "initial_reserve"), wage_arrears = 0, operating_arrears = 0;
  Amount cumulative_result = 0, successful = 0, failed = 0;
  Json totals = {{"revenue", 0}, {"procurement", 0}, {"cost_of_goods_sold", 0}, {"waste_cost", 0},
      {"waste_units", 0}, {"sales_units", 0}, {"unmet_demand", 0}, {"worker_wages_due", 0},
      {"worker_wages_paid", 0}, {"operating_cost_due", 0}, {"operating_cost_paid", 0}};
  Json rows = Json::array();
  auto backend = make_enumeration_backend(729);
  for (Amount day = 1; day <= value(config, "periods"); ++day) {
    const auto& shock = config.at("shock");
    const bool shock_active = value(shock, "start_day") > 0 && day >= value(shock, "start_day") && day <= value(shock, "end_day");
    const Amount demand_factor = shock_active ? value(shock, "demand_factor_bps") : scale;
    const Amount cost_factor = shock_active ? value(shock, "cost_factor_bps") : scale;
    const Amount opening_cash = cash, opening_reserve = reserve;
    const Amount wages_due = value(config, "worker_wages"), operations_due = value(config, "operating_cost");
    // Preserve existing reserves and known payroll/operating obligations when purchasing.
    Amount budget = std::min(value(config, "procurement_budget"), std::max<Amount>(0,
        cash - reserve - wage_arrears - operating_arrears - wages_due - operations_due));
    Request request;
    request.request_id = (optimized ? "optimized-" : "fixed-") + std::to_string(day);
    request.currency = config.at("currency").get<std::string>();
    request.forecast_source = "synthetic linear demand; scenario assumptions";
    request.worker_policy_reference = "synthetic exchange policy; unauthenticated";
    request.as_of = 1800000000 + day * 86400; request.valid_until = request.as_of + 3600;
    request.max_input_age_seconds = 3600;
    request.worker_wage_floor = wages_due; request.operating_cost = operations_due;
    request.reserve_floor = std::min(value(config, "reserve_contribution"), std::max<Amount>(0, value(config, "reserve_target") - reserve));
    for (const auto& scenario : config.at("scenarios")) request.scenarios.push_back({scenario.at("id").get<std::string>(), scenario.at("probability").get<double>()});
    Json product_rows = Json::array();
    Amount purchases = 0, opening_inventory = 0;
    for (std::size_t i = 0; i < stocks.size(); ++i) {
      const auto& p = config.at("products")[i]; auto& stock = stocks[i];
      const Amount before_units = stock.units(), before_worth = stock.worth(); opening_inventory += before_worth;
      const Amount cost = rounded(value(p, "unit_cost") * cost_factor);
      const Amount requested = std::max<Amount>(0, value(p, "target_stock") - before_units);
      const Amount purchased = cost ? std::min(requested, budget / cost) : requested;
      const Amount spending = purchased * cost;
      cash -= spending; budget -= spending; purchases += spending; stock.add(purchased, cost);
      Product product;
      product.sku = p.at("sku").get<std::string>(); product.unit_cost = cost; product.previous_price = previous[i];
      product.affordability_ceiling = value(p, "affordability_ceiling"); product.max_change_basis_points = static_cast<int>(value(p, "max_change_bps"));
      product.inventory = stock.units();
      const auto add_candidate = [&](Amount price) {
        Candidate candidate; candidate.price = price;
        for (const auto& scenario : config.at("scenarios")) candidate.forecast_units.push_back(demand(p, price, demand_factor, value(scenario, "factor_bps")));
        product.candidates.push_back(std::move(candidate));
      };
      if (optimized) for (const auto& price : p.at("candidate_prices")) add_candidate(price.get<Amount>());
      else add_candidate(value(p, "fixed_price"));
      request.products.push_back(std::move(product));
      product_rows.push_back({{"sku", p.at("sku")}, {"label", p.at("label")}, {"unit_cost", cost},
          {"opening_stock", before_units}, {"opening_inventory_value", before_worth}, {"requested_units", requested},
          {"purchased_units", purchased}, {"purchase_cost", spending}, {"realized_noise_bps", draws[day - 1][i]}});
    }
    SolveResult solved;
    if (optimized) solved = optimize(request, *backend, [&] { return request.as_of; });
    else {
      const auto evaluation = evaluate_selection(request, std::vector<std::size_t>(stocks.size(), 0), request.as_of);
      solved = {evaluation.recommendation ? SolveStatus::recommended : SolveStatus::infeasible,
          evaluation.recommendation ? "fixed public prices independently feasible" : "fixed prices violate supplied policy or coverage; no trades",
          evaluation.recommendation};
    }
    const bool trading = solved.status == SolveStatus::recommended && solved.recommendation;
    if (trading) ++successful; else ++failed;
    Amount revenue = 0, cogs = 0, waste_cost = 0, waste_units = 0, sales_units = 0, unmet = 0;
    Amount inventory_error = 0, valuation_error = 0;
    for (std::size_t i = 0; i < stocks.size(); ++i) {
      const auto& p = config.at("products")[i]; auto& stock = stocks[i]; auto& row = product_rows[i];
      const Amount price = trading ? solved.recommendation->public_prices[i] : value(p, "reference_price");
      const Amount wanted = demand(p, price, demand_factor, draws[day - 1][i]);
      const Amount sold = trading ? std::min(wanted, stock.units()) : 0;
      const Amount sales_cost = stock.remove(sold);
      const Amount wasted = rounded(stock.units() * value(p, "spoilage_bps"));
      const Amount discarded_cost = stock.remove(wasted);
      const Amount income = price * sold;
      revenue += income; cogs += sales_cost; waste_cost += discarded_cost;
      waste_units += wasted; sales_units += sold; unmet += wanted - sold;
      row["selected_price"] = trading ? Json(price) : Json(nullptr);
      row["demand_basis"] = trading ? "selected_price" : "reference_price_while_closed";
      row["demand_reference_price"] = price;
      row["forecast_units"] = trading ? Json(request.products[i].candidates[solved.recommendation->candidate_indices[i]].forecast_units) : Json(nullptr);
      row["actual_demand"] = wanted; row["sales_units"] = sold; row["unmet_demand"] = wanted - sold;
      row["revenue"] = income; row["cost_of_goods_sold"] = sales_cost; row["waste_units"] = wasted;
      row["waste_cost"] = discarded_cost; row["closing_stock"] = stock.units(); row["closing_inventory_value"] = stock.worth();
      const Amount quantity_error = value(row, "opening_stock") + value(row, "purchased_units") - sold - wasted - stock.units();
      const Amount cost_error = value(row, "opening_inventory_value") + value(row, "purchase_cost") - sales_cost - discarded_cost - stock.worth();
      row["inventory_reconciliation_error"] = quantity_error; row["valuation_reconciliation_error"] = cost_error;
      inventory_error += std::abs(quantity_error); valuation_error += std::abs(cost_error);
      if (trading) previous[i] = price;
    }
    cash += revenue; wage_arrears += wages_due; operating_arrears += operations_due;
    const Amount wages_paid = std::min(cash, wage_arrears); cash -= wages_paid; wage_arrears -= wages_paid;
    const Amount operations_paid = std::min(cash, operating_arrears); cash -= operations_paid; operating_arrears -= operations_paid;
    const Amount reserve_released = std::max<Amount>(0, reserve - cash); reserve -= reserve_released;
    const Amount economic_result = revenue - cogs - waste_cost - wages_due - operations_due;
    cumulative_result += economic_result;
    const Amount allocation = wage_arrears || operating_arrears ? 0 : std::min({value(config, "reserve_contribution"),
        std::max<Amount>(0, value(config, "reserve_target") - reserve), std::max<Amount>(0, economic_result), cash - reserve});
    reserve += allocation;
    Amount closing_inventory = 0; for (const auto& stock : stocks) closing_inventory += stock.worth();
    const Amount cash_error = opening_cash - purchases + revenue - wages_paid - operations_paid - cash;
    const Amount equity_error = value(config, "initial_cash") + initial_inventory + cumulative_result -
        (cash + closing_inventory - wage_arrears - operating_arrears);
    if (cash_error || equity_error || inventory_error || valuation_error || cash < 0 || reserve < 0 || reserve > cash)
      throw std::logic_error("accounting reconciliation failed");
    Json row = {{"day", day}, {"status", status(solved.status)}, {"detail", solved.detail},
        {"shock_active", shock_active}, {"demand_factor_bps", demand_factor}, {"cost_factor_bps", cost_factor},
        {"opening_cash", opening_cash}, {"opening_inventory_value", opening_inventory}, {"procurement", purchases},
        {"revenue", revenue}, {"cost_of_goods_sold", cogs}, {"waste_cost", waste_cost}, {"waste_units", waste_units},
        {"sales_units", sales_units}, {"unmet_demand", unmet}, {"worker_wages_due", wages_due}, {"worker_wages_paid", wages_paid},
        {"operating_cost_due", operations_due}, {"operating_cost_paid", operations_paid}, {"wage_arrears", wage_arrears},
        {"operating_arrears", operating_arrears}, {"economic_result", economic_result}, {"cumulative_economic_result", cumulative_result},
        {"closing_cash", cash}, {"available_cash", cash - reserve}, {"reserve_balance", reserve}, {"reserve_allocated", allocation},
        {"reserve_released", reserve_released}, {"opening_reserve", opening_reserve}, {"closing_inventory_value", closing_inventory},
        {"cash_reconciliation_error", cash_error}, {"equity_reconciliation_error", equity_error},
        {"inventory_reconciliation_error", inventory_error}, {"valuation_reconciliation_error", valuation_error},
        {"products", product_rows}};
    row["expected_worker_surplus"] = trading ? Json(solved.recommendation->expected_worker_surplus) : Json(nullptr);
    row["scenario_worker_surplus"] = trading ? Json(solved.recommendation->scenario_worker_surplus) : Json(nullptr);
    for (auto it = totals.begin(); it != totals.end(); ++it) it.value() = it.value().get<Amount>() + value(row, it.key().c_str());
    rows.push_back(std::move(row));
  }
  Json summary = totals;
  summary["economic_result"] = cumulative_result; summary["closing_cash"] = cash; summary["reserve_balance"] = reserve;
  summary["available_cash"] = cash - reserve; summary["wage_arrears"] = wage_arrears; summary["operating_arrears"] = operating_arrears;
  summary["closing_inventory_value"] = rows.back().at("closing_inventory_value");
  summary["initial_inventory_value"] = initial_inventory; summary["successful_periods"] = successful;
  summary["failed_periods"] = failed; summary["accounting_ok"] = true;
  return {{"mode", optimized ? "optimized" : "fixed"}, {"summary", summary}, {"rows", rows}};
}
Json simulate(const Json& config) {
  validate(config);
  std::uint32_t seed = config.at("seed").get<std::uint32_t>();
  std::vector<std::vector<Amount>> draws;
  for (Amount day = 0; day < value(config, "periods"); ++day) {
    std::vector<Amount> daily;
    for (std::size_t product = 0; product < config.at("products").size(); ++product) {
      const auto span = 2 * value(config, "demand_noise_bps") + 1;
      daily.push_back(scale - value(config, "demand_noise_bps") + random_step(seed) % span);
    }
    draws.push_back(std::move(daily));
  }
  const auto optimized = path(config, true, draws), fixed = path(config, false, draws);
  Json comparison;
  for (const auto* key : {"economic_result", "closing_cash", "worker_wages_paid", "unmet_demand", "waste_units"})
    comparison[key] = value(optimized.at("summary"), key) - value(fixed.at("summary"), key);
  return {{"schema_version", schema}, {"status", "ok"}, {"config", config},
      {"engine", "bounded_enumeration_of_existing_price_model"}, {"objective", "expected_worker_surplus"},
      {"forecast_cost_basis", "current_replacement_cost"}, {"realized_cost_basis", "FIFO"},
      {"optimized", optimized}, {"fixed", fixed}, {"comparison", {{"optimized_minus_fixed", comparison}}},
      {"limitations", Json::array({"Synthetic linear demand and shocks are assumptions, not measured store evidence.",
          "Browser evaluation enumerates the existing pricing formulation; AMPL does not run inside this page.",
          "No governance, tax, debt, replenishment optimization, payment action or live price publication.",
          "No lexicographic surplus-target or essential-basket objective is implemented.",
          "Forecast surplus uses current replacement costs; realized economic result uses FIFO inventory costs, so a cost shock can make their signs differ.",
          "Closed periods have no trades; unmet demand is estimated at the configured reference price.",
          "Reserve is earmarked cash, not an expense; obligations can release reserve before arrears accrue.",
          "Baseline and optimized paths share exogenous demand draws; quantities respond separately to their prices."})}};
}
}  // namespace
std::string run_json(const std::string& command) {
  try {
    const auto input = parse(command);
    if (!input.is_object() || !input.contains("op") || !input.at("op").is_string()) bad("command requires op");
    const auto op = input.at("op").get<std::string>();
    if (op == "defaults") { fields(input, {"op"}, "command"); return Json{{"schema_version", schema}, {"status", "ok"}, {"config", defaults()}}.dump(); }
    if (op == "simulate") { fields(input, {"op", "config"}, "command"); return simulate(input.at("config")).dump(); }
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
