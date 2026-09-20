// SPDX-License-Identifier: MIT
#include "simulation.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using Json = nlohmann::json;
using Amount = std::int64_t;
int checks = 0;
void require(bool good, const std::string& message) { ++checks; if (!good) throw std::runtime_error(message); }
Json run(const Json& config) { return Json::parse(ph::exchange::run_json(Json{{"op", "simulate"}, {"config", config}}.dump())); }
Json defaults() { return Json::parse(ph::exchange::run_json("{\"op\":\"defaults\"}")).at("config"); }
Amount n(const Json& value, const char* key) { return value.at(key).get<Amount>(); }
void verify_accounts(const Json& result) {
  require(result.at("status") == "ok", "simulation failed: " + result.dump());
  const auto& config = result.at("config");
  for (const auto* mode : {"optimized", "fixed"}) {
    Amount cumulative = 0, previous_cash = n(config, "initial_cash"), revenue_total = 0, wages_total = 0;
    for (const auto& row : result.at(mode).at("rows")) {
      require(n(row, "opening_cash") == previous_cash, "periods carry cash forward");
      require(n(row, "closing_cash") == n(row, "opening_cash") - n(row, "procurement") + n(row, "revenue") - n(row, "worker_wages_paid") - n(row, "operating_cost_paid"), "cash ledger independent identity");
      require(n(row, "economic_result") == n(row, "revenue") - n(row, "cost_of_goods_sold") - n(row, "waste_cost") - n(row, "worker_wages_due") - n(row, "operating_cost_due"), "economic ledger excludes procurement and reserve transfers");
      cumulative += n(row, "economic_result"); revenue_total += n(row, "revenue"); wages_total += n(row, "worker_wages_paid");
      const auto assets_net = n(row, "closing_cash") + n(row, "closing_inventory_value") - n(row, "wage_arrears") - n(row, "operating_arrears");
      require(assets_net == n(config, "initial_cash") + n(result.at(mode).at("summary"), "initial_inventory_value") + cumulative, "economic result reconciles assets less arrears");
      require(n(row, "closing_cash") >= 0 && n(row, "reserve_balance") >= 0 && n(row, "reserve_balance") <= n(row, "closing_cash"), "cash and reserve cannot be negative or invented");
      require(n(row, "reserve_balance") == n(row, "opening_reserve") - n(row, "reserve_released") + n(row, "reserve_allocated"), "reserve allocation reconciles without being charged twice");
      for (const auto& product : row.at("products")) {
        require(n(product, "opening_stock") + n(product, "purchased_units") == n(product, "sales_units") + n(product, "waste_units") + n(product, "closing_stock"), "unit conservation");
        require(n(product, "opening_inventory_value") + n(product, "purchase_cost") == n(product, "cost_of_goods_sold") + n(product, "waste_cost") + n(product, "closing_inventory_value"), "FIFO inventory valuation conservation");
        require(n(product, "actual_demand") == n(product, "sales_units") + n(product, "unmet_demand"), "unmet demand accounted separately from sales");
        if (row.at("status") != "recommended") require(product.at("selected_price").is_null() && n(product, "sales_units") == 0, "failed decision cannot use an old price");
      }
      previous_cash = n(row, "closing_cash");
    }
    require(n(result.at(mode).at("summary"), "economic_result") == cumulative && n(result.at(mode).at("summary"), "revenue") == revenue_total && n(result.at(mode).at("summary"), "worker_wages_paid") == wages_total, "summary totals match rows");
  }
}
void reject(Json config, const std::string& message) { require(run(config).at("status") == "error", message); }
}
int main() {
  try {
    auto config = defaults(); config["periods"] = 40;
    const auto result = run(config);
    require(result == run(config), "same config and seed produce identical trajectories");
    verify_accounts(result);
    require(result.at("optimized").at("rows")[14].at("shock_active") == true &&
            result.at("optimized").at("rows")[20].at("shock_active") == false, "shock inclusive schedule and recovery");
    for (std::size_t day = 0; day < result.at("optimized").at("rows").size(); ++day)
      for (std::size_t product = 0; product < config.at("products").size(); ++product)
        require(result.at("optimized").at("rows")[day].at("products")[product].at("realized_noise_bps") == result.at("fixed").at("rows")[day].at("products")[product].at("realized_noise_bps"), "paired comparison shares exogenous draws");
    auto different_seed = config; different_seed["seed"] = 43;
    require(run(different_seed).at("optimized").at("rows") != result.at("optimized").at("rows"), "seed changes realized path");
    auto impossible = defaults(); impossible["periods"] = 3; impossible["worker_wages"] = 100000000;
    impossible["initial_cash"] = 0; impossible["initial_reserve"] = 0;
    const auto closed = run(impossible); verify_accounts(closed);
    require(n(closed.at("optimized").at("summary"), "sales_units") == 0 && n(closed.at("fixed").at("summary"), "sales_units") == 0, "infeasible pricing has no trades on either path");
    require(n(closed.at("optimized").at("summary"), "wage_arrears") == 300000000 && n(closed.at("optimized").at("summary"), "closing_cash") == 0, "unpaid obligations accrue instead of negative cash");
    auto stockout = defaults(); stockout["periods"] = 1; stockout["worker_wages"] = 1; stockout["operating_cost"] = 0;
    stockout["reserve_contribution"] = 0; stockout["demand_noise_bps"] = 0;
    stockout["products"].erase(1); stockout["products"][0]["candidate_prices"] = {200};
    stockout["products"][0]["initial_stock"] = 10; stockout["products"][0]["target_stock"] = 10;
    for (auto& scenario : stockout["scenarios"]) scenario["factor_bps"] = 4000;
    const auto shortage = run(stockout); verify_accounts(shortage);
    require(n(shortage.at("optimized").at("summary"), "sales_units") == 10 && n(shortage.at("optimized").at("summary"), "unmet_demand") == 10, "realized demand is independent of scenario assumptions and can exceed stock");
    auto simple = stockout; simple["products"][0]["initial_stock"] = 20; simple["products"][0]["target_stock"] = 20;
    simple["products"][0]["spoilage_bps"] = 0; simple["reserve_contribution"] = 100;
    const auto exact = run(simple); verify_accounts(exact);
    const auto& row = exact.at("optimized").at("rows")[0];
    require(n(row, "revenue") == 4000 && n(row, "cost_of_goods_sold") == 2000 && n(row, "economic_result") == 1999 && n(row, "reserve_allocated") == 100, "hand-computed accounting does not treat reserve as expense");
    auto no_reserve = simple; no_reserve["reserve_contribution"] = 0;
    require(run(no_reserve).at("optimized").at("summary").at("economic_result") == exact.at("optimized").at("summary").at("economic_result"), "earmarking reserve does not alter economic result with same decisions");
    auto cost_drop = simple; cost_drop["worker_wages"] = 3000; cost_drop["reserve_contribution"] = 0;
    for (auto& scenario : cost_drop["scenarios"]) scenario["factor_bps"] = 10000;
    cost_drop["shock"] = {{"start_day", 1}, {"end_day", 1}, {"demand_factor_bps", 10000}, {"cost_factor_bps", 0}};
    const auto cost_basis = run(cost_drop); verify_accounts(cost_basis);
    const auto& cost_row = cost_basis.at("optimized").at("rows")[0];
    require(cost_row.at("expected_worker_surplus") == 1000 && n(cost_row, "economic_result") == -1000,
            "forecast replacement-cost surplus can differ in sign from realized FIFO economics");
    require(cost_basis.at("forecast_cost_basis") == "current_replacement_cost" && cost_basis.at("realized_cost_basis") == "FIFO",
            "cost basis difference explicitly labeled");
    auto invalid = defaults(); invalid["unknown"] = 1; reject(invalid, "unknown field rejected");
    invalid = defaults(); invalid["products"][0]["candidate_prices"][0] = 160.0; reject(invalid, "float money rejected");
    invalid = defaults(); invalid["initial_cash"] = true; reject(invalid, "boolean money rejected");
    invalid = defaults(); invalid["seed"] = 18446744073709551615ULL; reject(invalid, "unsigned overflow rejected");
    invalid = defaults(); invalid["periods"] = 366; reject(invalid, "period count bounded");
    invalid = defaults(); invalid["products"][0]["candidate_prices"].push_back(160); reject(invalid, "duplicate candidate rejected");
    invalid = defaults(); invalid["products"][0]["unit_cost"] = 1000001; reject(invalid, "price bound prevents arithmetic overflow");
    invalid = defaults(); invalid["scenarios"][0]["probability"] = 0.8; reject(invalid, "probability sum validated");
    invalid = defaults(); invalid["shock"]["end_day"] = 0; reject(invalid, "malformed shock interval rejected");
    require(Json::parse(ph::exchange::run_json("{\"op\":\"defaults\",\"op\":\"defaults\"}")).at("status") == "error", "duplicate JSON keys rejected");
    require(Json::parse(ph::exchange::run_json("[1e10000]")).at("status") == "error", "numeric overflow rejected");
    require(Json::parse(ph::exchange::run_json(std::string("{\"op\":\"") + static_cast<char>(0xff) + "\"}")).at("status") == "error",
            "invalid UTF-8 still returns well-formed error JSON");
    require(Json::parse(exchange_run("{\"op\":\"defaults\"}")).at("status") == "ok", "WebAssembly C export shares contract");
    std::cout << checks << " exchange simulation checks passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
