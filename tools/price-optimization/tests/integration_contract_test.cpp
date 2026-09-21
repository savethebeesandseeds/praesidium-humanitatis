// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#include "ph/price/integration.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace ph::price;
using namespace ph::price::integration;
constexpr Timestamp now = 1800000000;
int assertions = 0;
void check(bool condition, const char* message) {
  ++assertions;
  if (!condition) throw std::runtime_error(message);
}
void rejects(const std::function<void()>& action, const char* message) {
  ++assertions;
  try { action(); } catch (const std::invalid_argument&) { return; }
  throw std::runtime_error(message);
}
void bad_request(const std::function<void(Json&)>& mutate, const char* message) {
  auto request = synthetic_request_json(now);
  mutate(request);
  rejects([&] { (void)parse_request(request, now); }, message);
}

void parsing_tests() {
  const auto input = synthetic_request_json(now);
  const auto parsed = parse_request(parse_json(input.dump()), now);
  check(parsed.snapshot == input, "snapshot preserves exact validated input");
  check(parsed.engine.products.size() == 2 && parsed.engine.products[0].candidates.size() == 4,
        "split contracts join into products");
  check(parsed.engine.worker_policy_reference == "synthetic worker assembly resolution 001@1",
        "policy identity/version audit label");
  check(parsed.engine.scenarios[0].probability == 0.6, "probability parsed");
  rejects([] { (void)parse_json("{\"x\":1,\"x\":2}"); }, "duplicate root keys rejected");
  rejects([] { (void)parse_json("{\"a\":[{\"x\":1,\"x\":2}]}"); }, "duplicate nested keys rejected");
  rejects([] { (void)parse_json("{\"a\":1,\"\\u0061\":2}"); }, "escaped duplicate key rejected");
  check(parse_json("[{\"x\":1},{\"x\":2}]").size() == 2, "keys allowed in independent object scopes");
  rejects([] { (void)parse_json("[NaN]"); }, "NaN rejected");
  rejects([] { (void)parse_json("[1e10000]"); }, "floating overflow rejected");
  rejects([] { (void)parse_json("{}{} "); }, "trailing JSON rejected");
  rejects([] { (void)parse_json(std::string(33, '[') + "0" + std::string(33, ']')); }, "depth limit enforced");
  check(parse_json(std::string(32, '[') + "0" + std::string(32, ']')).is_array(), "depth32 allowed");
  rejects([] { (void)parse_json(std::string(kMaxJsonBytes + 1, ' ')); }, "JSON size limit enforced");
  bad_request([](Json& j) { j["extra"] = 1; }, "unknown top-level field rejected");
  bad_request([](Json& j) { j["catalog"][0]["extra"] = 1; }, "unknown nested field rejected");
  bad_request([](Json& j) { j.erase("currency"); }, "missing required field rejected");
  bad_request([](Json& j) { j["schema_version"] = "ph.price.v1"; }, "old profit-maximizing schema rejected");
  bad_request([](Json& j) { j["schema_version"] = "ph.price.v2"; }, "old feedback-only schema rejected");
  bad_request([](Json& j) { j.erase("feedback"); }, "feedback must be explicit");
  bad_request([](Json& j) { j["feedback"]["extra"] = 0; }, "unknown feedback field rejected");
  bad_request([](Json& j) { j["feedback"]["funding_balance"] = 0.5; }, "fractional balance rejected");
  bad_request([](Json& j) { j["feedback"]["coverage_credit"] = -1; }, "negative credit rejected");
  bad_request([](Json& j) { j["feedback"]["coverage_credit"] = 1; }, "unearned credit rejected");
  bad_request([](Json& j) { j["feedback"].erase("liquidity_buffer"); }, "continuity liquidity must be explicit");
  bad_request([](Json& j) { j["feedback"]["liquidity_buffer"] = -1; }, "negative liquidity rejected");
  bad_request([](Json& j) { j["feedback"]["liquidity_buffer"] = 1.0; }, "fractional liquidity rejected");
  bad_request([](Json& j) { j["feedback"]["liquidity_buffer"] = (1LL << 50) + 1; }, "liquidity bound enforced");
  bad_request([](Json& j) { j["feedback"]["funding_balance"] = (1LL << 50) + 1; }, "balance bound enforced");
  bad_request([](Json& j) { j["objective"]["id"] = "expected_worker_surplus"; }, "old objective rejected");
  bad_request([](Json& j) { j["objective"]["id"] = "maximize_revenue"; }, "unsupported objective rejected");
  bad_request([](Json& j) { j["objective"]["version"] = 1.0; }, "objective version float rejected");
  bad_request([](Json& j) { j["policy"]["version"] = 0; }, "policy version lower bound");
  bad_request([](Json& j) { j["policy"]["id"] = std::string(255, 'a'); }, "combined policy reference length bounded");
  bad_request([](Json& j) { j["policy"]["worker_wage_floor"] = 800.0; }, "float money rejected");
  bad_request([](Json& j) { j["costs"]["operating_cost"] = true; }, "boolean money rejected");
  bad_request([](Json& j) { j["catalog"][0]["inventory"] = 20.0; }, "float inventory rejected");
  bad_request([](Json& j) { j["as_of"] = std::numeric_limits<std::uint64_t>::max(); }, "unsigned overflow rejected");
  bad_request([](Json& j) { j["policy"]["products"][0]["max_change_basis_points"] = 4294967296LL; }, "int narrowing prevented");
  bad_request([](Json& j) { j["demand"]["scenarios"][0]["probability"] = true; }, "boolean probability rejected");
  bad_request([](Json& j) { j["policy"]["products"].erase(0); }, "missing policy join rejected");
  bad_request([](Json& j) { j["costs"]["unit_costs"].erase(0); }, "missing cost join rejected");
  bad_request([](Json& j) { j["demand"]["forecasts"].erase(0); }, "missing forecast join rejected");
  bad_request([](Json& j) { j["policy"]["products"].push_back(j["policy"]["products"][0]); }, "duplicate policy join rejected");
  bad_request([](Json& j) { j["costs"]["unit_costs"].push_back(j["costs"]["unit_costs"][0]); }, "duplicate cost join rejected");
  bad_request([](Json& j) { j["demand"]["forecasts"].push_back(j["demand"]["forecasts"][0]); }, "duplicate forecast rejected");
  bad_request([](Json& j) { j["catalog"].push_back(j["catalog"][0]); }, "duplicate catalog rejected");
  bad_request([](Json& j) { j["catalog"][0]["candidate_prices"].push_back(180); }, "duplicate candidate rejected");
  bad_request([](Json& j) { auto extra = j["policy"]["products"][0]; extra["sku"] = "extra"; j["policy"]["products"].push_back(extra); }, "extra policy join rejected");
  bad_request([](Json& j) { auto extra = j["costs"]["unit_costs"][0]; extra["sku"] = "extra"; j["costs"]["unit_costs"].push_back(extra); }, "extra cost join rejected");
  bad_request([](Json& j) { auto extra = j["demand"]["forecasts"][0]; extra["price"] = 181; j["demand"]["forecasts"].push_back(extra); }, "extra forecast rejected");
  bad_request([](Json& j) { j["demand"]["forecasts"][0]["units"].erase(0); }, "scenario forecast dimension mismatch rejected");
  bad_request([](Json& j) { j["valid_until"] = now; }, "expired input rejected");
  bad_request([](Json& j) { j["as_of"] = now - 120; }, "stale input rejected");
  // The join is by SKU/price, independently of policy, cost and forecast order.
  auto reordered = input;
  std::reverse(reordered["policy"]["products"].begin(), reordered["policy"]["products"].end());
  std::reverse(reordered["costs"]["unit_costs"].begin(), reordered["costs"]["unit_costs"].end());
  std::reverse(reordered["demand"]["forecasts"].begin(), reordered["demand"]["forecasts"].end());
  const auto joined = parse_request(reordered, now);
  check(joined.engine.products[0].unit_cost == 100 &&
        joined.engine.products[0].candidates[1].forecast_units == std::vector<std::int64_t>({16, 12}),
        "cross-joins do not depend on input array ordering");
}

void result_tests() {
  const auto request = parse_request(synthetic_request_json(now), now);
  const auto selected = evaluate_selection(request.engine, {1, 1}, now);
  const SolveResult solved{SolveStatus::recommended, "verified", selected.recommendation};
  const auto result = result_json(request, solved);
  check(result.at("status") == "recommended", "verified result accepted");
  check(result.at("input_snapshot") == request.snapshot, "result retains validated request");
  check(result.at("model_version") == "public-prices.v3" && result.at("engine_version") == "0.4.0" &&
        result.at("objective").at("id") == kObjectiveId,
        "versioned model and objective");
  const auto& rec = result.at("recommendation");
  check(rec.at("products")[0].at("selected_price") == 200 && rec.at("products")[1].at("selected_price") == 300,
        "selected public prices");
  check(rec.at("products")[0].at("previous_price") == 200 && rec.at("products")[0].at("candidate_index") == 1,
        "selected and previous price plus index");
  const auto& usual = rec.at("scenarios")[0];
  check(usual.at("revenue") == 6800 && usual.at("unit_cost_total") == 3400 && usual.at("worker_surplus") == 2400,
        "exact ordinary scenario accounts");
  check(rec.at("scenarios")[1].at("revenue") == 4800 && rec.at("scenarios")[1].at("worker_surplus") == 1400,
        "exact low-demand scenario accounts");
  check(rec.at("expected").at("revenue") == 6000.0 && rec.at("expected").at("worker_surplus") == 2000.0,
        "expected financial amounts recomputed");
  check(rec.at("expected").at("absolute_funding_balance") == 2000.0 &&
        rec.at("feedback").at("direction") == "hold" && usual.at("funding_balance") == 2400,
        "balance objective and direction are explicit");
  check(usual.at("revenue").is_number_integer(), "scenario money uses integer JSON");
  check(rec.at("products")[0].at("scenarios")[0].at("forecast_origin") == "supplied_input",
        "forecast origin distinguished from derived money");
  check(rec.at("products")[0].at("scenarios")[0].at("inventory").at("slack") == 4,
        "exact inventory slack");
  check(rec.at("products")[0].at("constraints").at("affordability").at("slack") == 20,
        "exact affordability slack");
  check(rec.at("products")[0].at("constraints").at("price_change").at("slack") == 200000,
        "exact price-change numerator slack");
  check(usual.at("coverage").at("slack") == 2400 && usual.at("coverage").at("binding") == false,
        "exact protected coverage slack");
  const auto& local = result.at("local_candidate_exclusions");
  check(local.at("global_infeasibility_explanation") == false && local.at("candidates").size() == 5,
        "explanation does not assert global infeasibility cause");
  const auto excluded = std::find_if(local.at("candidates").begin(), local.at("candidates").end(),
      [](const Json& item) { return item.at("sku") == "bread" && item.at("price") == 240; });
  check(excluded != local.at("candidates").end() &&
        excluded->at("reasons")[0].at("code") == "affordability_ceiling" &&
        excluded->at("reasons")[1].at("code") == "price_change_cap" &&
        excluded->at("reasons")[2].at("code") == "feedback_direction",
        "unavailable candidate has explicit local reasons");
  auto binding_input = request.snapshot;
  binding_input["policy"]["products"][0]["affordability_ceiling"] = 200;
  binding_input["policy"]["products"][0]["max_change_basis_points"] = 0;
  binding_input["catalog"][0]["inventory"] = 16;
  binding_input["policy"]["worker_wage_floor"] = 2200;
  const auto binding = parse_request(binding_input, now);
  const auto binding_selected = evaluate_selection(binding.engine, {1, 1}, now);
  const auto binding_result = result_json(binding, {SolveStatus::recommended, "", binding_selected.recommendation});
  const auto& bind = binding_result.at("recommendation");
  check(bind.at("products")[0].at("constraints").at("affordability").at("binding") == true &&
        bind.at("products")[0].at("constraints").at("price_change").at("binding") == true,
        "binding price limits explained");
  check(bind.at("products")[0].at("scenarios")[0].at("inventory").at("binding") == true &&
        bind.at("scenarios")[1].at("coverage").at("binding") == true,
        "binding inventory and coverage explained");
  for (const auto status : {SolveStatus::invalid_input, SolveStatus::infeasible, SolveStatus::unavailable,
                            SolveStatus::solver_failed, SolveStatus::rejected_solution}) {
    const auto failure = result_json(request, {status, "unproven cause", std::nullopt});
    check(!failure.contains("recommendation") && failure.contains("error"), "failures contain no recommendation");
    check(!failure.contains("global_infeasibility_causes"), "no fabricated infeasibility explanation");
  }
  auto forged = solved;
  forged.recommendation->public_prices[0] = 201;
  check(result_json(request, forged).at("status") == "rejected_solution", "forged public price rejected");
  forged = solved;
  forged.recommendation->expected_worker_surplus += 1;
  check(result_json(request, forged).at("status") == "rejected_solution", "forged objective rejected");
  forged = solved;
  forged.recommendation->expected_absolute_balance += 1;
  check(result_json(request, forged).at("status") == "rejected_solution", "forged balance objective rejected");
  forged = solved;
  forged.recommendation->scenario_funding_balance[0] += 1;
  check(result_json(request, forged).at("status") == "rejected_solution", "forged scenario balance rejected");
  forged = solved;
  forged.recommendation->candidate_indices[0] = 3;
  check(result_json(request, forged).at("status") == "rejected_solution", "infeasible candidate rejected");
  forged = solved;
  forged.recommendation->checked_at = now + 120;
  check(result_json(request, forged).at("status") == "rejected_solution", "expired result rejected");
  check(result_json(request, {SolveStatus::recommended, "", std::nullopt}).at("status") == "rejected_solution",
        "missing recommendation rejected");
  for (const auto* status : {"timed_out", "cancelled"}) {
    const auto failure = failure_json("id", status, status, "execution stopped");
    check(failure.at("status") == status && failure.at("input_snapshot").is_null() &&
          !failure.contains("recommendation"), "external failure stable envelope");
  }
  rejects([] { (void)failure_json("id", "recommended", "x", "x"); }, "cannot label failure as success");
}

void liquidity_tests() {
  auto input = synthetic_request_json(now);
  input["policy"]["worker_wage_floor"] = 2500;
  input["feedback"]["liquidity_buffer"] = 300;
  for (const auto balance : {Money{0}, Money{-100}}) {
    input["feedback"]["funding_balance"] = balance;
    const auto parsed = parse_request(input, now);
    const auto selected = evaluate_selection(parsed.engine, {1, 1}, now);
    check(selected.recommendation.has_value(), "unearned liquidity funds continuity at neutral or negative feedback");
    const auto result = result_json(parsed, {SolveStatus::recommended, "", selected.recommendation});
    const auto& recommendation = result.at("recommendation");
    check(recommendation.at("feedback").at("liquidity_buffer") == 300 &&
          recommendation.at("scenarios")[1].at("coverage").at("slack") == 0,
          "liquidity is explicit and exact coverage is binding");
    check(recommendation.at("scenarios")[1].at("worker_surplus") == -300 &&
          recommendation.at("scenarios")[1].at("funding_balance") == balance - 300,
          "cash does not become earned income in either financial or feedback accounting");
    auto altered = parsed;
    altered.snapshot["feedback"]["liquidity_buffer"] = 299;
    check(result_json(altered, {SolveStatus::recommended, "", selected.recommendation}).at("status") == "rejected_solution",
          "independent result validation uses the authoritative liquidity amount");
  }
  input["feedback"] = {{"funding_balance", 1}, {"coverage_credit", 100}, {"liquidity_buffer", 200}};
  const auto mixed = parse_request(input, now);
  const auto mixed_selection = evaluate_selection(mixed.engine, {1, 1}, now);
  const auto mixed_result = result_json(mixed, {SolveStatus::recommended, "", mixed_selection.recommendation});
  check(mixed_result.at("recommendation").at("scenarios")[1].at("coverage").at("slack") == 0,
        "coverage adds the two caller-established disjoint cash sources");
  input["feedback"]["coverage_credit"] = Money{1} << 50;
  input["feedback"]["liquidity_buffer"] = Money{1} << 50;
  const auto boundary = parse_request(input, now);
  const auto boundary_selection = evaluate_selection(boundary.engine, {1, 1}, now);
  const auto boundary_result = result_json(boundary, {SolveStatus::recommended, "", boundary_selection.recommendation});
  check(boundary_result.at("recommendation").at("scenarios")[1].at("coverage").at("slack") == (Money{1} << 51) - 300,
        "combined upper-bound cash slack remains an exact JSON integer");
}
}  // namespace
int main() {
  try {
    parsing_tests();
    result_tests();
    liquidity_tests();
    std::cout << "Integration contract: " << assertions << " assertions passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Integration contract failure after " << assertions << " assertions: " << error.what() << '\n';
    return 1;
  }
}
