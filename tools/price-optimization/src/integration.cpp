// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#include "ph/price/integration.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ph::price::integration {
namespace {
[[noreturn]] void invalid(const std::string& message) {
  throw std::invalid_argument(message);
}
void fields(const Json& value, std::initializer_list<const char*> names,
            const std::string& path) {
  if (!value.is_object()) invalid(path + " must be an object");
  std::set<std::string> expected;
  for (const auto* name : names) expected.insert(name);
  for (auto it = value.begin(); it != value.end(); ++it)
    if (!expected.count(it.key())) invalid(path + ": unknown field " + it.key());
  for (const auto& name : expected)
    if (!value.contains(name)) invalid(path + ": missing field " + name);
}
const Json& array(const Json& value, const std::string& path,
                  std::size_t maximum) {
  if (!value.is_array() || value.size() > maximum)
    invalid(path + " must be an array with at most " + std::to_string(maximum) + " entries");
  return value;
}
std::string string(const Json& value, const std::string& path) {
  if (!value.is_string()) invalid(path + " must be a string");
  return value.get<std::string>();
}
std::int64_t integer(const Json& value, const std::string& path) {
  if (!value.is_number_integer()) invalid(path + " must be a signed 64-bit integer value");
  if (value.is_number_unsigned() &&
      value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    invalid(path + " exceeds signed 64-bit range");
  return value.get<std::int64_t>();
}
double probability(const Json& value) {
  if (!value.is_number()) invalid("scenario probability must be a number");
  const auto result = value.get<double>();
  if (!std::isfinite(result)) invalid("scenario probability must be finite");
  return result;
}
std::string errors(const Validation& validation) {
  std::string result;
  for (const auto& error : validation.errors) {
    if (!result.empty()) result += "; ";
    result += error;
  }
  return result;
}
const char* status_name(SolveStatus status) {
  switch (status) {
    case SolveStatus::recommended: return "recommended";
    case SolveStatus::invalid_input: return "invalid_input";
    case SolveStatus::infeasible: return "infeasible";
    case SolveStatus::unavailable: return "unavailable";
    case SolveStatus::solver_failed: return "solver_failed";
    case SolveStatus::rejected_solution: return "rejected_solution";
  }
  return "solver_failed";
}
Json objective() { return {{"id", kObjectiveId}, {"version", 1}}; }
Json envelope(const std::string& request_id, const std::string& status) {
#ifdef PH_PRICE_MODEL_SHA256
  constexpr const char* model_sha256 = PH_PRICE_MODEL_SHA256;
#else
  constexpr const char* model_sha256 = "unavailable";
#endif
  return {{"event", "result"}, {"schema_version", kSchemaVersion},
          {"request_id", request_id}, {"status", status},
          {"model_version", kModelVersion}, {"model_sha256", model_sha256},
          {"engine_version", "0.4.0"}, {"objective", objective()},
          {"policy", nullptr}, {"input_snapshot", nullptr}};
}
Json slack(std::int64_t amount) {
  return {{"slack", amount}, {"binding", amount == 0}};
}
Json exclusions(const Request& request) {
  Json candidates = Json::array();
  for (const auto& product : request.products) {
    for (std::size_t k = 0; k < product.candidates.size(); ++k) {
      const auto& candidate = product.candidates[k];
      Json reasons = Json::array();
      if (candidate.price > product.affordability_ceiling)
        reasons.push_back({{"code", "affordability_ceiling"}});
      if (std::abs(candidate.price - product.previous_price) * 10000 >
          product.previous_price * product.max_change_basis_points)
        reasons.push_back({{"code", "price_change_cap"}});
      for (std::size_t s = 0; s < request.scenarios.size(); ++s)
        if (candidate.forecast_units[s] > product.inventory)
          reasons.push_back({{"code", "inventory"}, {"scenario_id", request.scenarios[s].id}});
      if ((request.funding_balance > 0 && candidate.price > product.previous_price) ||
          (request.funding_balance < 0 && candidate.price < product.previous_price) ||
          (request.funding_balance == 0 && candidate.price != product.previous_price))
        reasons.push_back({{"code", "feedback_direction"}});
      if (request.funding_balance < 0 && candidate.price > product.previous_price) {
        const auto held = std::find_if(product.candidates.begin(), product.candidates.end(),
            [&](const Candidate& c) { return c.price == product.previous_price; });
        for (std::size_t s = 0; s < request.scenarios.size(); ++s)
          if ((candidate.price - product.unit_cost) * candidate.forecast_units[s] <
              (held->price - product.unit_cost) * held->forecast_units[s])
            reasons.push_back({{"code", "increase_reduces_contribution"},
                               {"scenario_id", request.scenarios[s].id}});
      }
      if (!reasons.empty())
        candidates.push_back({{"sku", product.sku}, {"candidate_index", k},
                              {"price", candidate.price}, {"reasons", reasons}});
    }
  }
  return {{"scope", "local_candidate_constraints_only"},
          {"global_infeasibility_explanation", false}, {"candidates", candidates}};
}
bool matches(const Recommendation& provided, const Recommendation& checked) {
  return provided.request_id == checked.request_id &&
      provided.worker_policy_reference == checked.worker_policy_reference &&
      provided.currency == checked.currency && provided.checked_at == checked.checked_at &&
      provided.valid_until == checked.valid_until && provided.skus == checked.skus &&
      provided.candidate_indices == checked.candidate_indices &&
      provided.public_prices == checked.public_prices &&
      provided.scenario_worker_surplus == checked.scenario_worker_surplus &&
      provided.scenario_funding_balance == checked.scenario_funding_balance &&
      std::isfinite(provided.expected_absolute_balance) &&
      std::abs(provided.expected_absolute_balance - checked.expected_absolute_balance) <=
          std::max(1e-5, std::abs(checked.expected_absolute_balance) * 1e-10) &&
      std::isfinite(provided.expected_worker_surplus) &&
      std::abs(provided.expected_worker_surplus - checked.expected_worker_surplus) <=
          std::max(1e-5, std::abs(checked.expected_worker_surplus) * 1e-10);
}
}  // namespace

Json parse_json(const std::string& text) {
  if (text.size() > kMaxJsonBytes) invalid("JSON input exceeds 8 MiB");
  // A stack for every container keeps keys in separate object scopes. Rejecting
  // duplicates during parsing prevents the DOM's normal last-key-wins behavior.
  struct Container { bool object; std::set<std::string> keys; };
  std::vector<Container> stack;
  const auto callback = [&](int, Json::parse_event_t event, Json& parsed) {
    if (event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start) {
      stack.push_back({event == Json::parse_event_t::object_start, {}});
      if (stack.size() > 32) invalid("JSON nesting exceeds 32 containers");
    } else if (event == Json::parse_event_t::key) {
      if (stack.empty() || !stack.back().object ||
          !stack.back().keys.insert(parsed.get<std::string>()).second)
        invalid("duplicate JSON object key");
    } else if (event == Json::parse_event_t::object_end || event == Json::parse_event_t::array_end) {
      stack.pop_back();
    }
    return true;
  };
  try {
    return Json::parse(text, callback);
  } catch (const Json::exception& error) {
    invalid(std::string("invalid JSON: ") + error.what());
  }
}

ParsedRequest parse_request(const Json& json, Timestamp now) {
  fields(json, {"schema_version", "request_id", "currency", "as_of", "valid_until",
                "max_input_age_seconds", "policy", "costs", "catalog", "demand", "objective", "feedback"}, "request");
  if (string(json.at("schema_version"), "schema_version") != kSchemaVersion)
    invalid("unsupported schema_version");
  ParsedRequest parsed;
  auto& request = parsed.engine;
  request.request_id = string(json.at("request_id"), "request_id");
  request.currency = string(json.at("currency"), "currency");
  request.as_of = integer(json.at("as_of"), "as_of");
  request.valid_until = integer(json.at("valid_until"), "valid_until");
  request.max_input_age_seconds = integer(json.at("max_input_age_seconds"), "max_input_age_seconds");
  const auto& feedback = json.at("feedback");
  fields(feedback, {"funding_balance", "coverage_credit", "liquidity_buffer"}, "feedback");
  request.funding_balance = integer(feedback.at("funding_balance"), "feedback.funding_balance");
  request.coverage_credit = integer(feedback.at("coverage_credit"), "feedback.coverage_credit");
  request.liquidity_buffer = integer(feedback.at("liquidity_buffer"), "feedback.liquidity_buffer");
  const auto& policy = json.at("policy");
  fields(policy, {"id", "version", "worker_wage_floor", "reserve_contribution", "products"}, "policy");
  parsed.policy_id = string(policy.at("id"), "policy.id");
  parsed.policy_version = integer(policy.at("version"), "policy.version");
  if (parsed.policy_id.empty() || parsed.policy_version < 1) invalid("policy needs a nonempty id and positive version");
  request.worker_policy_reference = parsed.policy_id + "@" + std::to_string(parsed.policy_version);
  request.worker_wage_floor = integer(policy.at("worker_wage_floor"), "policy.worker_wage_floor");
  request.reserve_floor = integer(policy.at("reserve_contribution"), "policy.reserve_contribution");
  std::map<std::string, const Json*> policies;
  for (const auto& item : array(policy.at("products"), "policy.products", 256)) {
    fields(item, {"sku", "affordability_ceiling", "max_change_basis_points"}, "policy.products[]");
    if (!policies.emplace(string(item.at("sku"), "policy.products[].sku"), &item).second)
      invalid("duplicate SKU in policy.products");
  }
  const auto& costs = json.at("costs");
  fields(costs, {"operating_cost", "unit_costs"}, "costs");
  request.operating_cost = integer(costs.at("operating_cost"), "costs.operating_cost");
  std::map<std::string, Money> unit_costs;
  for (const auto& item : array(costs.at("unit_costs"), "costs.unit_costs", 256)) {
    fields(item, {"sku", "amount"}, "costs.unit_costs[]");
    if (!unit_costs.emplace(string(item.at("sku"), "costs.unit_costs[].sku"),
                            integer(item.at("amount"), "costs.unit_costs[].amount")).second)
      invalid("duplicate SKU in costs.unit_costs");
  }
  const auto& demand = json.at("demand");
  fields(demand, {"source", "scenarios", "forecasts"}, "demand");
  request.forecast_source = string(demand.at("source"), "demand.source");
  for (const auto& item : array(demand.at("scenarios"), "demand.scenarios", 32)) {
    fields(item, {"id", "probability"}, "demand.scenarios[]");
    request.scenarios.push_back({string(item.at("id"), "demand.scenarios[].id"),
                                 probability(item.at("probability"))});
  }
  std::map<std::pair<std::string, Money>, std::vector<std::int64_t>> forecasts;
  for (const auto& item : array(demand.at("forecasts"), "demand.forecasts", 256 * 64)) {
    fields(item, {"sku", "price", "units"}, "demand.forecasts[]");
    std::vector<std::int64_t> units;
    for (const auto& count : array(item.at("units"), "demand.forecasts[].units", 32))
      units.push_back(integer(count, "demand.forecasts[].units[]"));
    const auto key = std::make_pair(string(item.at("sku"), "demand.forecasts[].sku"),
                                    integer(item.at("price"), "demand.forecasts[].price"));
    if (!forecasts.emplace(key, std::move(units)).second) invalid("duplicate SKU/price forecast");
  }
  std::set<std::string> seen_skus;
  for (const auto& item : array(json.at("catalog"), "catalog", 256)) {
    fields(item, {"sku", "previous_price", "inventory", "candidate_prices"}, "catalog[]");
    Product product;
    product.sku = string(item.at("sku"), "catalog[].sku");
    if (!seen_skus.insert(product.sku).second) invalid("duplicate SKU in catalog");
    const auto policy_it = policies.find(product.sku);
    const auto cost_it = unit_costs.find(product.sku);
    if (policy_it == policies.end() || cost_it == unit_costs.end())
      invalid("catalog SKU missing matching policy or unit cost: " + product.sku);
    const auto& product_policy = *policy_it->second;
    product.affordability_ceiling = integer(product_policy.at("affordability_ceiling"), "affordability_ceiling");
    const auto change = integer(product_policy.at("max_change_basis_points"), "max_change_basis_points");
    if (change < 0 || change > 10000) invalid("max_change_basis_points must be 0..10000");
    product.max_change_basis_points = static_cast<int>(change);
    product.unit_cost = cost_it->second;
    policies.erase(policy_it);
    unit_costs.erase(cost_it);
    product.previous_price = integer(item.at("previous_price"), "catalog[].previous_price");
    product.inventory = integer(item.at("inventory"), "catalog[].inventory");
    for (const auto& price_json : array(item.at("candidate_prices"), "catalog[].candidate_prices", 64)) {
      const auto price = integer(price_json, "catalog[].candidate_prices[]");
      const auto forecast = forecasts.find({product.sku, price});
      if (forecast == forecasts.end()) invalid("missing or duplicate candidate forecast for " + product.sku);
      product.candidates.push_back({price, std::move(forecast->second)});
      forecasts.erase(forecast);
    }
    request.products.push_back(std::move(product));
  }
  if (!policies.empty() || !unit_costs.empty() || !forecasts.empty())
    invalid("extra policy, cost, or forecast without a catalog candidate");
  const auto& requested_objective = json.at("objective");
  fields(requested_objective, {"id", "version"}, "objective");
  if (string(requested_objective.at("id"), "objective.id") != kObjectiveId ||
      integer(requested_objective.at("version"), "objective.version") != 1)
    invalid("unsupported objective id or version");
  const auto validation = validate_request(request, now);
  if (!validation.ok()) invalid(errors(validation));
  parsed.snapshot = json;
  return parsed;
}

Json failure_json(const std::string& request_id, const std::string& status,
                  const std::string& code, const std::string& message) {
  static const std::set<std::string> statuses = {"invalid_input", "infeasible", "unavailable",
      "solver_failed", "rejected_solution", "timed_out", "cancelled"};
  if (!statuses.count(status)) invalid("invalid failure status");
  auto result = envelope(request_id, status);
  result["error"] = {{"code", code}, {"message", message}};
  return result;
}

Json result_json(const ParsedRequest& parsed, const SolveResult& solved) {
  auto request = parsed.engine;
  auto result = envelope(request.request_id, status_name(solved.status));
  result["policy"] = {{"id", parsed.policy_id}, {"version", parsed.policy_version}};
  result["input_snapshot"] = parsed.snapshot;
  const auto fail = [&](const std::string& status, const std::string& message) {
    auto failure = result;
    failure["status"] = status;
    failure["error"] = {{"code", status}, {"message", message}};
    return failure;
  };
  if (solved.status != SolveStatus::recommended) return fail(status_name(solved.status), solved.detail);
  if (!solved.recommendation) return fail("rejected_solution", "recommended result has no selection");
  const auto& supplied = *solved.recommendation;
  // The retained snapshot is authoritative for explanations. Reparse it so an
  // accidentally modified ParsedRequest cannot attach different inputs to an
  // otherwise valid selection. Only a fully fresh snapshot produces success.
  try {
    const auto snapshot = parse_request(parsed.snapshot, supplied.checked_at);
    if (snapshot.policy_id != parsed.policy_id || snapshot.policy_version != parsed.policy_version ||
        snapshot.engine.request_id != request.request_id)
      return fail("rejected_solution", "request metadata disagrees with input snapshot");
    request = snapshot.engine;
  } catch (const std::exception& error) {
    return fail("rejected_solution", std::string("invalid result input snapshot: ") + error.what());
  }
  const auto evaluated = evaluate_selection(request, supplied.candidate_indices, supplied.checked_at);
  if (!evaluated.recommendation) return fail("rejected_solution", errors(evaluated.validation));
  const auto& checked = *evaluated.recommendation;
  if (!matches(supplied, checked)) return fail("rejected_solution", "recommendation disagrees with independent verification");
  Json products = Json::array();
  std::vector<Money> revenue(request.scenarios.size(), 0), unit_cost(request.scenarios.size(), 0);
  for (std::size_t i = 0; i < request.products.size(); ++i) {
    const auto& product = request.products[i];
    const auto k = checked.candidate_indices[i];
    const auto& candidate = product.candidates[k];
    Json scenarios = Json::array();
    for (std::size_t s = 0; s < request.scenarios.size(); ++s) {
      const auto quantity = candidate.forecast_units[s];
      const auto sales = candidate.price * quantity;
      const auto cost = product.unit_cost * quantity;
      revenue[s] += sales;
      unit_cost[s] += cost;
      scenarios.push_back({{"id", request.scenarios[s].id},
          {"forecast_units", quantity}, {"forecast_origin", "supplied_input"},
          {"revenue", sales}, {"unit_cost_total", cost}, {"contribution", sales - cost},
          {"inventory", slack(product.inventory - quantity)}});
    }
    auto price_change = slack(product.previous_price * product.max_change_basis_points -
                              std::abs(candidate.price - product.previous_price) * 10000);
    price_change["unit"] = "minor_currency_units_times_basis_points";
    products.push_back({{"sku", product.sku}, {"candidate_index", k},
        {"previous_price", product.previous_price}, {"selected_price", candidate.price},
        {"unit_cost", product.unit_cost}, {"scenarios", scenarios},
        {"constraints", {{"affordability", slack(product.affordability_ceiling - candidate.price)},
                          {"price_change", price_change}}}});
  }
  Json scenarios = Json::array();
  long double expected_revenue = 0, expected_unit_cost = 0;
  for (std::size_t s = 0; s < request.scenarios.size(); ++s) {
    const auto& scenario = request.scenarios[s];
    expected_revenue += static_cast<long double>(scenario.probability) * revenue[s];
    expected_unit_cost += static_cast<long double>(scenario.probability) * unit_cost[s];
    scenarios.push_back({{"id", scenario.id}, {"probability", scenario.probability},
        {"revenue", revenue[s]}, {"unit_cost_total", unit_cost[s]},
        {"contribution", revenue[s] - unit_cost[s]},
        {"worker_wages", request.worker_wage_floor}, {"operating_cost", request.operating_cost},
        {"reserve_contribution", request.reserve_floor},
        {"worker_surplus", checked.scenario_worker_surplus[s]},
        {"funding_balance", checked.scenario_funding_balance[s]},
        {"coverage", slack(checked.scenario_worker_surplus[s] + request.coverage_credit + request.liquidity_buffer)}});
  }
  // Probabilities are input doubles; expected amounts are weighted estimates,
  // while all scenario amounts and hard-constraint slack remain exact integers.
  double sum_probability = 0;
  for (const auto& scenario : request.scenarios) sum_probability += scenario.probability;
  Json expected = {{"revenue", static_cast<double>(expected_revenue)},
      {"unit_cost_total", static_cast<double>(expected_unit_cost)},
      {"contribution", static_cast<double>(expected_revenue - expected_unit_cost)},
      {"worker_wages", sum_probability * request.worker_wage_floor},
      {"operating_cost", sum_probability * request.operating_cost},
      {"reserve_contribution", sum_probability * request.reserve_floor},
      {"worker_surplus", checked.expected_worker_surplus},
      {"absolute_funding_balance", checked.expected_absolute_balance},
      {"arithmetic", "probability_weighted_floating_point"}};
  result["recommendation"] = {{"currency", request.currency},
      {"monetary_unit", "minor_currency_unit"}, {"checked_at", checked.checked_at},
      {"valid_until", checked.valid_until}, {"products", products},
      {"scenarios", scenarios}, {"expected", expected},
      {"feedback", {{"funding_balance", request.funding_balance},
                     {"coverage_credit", request.coverage_credit},
                     {"liquidity_buffer", request.liquidity_buffer},
                     {"direction", request.funding_balance > 0 ? "down_or_hold" :
                         request.funding_balance < 0 ? "up_or_hold" : "hold"}}}};
  result["local_candidate_exclusions"] = exclusions(request);
  return result;
}

Json synthetic_request_json(Timestamp now) {
  if (now <= 0 || now > std::numeric_limits<Timestamp>::max() - 300)
    invalid("invalid synthetic fixture timestamp");
  return {{"schema_version", kSchemaVersion}, {"request_id", "synthetic-cooperative-001"},
      {"currency", "EUR"}, {"as_of", now}, {"valid_until", now + 300},
      {"max_input_age_seconds", 120},
      {"feedback", {{"funding_balance", 0}, {"coverage_credit", 0}, {"liquidity_buffer", 0}}},
      {"policy", {{"id", "synthetic worker assembly resolution 001"}, {"version", 1},
          {"worker_wage_floor", 800}, {"reserve_contribution", 100},
          {"products", Json::array({{{"sku", "bread"}, {"affordability_ceiling", 220}, {"max_change_basis_points", 1000}},
                                     {{"sku", "beans"}, {"affordability_ceiling", 330}, {"max_change_basis_points", 1000}}})}}},
      {"costs", {{"operating_cost", 100}, {"unit_costs", Json::array({{{"sku", "bread"}, {"amount", 100}},
                                                                    {{"sku", "beans"}, {"amount", 150}}})}}},
      {"catalog", Json::array({{{"sku", "bread"}, {"previous_price", 200}, {"inventory", 20}, {"candidate_prices", {180, 200, 220, 240}}},
                                {{"sku", "beans"}, {"previous_price", 300}, {"inventory", 15}, {"candidate_prices", {270, 300, 330}}}})},
      {"demand", {{"source", "synthetic fixture; no empirical elasticity claim"},
          {"scenarios", Json::array({{{"id", "usual"}, {"probability", 0.6}}, {{"id", "low-demand"}, {"probability", 0.4}}})},
          {"forecasts", Json::array({{{"sku", "bread"}, {"price", 180}, {"units", {20, 14}}},
                                      {{"sku", "bread"}, {"price", 200}, {"units", {16, 12}}},
                                      {{"sku", "bread"}, {"price", 220}, {"units", {12, 8}}},
                                      {{"sku", "bread"}, {"price", 240}, {"units", {20, 18}}},
                                      {{"sku", "beans"}, {"price", 270}, {"units", {13, 9}}},
                                      {{"sku", "beans"}, {"price", 300}, {"units", {12, 8}}},
                                      {{"sku", "beans"}, {"price", 330}, {"units", {8, 5}}}})}}},
      {"objective", objective()}};
}
}  // namespace ph::price::integration
