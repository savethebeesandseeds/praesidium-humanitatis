// SPDX-License-Identifier: MIT
#include "simulation.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Json = nlohmann::json;
using Amount = std::int64_t;
int checks = 0;
void require(bool good, const std::string& message) { ++checks; if (!good) throw std::runtime_error(message); }
Amount n(const Json& value, const char* key) { return value.at(key).get<Amount>(); }
Json run(const Json& config) { return Json::parse(ph::exchange::run_json(Json{{"op", "simulate"}, {"config", config}}.dump())); }
Json run(const Json& config, const Json& history) {
  return Json::parse(ph::exchange::run_json(Json{{"op", "simulate"}, {"config", config}, {"history", history}}.dump()));
}
Json defaults() { return Json::parse(ph::exchange::run_json("{\"op\":\"defaults\"}")).at("config"); }
void reject(const Json& config, const std::string& message) { require(run(config).at("status") == "error", message); }

void near(double actual, double expected, const std::string& message) {
  require(std::abs(actual - expected) <= 1e-9 * std::max(1.0, std::abs(expected)), message);
}
const Json& candidate_at_price(const Json& product, Amount price) {
  for (const auto& candidate : product.at("candidate_forecasts"))
    if (n(candidate, "price") == price) return candidate;
  throw std::runtime_error("published price is missing from the candidate evidence");
}

// Check the explanation against the recorded quantities and accounts, without
// reproducing the forecast or optimizer. A comparison changes one product only;
// the other products retain their published prices and share one funding need.
void verify_price_comparisons(const Json& config, const Json& row) {
  const auto& scenarios = config.at("scenarios");
  const auto& products = row.at("products");
  const Amount fixed = n(row, "worker_wages_due") + n(row, "operating_cost_due") + n(row, "reserve_requirement");
  std::vector<Amount> published_contribution(scenarios.size(), 0);
  for (const auto& product : products) {
    const auto& candidate = candidate_at_price(product, n(product, "selected_price"));
    for (std::size_t s = 0; s < scenarios.size(); ++s)
      published_contribution[s] += (n(candidate, "price") - n(product, "unit_cost")) * candidate.at("saleable_scenario_units")[s].get<Amount>();
  }
  for (const auto& product : products) {
    const auto& chosen = candidate_at_price(product, n(product, "selected_price"));
    std::size_t published_count = 0;
    for (const auto& candidate : product.at("candidate_forecasts")) {
      const auto& comparison = candidate.at("price_comparison");
      require(comparison.at("scenario_contribution").size() == scenarios.size() &&
              comparison.at("scenario_funding_balance").size() == scenarios.size(), "candidate explanation preserves every forecast scenario");
      double expected_units = 0, expected_contribution = 0, expected_balance = 0, score = 0;
      Amount minimum_slack = std::numeric_limits<Amount>::max();
      for (std::size_t s = 0; s < scenarios.size(); ++s) {
        const double probability = scenarios[s].at("probability").get<double>();
        const Amount units = candidate.at("saleable_scenario_units")[s].get<Amount>();
        const Amount contribution = (n(candidate, "price") - n(product, "unit_cost")) * units;
        const Amount chosen_contribution = (n(chosen, "price") - n(product, "unit_cost")) * chosen.at("saleable_scenario_units")[s].get<Amount>();
        const Amount surplus = published_contribution[s] - chosen_contribution + contribution - fixed;
        const Amount balance = n(row, "feedback_adjustment") + surplus;
        require(comparison.at("scenario_contribution")[s] == contribution, "candidate product contribution subtracts replacement cost");
        require(comparison.at("scenario_funding_balance")[s] == balance, "candidate balance changes one published price and deducts shared funding needs once");
        expected_units += probability * units; expected_contribution += probability * contribution;
        expected_balance += probability * balance; score += probability * std::abs(balance);
        minimum_slack = std::min(minimum_slack, surplus + n(row, "coverage_credit") + n(row, "liquidity_buffer"));
      }
      near(comparison.at("expected_units").get<double>(), expected_units, "candidate expected units use configured scenario weights");
      near(comparison.at("expected_contribution").get<double>(), expected_contribution, "candidate expected contribution reconciles");
      near(comparison.at("expected_funding_balance").get<double>(), expected_balance, "candidate expected balance reconciles");
      near(comparison.at("hypothetical_balance_score").get<double>(), score, "candidate hypothetical score uses absolute scenario balances");
      require(n(comparison, "minimum_coverage_slack") == minimum_slack, "coverage slack includes explicit support without treating it as earned income");
      const bool published = n(candidate, "price") == n(product, "selected_price");
      require(comparison.at("published") == published, "candidate publication marker identifies the actual public price");
      published_count += published ? 1 : 0;
      require(comparison.at("admissible").is_boolean() && comparison.at("exclusion_reasons").is_array(), "candidate decision status is explicit");
      require(comparison.at("admissible").get<bool>() == comparison.at("exclusion_reasons").empty(), "excluded candidates state a reason");
      if (published && row.at("status") == "recommended") {
        require(comparison.at("admissible").get<bool>(), "recommended public prices pass their own explanation check");
        near(score, row.at("expected_absolute_balance").get<double>(), "published explanation agrees with the certified optimization score");
        require(comparison.at("scenario_funding_balance") == row.at("scenario_funding_balance"), "published candidate preserves certified scenario accounts");
      }
      if (!comparison.at("affordable_alternative_price").is_null()) {
        const Amount alternative_price = n(comparison, "affordable_alternative_price");
        const auto& alternative = candidate_at_price(product, alternative_price);
        require(alternative_price < n(candidate, "price"), "affordability witness is a cheaper candidate");
        for (std::size_t s = 0; s < scenarios.size(); ++s) {
          const Amount alternative_units = alternative.at("saleable_scenario_units")[s].get<Amount>();
          require(alternative_units >= candidate.at("saleable_scenario_units")[s].get<Amount>() &&
                  (alternative_price - n(product, "unit_cost")) * alternative_units >= comparison.at("scenario_contribution")[s].get<Amount>(),
                  "affordability witness serves and contributes at least as much in every scenario");
        }
        require(!comparison.at("admissible").get<bool>(), "guard witness excludes the higher price");
      }
    }
    require(published_count == 1, "each product has exactly one published candidate explanation");
  }
}

// Independent transaction identities, without reproducing the procurement,
// pricing, forecasting or FIFO implementation.
void verify_accounts(const Json& result) {
  require(result.at("status") == "ok", "simulation failed: " + result.dump());
  require(result.at("schema_version") == "exchange.sim.v3", "current simulation schema");
  require(result.at("objective_version") == 2, "affordability-protected objective version is recorded");
  const auto& config = result.at("config");
  for (const auto* mode : {"optimized", "fixed"}) {
    const auto& path = result.at(mode); const auto& summary = path.at("summary");
    Amount cumulative = 0, balance = 0, scheduled = 0, revenue = 0, wages = 0, purchases = 0;
    Amount cash = n(config, "initial_cash"), reserve = n(config, "initial_reserve"), arrears = 0;
    Amount maximum_target = reserve, inventory = n(summary, "initial_inventory_value");
    for (const auto& row : path.at("rows")) {
      verify_price_comparisons(config, row);
      require(n(row, "opening_cash") == cash && n(row, "opening_reserve") == reserve &&
              n(row, "opening_inventory_value") == inventory, "opening balances carry executed state between days");
      require(n(row, "closing_cash") == cash - n(row, "procurement") + n(row, "revenue") -
              n(row, "worker_wages_paid") - n(row, "operating_cost_paid"), "cash reconciles without an assurance payout");
      require(n(row, "economic_result") == n(row, "revenue") - n(row, "cost_of_goods_sold") - n(row, "waste_cost") -
              n(row, "worker_wages_due") - n(row, "operating_cost_due"), "economic costs exclude procurement and reserve transfers");
      cumulative += n(row, "economic_result"); revenue += n(row, "revenue");
      wages += n(row, "worker_wages_paid"); purchases += n(row, "procurement");
      require(n(row, "funding_balance_before") == balance, "feedback carries realized funding history");
      scheduled += n(row, "reserve_requirement"); balance += n(row, "economic_result") - n(row, "reserve_requirement");
      require(n(row, "funding_balance_after") == balance && balance == cumulative - scheduled &&
              n(row, "cumulative_reserve_requirement") == scheduled, "new reserve requirements enter earned history exactly once");
      maximum_target = std::max(maximum_target, n(row, "reserve_target"));
      require(scheduled <= maximum_target - n(config, "initial_reserve") && n(row, "reserve_requirement") >= 0 &&
              n(row, "reserve_requirement") <= n(config, "reserve_contribution"), "reserve requirements respect highest target increment and daily cap");
      require(n(row, "actual_funding_result") == n(row, "actual_contribution") - n(row, "actual_required"), "actual funding shortfall is explicit");
      const Amount liquid = std::max<Amount>(0, cash - n(row, "procurement") - arrears);
      require(n(row, "coverage_credit") >= 0 && n(row, "liquidity_buffer") >= 0 &&
              n(row, "coverage_credit") + n(row, "liquidity_buffer") == liquid, "credit and liquidity partition spendable cash without duplication");
      require(n(row, "coverage_credit") <= std::max<Amount>(0, n(row, "funding_balance_before")) &&
              n(row, "coverage_credit") <= std::max<Amount>(0, liquid - reserve), "earned credit excludes opening endowment and committed cash");
      const Amount adjustment = n(row, "feedback_adjustment"), before = n(row, "funding_balance_before");
      require((before == 0 && adjustment == 0) || (before > 0 && adjustment > 0) || (before < 0 && adjustment < 0),
              "amortization preserves realized history sign");
      require(n(row, "closing_cash") >= 0 && n(row, "reserve_balance") >= 0 && n(row, "reserve_balance") <= n(row, "closing_cash"), "cash and reserve are real and nonnegative");
      require(n(row, "reserve_balance") == reserve - n(row, "reserve_released") + n(row, "reserve_allocated"), "reserve transfers reconcile");
      require(n(row, "closing_cash") + n(row, "closing_inventory_value") - n(row, "wage_arrears") - n(row, "operating_arrears") ==
              n(config, "initial_cash") + n(summary, "initial_inventory_value") + cumulative, "earned economics reconciles assets less arrears");
      require(row.at("status") == "recommended" || row.at("status") == "continuity", "every executed day identifies its pricing basis");
      for (const auto* error : {"cash_reconciliation_error", "equity_reconciliation_error", "inventory_reconciliation_error", "valuation_reconciliation_error"})
        require(n(row, error) == 0, "reported reconciliation is exact");
      Amount product_revenue = 0;
      for (std::size_t i = 0; i < row.at("products").size(); ++i) {
        const auto& product = row.at("products")[i];
        require(product.at("selected_price").is_number_integer() && n(product, "selected_price") > 0, "continuity keeps a non-null public price");
        require(n(product, "opening_stock") + n(product, "purchased_units") == n(product, "sales_units") + n(product, "waste_units") + n(product, "closing_stock"), "stock units are conserved");
        require(n(product, "opening_inventory_value") + n(product, "purchase_cost") == n(product, "cost_of_goods_sold") + n(product, "waste_cost") + n(product, "closing_inventory_value"), "inventory book value is conserved");
        require(n(product, "actual_demand") == n(product, "sales_units") + n(product, "unmet_demand"), "unmet requests remain distinct from sales");
        require(n(product, "revenue") == n(product, "selected_price") * n(product, "sales_units"), "sales use the posted integer price");
        Amount lot_units = 0, lot_value = 0;
        for (const auto& lot : product.at("closing_lots")) { lot_units += n(lot, "units"); lot_value += n(lot, "units") * n(lot, "unit_cost"); }
        require(lot_units == n(product, "closing_stock") && lot_value == n(product, "closing_inventory_value"), "acquisition lots reconcile to inventory");
        if (row.at("status") == "continuity") {
          require(n(product, "selected_price") == n(product, "previous_price") && product.at("forecast_units").is_null(), "continuity holds prices without forging a certified forecast");
        } else if (std::string(mode) == "optimized") {
          const auto price = n(product, "selected_price"), previous = n(product, "previous_price");
          require((adjustment == 0 && price == previous) || (adjustment > 0 && price <= previous) || (adjustment < 0 && price >= previous), "actual feedback determines allowed price direction");
        } else require(n(product, "selected_price") == n(config.at("products")[i], "fixed_price"), "baseline keeps configured fixed prices");
        product_revenue += n(product, "revenue");
      }
      require(product_revenue == n(row, "revenue") && n(row.at("consumers"), "spending") == product_revenue, "consumer spending equals supplier revenue");
      require(n(row.at("consumers"), "opening_budget") == n(row.at("consumers"), "remaining_budget") + product_revenue, "consumer budgets reconcile exactly");
      if (row.at("status") == "recommended") {
        for (std::size_t s = 0; s < row.at("scenario_worker_surplus").size(); ++s) {
          const auto surplus = row.at("scenario_worker_surplus")[s].get<Amount>();
          require(surplus + n(row, "coverage_credit") + n(row, "liquidity_buffer") >= 0, "certified coverage uses explicit disjoint cash");
          require(row.at("scenario_funding_balance")[s] == adjustment + surplus, "cash support is not earned projected balance");
        }
      } else require(row.at("expected_absolute_balance").is_null(), "continuity has no fabricated optimization score");
      cash = n(row, "closing_cash"); reserve = n(row, "reserve_balance"); inventory = n(row, "closing_inventory_value");
      arrears = n(row, "wage_arrears") + n(row, "operating_arrears");
    }
    require(n(summary, "economic_result") == cumulative && n(summary, "revenue") == revenue && n(summary, "worker_wages_paid") == wages &&
            n(summary, "procurement") == purchases, "summary transactions equal executed rows");
    require(n(summary, "closing_cash") == cash && n(summary, "closing_inventory_value") == inventory && n(summary, "reserve_balance") == reserve &&
            n(summary, "funding_balance") == balance && n(summary, "cumulative_reserve_requirement") == scheduled, "no unrecorded transaction survives path termination");
    if (summary.at("terminal_status") == "insolvent") {
      require(cash == 0, "insolvency requires exhausted cash");
      require(n(summary, "terminal_day") == (path.at("rows").empty() ? 0 : n(path.at("rows").back(), "day")), "insolvency day ends the observed horizon");
    }
    std::set<std::string> event_ids; Amount requests = 0;
    for (const auto& event : path.at("events")) {
      require(event.at("schema_version") == "exchange.assurance.v1" && event.at("settlement_status") == "unfunded_request", "assurance is an unfunded interface without implicit payout");
      require(event_ids.insert(event.at("id").get<std::string>()).second && n(event, "required_support") >= 0, "assurance events have unique IDs and nonnegative requested support");
      if (event.at("type") == "assurance_requested") ++requests;
    }
    require(n(summary, "assurance_requests") == requests, "assurance count matches events");
  }
}

Json simple() {
  auto c = defaults(); c["periods"] = 3; c["worker_wages"] = 1; c["operating_cost"] = 0;
  c["initial_reserve"] = 0; c["reserve_target"] = 0; c["reserve_contribution"] = 0;
  c["shock"] = {{"start_day", 0}, {"end_day", 0}, {"demand_factor_bps", 10000}, {"cost_factor_bps", 10000}};
  c["forecast"]["sigma_multiplier_bps"] = 0; c["forecast"]["prior_sigma_units"] = 0;
  c["products"].erase(1); auto& p = c["products"][0]; p["candidate_prices"] = {200}; p["spoilage_bps"] = 0;
  p["initial_stock"] = 100; p["target_stock"] = 100; p["base_demand"] = 0; p["elasticity_bps"] = 0;
  c["consumers"]["potential_visitors"] = 0;
  return c;
}

void explanation_reasons() {
  auto c = simple(); c["periods"] = 2;
  c["products"][0]["candidate_prices"] = {160, 200, 220};
  c["products"][0]["max_change_bps"] = 10000;
  const auto result = run(c); verify_accounts(result);
  const auto& rows = result.at("optimized").at("rows");
  const auto& neutral = candidate_at_price(rows[0].at("products")[0], 160).at("price_comparison");
  require(neutral.at("exclusion_reasons") == Json::array({"operating balance price direction violated for bread"}),
          "opening neutral balance explains why cheaper prices are excluded");
  const auto& guarded = candidate_at_price(rows[1].at("products")[0], 220).at("price_comparison");
  require(n(guarded, "affordable_alternative_price") == 200 && guarded.at("admissible") == false &&
          guarded.at("exclusion_reasons") == Json::array({"affordable alternative 1 preserves scenario provision and contribution for bread"}),
          "zero-demand equal-contribution comparison names the cheaper permitted hold instead of inventing a lower score");
  near(guarded.at("hypothetical_balance_score").get<double>(), rows[1].at("expected_absolute_balance").get<double>(),
       "affordability exclusion can apply even when financial scores tie");
  c = simple(); c["periods"] = 1; c["initial_cash"] = 100; c["worker_wages"] = 1000;
  const auto unfunded = run(c); verify_accounts(unfunded);
  const auto& row = unfunded.at("optimized").at("rows")[0];
  const auto& published = row.at("products")[0].at("candidate_forecasts")[0].at("price_comparison");
  require(row.at("status") == "continuity" && row.at("expected_absolute_balance").is_null() &&
          row.at("expected_funding_balance").is_null() && published.at("published") == true && published.at("admissible") == false,
          "continuity publication is not a certified feasible choice");
  require(n(published, "minimum_coverage_slack") == -900 && published.at("hypothetical_balance_score") == 1000 &&
          published.at("exclusion_reasons") == Json::array({
              "wages, operating costs, and reserve not covered in scenario adverse",
              "wages, operating costs, and reserve not covered in scenario expected",
              "wages, operating costs, and reserve not covered in scenario high"}),
          "continuity retains hypothetical arithmetic and identifies each uncovered scenario without fabricating an optimal score");
}

void continuity_and_insolvency() {
  auto c = simple(); c["periods"] = 10; c["initial_cash"] = 2500; c["worker_wages"] = 1000;
  c["products"][0]["initial_stock"] = 0; c["products"][0]["target_stock"] = 0;
  const auto exhausted = run(c); verify_accounts(exhausted);
  for (const auto* mode : {"optimized", "fixed"}) {
    const auto& path = exhausted.at(mode);
    require(path.at("rows").size() == 3 && path.at("summary").at("terminal_status") == "insolvent" && n(path.at("summary"), "terminal_day") == 3,
            "2500 cash funds three 1000-cost days with final arrears");
    require(n(path.at("rows")[0], "closing_cash") == 1500 && n(path.at("rows")[1], "closing_cash") == 500 &&
            n(path.at("rows")[2], "closing_cash") == 0 && n(path.at("summary"), "wage_arrears") == 500, "cash exhaustion pays only available money");
    require(path.at("rows")[2].at("status") == "continuity" && n(path.at("summary"), "funding_balance") == -3000, "forecast failure executes continuity until cash is exhausted");
  }
  c["initial_cash"] = 0; const auto opening = run(c); verify_accounts(opening);
  require(opening.at("optimized").at("rows").empty() && n(opening.at("optimized").at("summary"), "terminal_day") == 0 &&
          n(opening.at("optimized").at("summary"), "worker_wages_due") == 0, "opening insolvency does not accrue unexecuted future wages");
  c["initial_cash"] = 100000; c["periods"] = 3; c["reserve_target"] = 100000; c["reserve_contribution"] = 100000;
  const auto reserve_shortfall = run(c); verify_accounts(reserve_shortfall); const auto& rows = reserve_shortfall.at("optimized").at("rows");
  require(rows.size() == 3 && rows[0].at("status") == "continuity" && n(rows[0], "closing_cash") == 99000 &&
          reserve_shortfall.at("optimized").at("summary").at("terminal_status") == "completed", "reserve forecast shortfall cannot close a cash-funded exchange");
  require(n(rows[0], "reserve_requirement") == 100000 && n(rows[1], "reserve_requirement") == 0 && n(rows[2], "reserve_requirement") == 0,
          "reserve release cannot schedule the same requirement again");
  c["initial_cash"] = 2500; c["initial_reserve"] = 2000; c["reserve_target"] = 2000; c["reserve_contribution"] = 100;
  c["forecast"]["reserve_horizon_days"] = 1;
  const auto released = run(c); verify_accounts(released);
  require(n(released.at("optimized").at("rows")[0], "reserve_released") == 500 && n(released.at("optimized").at("rows")[0], "funding_balance_after") == -1000,
          "releasing endowed reserve never creates earned feedback");
}

void fifo_reference() {
  auto c = simple(); c["periods"] = 2; c["initial_cash"] = 10000;
  c["products"][0]["initial_stock"] = 3; c["products"][0]["target_stock"] = 5; c["products"][0]["spoilage_bps"] = 2000;
  c["shock"] = {{"start_day", 1}, {"end_day", 1}, {"demand_factor_bps", 10000}, {"cost_factor_bps", 20000}};
  const auto result = run(c); verify_accounts(result); const auto& rows = result.at("optimized").at("rows");
  require(n(rows[0], "procurement") == 400 && n(rows[0], "waste_cost") == 100 && n(rows[0], "closing_cash") == 9599 && n(rows[0], "closing_inventory_value") == 600,
          "first FIFO day discards one old 100-cost unit before new 200-cost stock");
  require(n(rows[1], "procurement") == 100 && n(rows[1], "waste_cost") == 100 && n(rows[1], "closing_cash") == 9498 && n(rows[1], "closing_inventory_value") == 600 &&
          n(result.at("optimized").at("summary"), "economic_result") == -202, "hand-calculated FIFO book values survive falling replacement costs");
  const auto& lots = rows[1].at("products")[0].at("closing_lots");
  require(lots.size() == 3 && n(lots[0], "units") == 1 && n(lots[0], "unit_cost") == 100 && n(lots[1], "units") == 2 &&
          n(lots[1], "unit_cost") == 200 && n(lots[2], "acquired_day") == 2, "remaining lots preserve their acquisition order and dates");
}

void actual_feedback() {
  auto c = simple(); c["periods"] = 2; c["feedback_recovery_days"] = 1;
  c["consumers"]["potential_visitors"] = 20; c["consumers"]["visit_probability_bps"] = 10000; c["consumers"]["need_probability_bps"] = 10000;
  c["consumers"]["max_units_per_product"] = 1; c["consumers"]["budget_min"] = 10000; c["consumers"]["budget_max"] = 10000;
  auto& p = c["products"][0]; p["base_demand"] = 20; p["affordability_ceiling"] = 250; p["max_change_bps"] = 10000;
  p["candidate_prices"] = {100, 125, 150, 175, 200, 225, 250}; c["worker_wages"] = 500;
  const auto good = run(c); verify_accounts(good); const auto& good_rows = good.at("optimized").at("rows");
  require(n(good_rows[0], "sales_units") == 20 && n(good_rows[0], "economic_result") == 1500 && n(good_rows[0], "funding_balance_after") == 1500,
          "twenty actual seeded customers create exact earned surplus");
  require(n(good_rows[0].at("products")[0], "selected_price") == 200 && n(good_rows[1].at("products")[0], "selected_price") < 200,
          "opening holds and good actual sales reduce the following public price");
  auto extra_cash = c; extra_cash["initial_cash"] = 1000000; const auto funded = run(extra_cash);
  require(funded.at("optimized").at("rows")[1].at("products")[0].at("selected_price") == good_rows[1].at("products")[0].at("selected_price"),
          "more initial endowment does not invent earned feedback");
  c["worker_wages"] = 3000; const auto bad = run(c); verify_accounts(bad); const auto& bad_rows = bad.at("optimized").at("rows");
  require(n(bad_rows[0], "economic_result") == -1000 && n(bad_rows[1], "funding_balance_before") == -1000 && n(bad_rows[1].at("products")[0], "selected_price") == 250,
          "an actual gap raises the price when supplied demand preserves contribution");
  require(n(bad_rows[1], "coverage_credit") == 0 && n(bad_rows[1], "liquidity_buffer") > 0, "negative earned history uses liquidity without earned credit");
}

void history_and_uncertainty() {
  auto c = simple(); c["periods"] = 2; c["products"][0]["base_demand"] = 10;
  c["forecast"]["min_history"] = 2; c["forecast"]["prior_sigma_units"] = 2;
  const Json history = {{"schema_version", "exchange.history.v1"}, {"observations", Json::array({
      {{"day", -2}, {"sku", "bread"}, {"price", 200}, {"sales_units", 20}, {"stockout", false}},
      {{"day", -1}, {"sku", "bread"}, {"price", 200}, {"sales_units", 30}, {"stockout", true}}})}};
  const auto observed = run(c, history); verify_accounts(observed); const auto& rows = observed.at("optimized").at("rows");
  const auto& first = rows[0].at("products")[0].at("forecast_before_sale"); const auto& second = rows[1].at("products")[0].at("forecast_before_sale");
  require(first.at("mean") == 12.5 && first.at("eligible_history") == 1 && first.at("warmed_up") == false, "negative history learns only the uncensored observation");
  require(second.at("mean") == 9.375 && second.at("eligible_history") == 2 && second.at("warmed_up") == true, "forecast learns only completed sales and exposes warmup");
  auto stockout = c; stockout["products"][0]["initial_stock"] = 0; stockout["products"][0]["target_stock"] = 0;
  const auto censored = run(stockout, history); verify_accounts(censored);
  require(censored.at("optimized").at("rows")[0].at("products")[0].at("forecast_update").at("stock_censored") == true &&
          censored.at("optimized").at("rows")[1].at("products")[0].at("forecast_before_sale").at("mean") == 12.5, "stockouts do not teach zero latent demand");
  auto future = c; future["shock"] = {{"start_day", 2}, {"end_day", 2}, {"demand_factor_bps", 0}, {"cost_factor_bps", 10000}};
  const auto shocked = run(future, history);
  require(shocked.at("optimized").at("rows")[0] == rows[0] && shocked.at("optimized").at("rows")[1].at("products")[0].at("forecast_before_sale") == second,
          "future shocks cannot leak into history-only forecasts");
  auto narrow = c; narrow["consumers"]["potential_visitors"] = 100;
  auto wide = narrow; wide["periods"] = 1; wide["forecast"]["sigma_multiplier_bps"] = 30000;
  const auto narrow_run = run(narrow), wide_run = run(wide);
  require(n(wide_run.at("optimized").at("rows")[0], "reserve_target") > n(narrow_run.at("optimized").at("rows")[0], "reserve_target"), "wider sigma stress increases planned reserves");
  auto invalid = history; invalid["observations"][0]["day"] = 0; require(run(c, invalid).at("status") == "error", "current/future imported observations rejected");
  invalid = history; invalid["observations"][1]["day"] = -2; require(run(c, invalid).at("status") == "error", "duplicate/reversed history days rejected");
  invalid = history; invalid["observations"][0]["sku"] = "unknown"; require(run(c, invalid).at("status") == "error", "unknown history SKU rejected");
}

void catalog_and_limits() {
  auto c = simple(); c["periods"] = 3; const auto product = c.at("products")[0]; c["products"] = Json::array();
  c["reserve_target"] = 100; c["reserve_contribution"] = 10;
  // Keep nonzero cold-start forecasts with no realized visits: higher prices
  // then have distinct contributions, so the affordability guard cannot fold
  // the candidate grid before this search-budget/transaction-rollback test.
  c["consumers"]["potential_visitors"] = 1; c["consumers"]["visit_probability_bps"] = 0;
  for (int i = 0; i < 12; ++i) {
    auto p = product; p["sku"] = "staple-" + std::to_string(i); p["label"] = "Staple " + std::to_string(i);
    p["candidate_prices"] = {200, 220}; p["max_change_bps"] = 10000; p["initial_stock"] = 1; p["target_stock"] = 2; p["spoilage_bps"] = 10000;
    p["base_demand"] = 1;
    c["products"].push_back(p);
  }
  const auto expanded = run(c); verify_accounts(expanded);
  require(expanded.at("optimized").at("rows").size() == 3 && expanded.at("optimized").at("summary").at("products").size() == 12,
          "all twelve configured products participate in consumer and stock accounting");
  c["max_price_combinations"] = 1; const auto refused = run(c); verify_accounts(refused);
  require(refused.at("optimized").at("summary").at("terminal_status") == "model_error" && n(refused.at("optimized").at("summary"), "terminal_day") == 2 &&
          refused.at("optimized").at("rows").size() == 1, "bounded-search failure refuses an unpriced second day and rolls back unexecuted transactions");
  require(refused.at("comparison").at("equal_observed_horizons") == false, "comparison identifies unequal observed horizons");
  auto too_many = c; auto extra = too_many["products"][0]; extra["sku"] = "thirteenth"; too_many["products"].push_back(extra); reject(too_many, "catalog bound rejects thirteen products");
}

void replay_and_validation() {
  auto c = defaults(); c["periods"] = 30; const auto result = run(c); verify_accounts(result);
  require(result == run(c), "same config, history and seed replay exactly");
  auto other = c; other["seed"] = 43; require(run(other).at("optimized").at("rows") != result.at("optimized").at("rows"), "seed changes actual consumer behavior");
  const auto days = std::min(result.at("optimized").at("rows").size(), result.at("fixed").at("rows").size());
  for (std::size_t day = 0; day < days; ++day) {
    const auto& a = result.at("optimized").at("rows")[day]; const auto& b = result.at("fixed").at("rows")[day];
    require(n(a, "visitors") == n(b, "visitors") && n(a, "no_visit") == n(b, "no_visit") &&
            n(a.at("consumers"), "opening_budget") == n(b.at("consumers"), "opening_budget"), "paired policies share arrivals and customer opening budgets");
    require(a.at("shock_active") == (day + 1 >= 15 && day + 1 <= 20), "shock interval is inclusive");
  }
  auto invalid = defaults(); invalid["unknown"] = 1; reject(invalid, "unknown config field rejected");
  invalid = defaults(); invalid["products"][0]["candidate_prices"][0] = 160.0; reject(invalid, "float money rejected");
  invalid = defaults(); invalid["initial_cash"] = true; reject(invalid, "boolean money rejected");
  invalid = defaults(); invalid["seed"] = 18446744073709551615ULL; reject(invalid, "unsigned overflow rejected");
  invalid = defaults(); invalid["periods"] = 366; reject(invalid, "period count bounded");
  invalid = defaults(); invalid["products"][0]["candidate_prices"].push_back(160); reject(invalid, "duplicate candidate rejected");
  invalid = defaults(); invalid["products"][0]["unit_cost"] = 1000001; reject(invalid, "price bound protects arithmetic");
  invalid = defaults(); invalid["scenarios"][0]["probability"] = 0.8; reject(invalid, "probability sum validated");
  invalid = defaults(); invalid["shock"]["end_day"] = 0; reject(invalid, "malformed shock rejected");
  invalid = defaults(); invalid["feedback_recovery_days"] = 0; reject(invalid, "recovery horizon cannot be zero");
  invalid = defaults(); invalid["feedback_recovery_days"] = 31; reject(invalid, "recovery horizon bounded");
  invalid = defaults(); invalid["products"][0]["candidate_prices"] = {110, 120}; reject(invalid, "hold candidate required");
  invalid = defaults(); invalid["max_price_combinations"] = 200001; reject(invalid, "application cannot bypass search cap");
  invalid = defaults(); invalid["consumers"]["budget_min"] = 3000; invalid["consumers"]["budget_max"] = 1000; reject(invalid, "consumer budgets ordered");
  invalid = defaults(); invalid["forecast"]["model"] = "unspecified"; reject(invalid, "unsupported forecast model rejected");
  require(Json::parse(ph::exchange::run_json("{\"op\":\"defaults\",\"op\":\"defaults\"}")).at("status") == "error", "duplicate JSON keys rejected");
  require(Json::parse(ph::exchange::run_json("[1e10000]")).at("status") == "error", "JSON overflow rejected");
  require(Json::parse(ph::exchange::run_json(std::string("{\"op\":\"") + static_cast<char>(0xff) + "\"}")).at("status") == "error", "invalid UTF-8 returns well-formed error JSON");
  require(Json::parse(exchange_run("{\"op\":\"defaults\"}")).at("schema_version") == "exchange.sim.v3", "C export shares the current contract");
}
}  // namespace

int main() {
  try {
    explanation_reasons(); continuity_and_insolvency(); fifo_reference(); actual_feedback(); history_and_uncertainty(); catalog_and_limits(); replay_and_validation();
    std::cout << checks << " exchange simulation checks passed\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
