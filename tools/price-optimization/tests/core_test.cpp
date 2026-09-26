// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#include "ph/price/engine.hpp"
#include "ampl_data.hpp"
#include "reference_oracle.hpp"
#include "feedback_fixtures.hpp"
#include "synthetic_request.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <locale>
#include <stdexcept>

namespace {
using namespace ph::price;
constexpr Timestamp now = 1800000000;
int assertions = 0;
void check(bool condition, const char* message) {
  ++assertions;
  if (!condition) throw std::runtime_error(message);
}
class FixedBackend final : public SolverBackend {
 public:
  explicit FixedBackend(RawSolution solution) : raw(std::move(solution)) {}
  RawSolution solve_raw(const Request&) override { ++calls; return raw; }
  RawSolution raw;
  int calls = 0;
};
RawSolution valid_raw(const Request& r) { return ph::price::test::exhaustive_oracle(r, now); }
class ThrowingBackend final : public SolverBackend {
  RawSolution solve_raw(const Request&) override { throw std::runtime_error("synthetic solver crash"); }
};

void test_money_and_policy() {
  const auto r = ph::price::example::synthetic_request(now);
  check(validate_request(r, now).ok(), "valid synthetic input");
  const auto checked = evaluate_selection(r, {1, 1}, now);
  check(checked.recommendation.has_value(), "valid selection accepted");
  check(checked.recommendation->scenario_worker_surplus == std::vector<Money>({2400, 1400}),
        "exact scenario surplus after wages, operating cost and reserve");
  check(std::abs(checked.recommendation->expected_worker_surplus - 2000.0) < 1e-9,
        "independent hand-calculated expected surplus");
  check(checked.recommendation->public_prices == std::vector<Money>({200, 300}), "one posted price per SKU");
  check(checked.recommendation->scenario_funding_balance == std::vector<Money>({2400, 1400}) &&
        checked.recommendation->expected_absolute_balance == 2000,
        "zero prior balance preserves exact scenario accounting");
  check(checked.recommendation->valid_until == now + 120, "recommendation expires at input age bound");
  check(!evaluate_selection(r, {3, 1}, now).recommendation, "affordability and change cap enforced");
  auto changed = r;
  changed.products[0].affordability_ceiling = 1000;
  check(!evaluate_selection(changed, {3, 1}, now).recommendation, "price-change cap independently enforced");
  changed = r;
  changed.products[0].max_change_basis_points = 10000;
  check(!evaluate_selection(changed, {3, 1}, now).recommendation, "affordability independently enforced");
  changed = r;
  changed.products[0].inventory = 15;
  check(!evaluate_selection(changed, {1, 1}, now).recommendation, "inventory cannot be exceeded or silently truncated");
  changed = r;
  changed.worker_wage_floor = 2500;
  check(!evaluate_selection(changed, {1, 1}, now).recommendation, "coverage enforced in low-demand scenario");
  changed = r;
  changed.reserve_floor = 1600;
  check(!evaluate_selection(changed, {1, 1}, now).recommendation, "reserve protected independently");
  changed = r;
  changed.operating_cost = 1600;
  check(!evaluate_selection(changed, {1, 1}, now).recommendation, "operating costs protected independently");
  check(!evaluate_selection(r, {1}, now).recommendation, "partial selection rejected");
  check(!evaluate_selection(r, {100, 1}, now).recommendation, "invalid candidate rejected");
  const auto raw = valid_raw(r);
  check(raw.status == BackendStatus::optimal && std::abs(raw.expected_worker_surplus - 2000) < 1e-9,
        "exhaustive oracle agrees with independent fixture objective");
  check(raw.choices == std::vector<double>({0, 1, 0, 0, 0, 1, 0}), "oracle selects expected candidates");

  changed = r;
  changed.funding_balance = 1;
  changed.products = {{"staple", 100, 220, 220, 100, 1000,
                       {{200, {100, 0}}, {220, {10, 10}}}}};
  const auto robust = valid_raw(changed);
  check(robust.choices == std::vector<double>({0, 1}), "higher expectation cannot override low-demand wage floor");
  check(std::abs(robust.expected_worker_surplus - 200) < 1e-9, "robust fixture objective");
  changed.worker_wage_floor = 2000;
  check(valid_raw(changed).status == BackendStatus::infeasible, "infeasible fixture has no fallback selection");
}

void test_invalid_inputs() {
  const auto r = ph::price::example::synthetic_request(now);
  std::vector<std::function<void(Request&)>> mutations = {
    [](Request& q) { q.request_id.clear(); },
    [](Request& q) { q.forecast_source.clear(); },
    [](Request& q) { q.worker_policy_reference.clear(); },
    [](Request& q) { q.currency = "eur"; },
    [](Request& q) { q.as_of = now + 1; },
    [](Request& q) { q.as_of = -1; },
    [](Request& q) { q.valid_until = now; },
    [](Request& q) { q.max_input_age_seconds = 0; },
    [](Request& q) { q.max_input_age_seconds = 86401; },
    [](Request& q) { q.worker_wage_floor = 0; },
    [](Request& q) { q.operating_cost = -1; },
    [](Request& q) { q.reserve_floor = std::numeric_limits<Money>::max(); },
    [](Request& q) { q.funding_balance = (Money{1} << 50) + 1; },
    [](Request& q) { q.funding_balance = -(Money{1} << 50) - 1; },
    [](Request& q) { q.coverage_credit = -1; },
    [](Request& q) { q.liquidity_buffer = -1; },
    [](Request& q) { q.liquidity_buffer = (Money{1} << 50) + 1; },
    [](Request& q) { q.funding_balance = 1; q.coverage_credit = (Money{1} << 50) + 1; },
    [](Request& q) { q.coverage_credit = 1; },
    [](Request& q) { q.funding_balance = -1; q.coverage_credit = 1; },
    [](Request& q) { q.scenarios[0].probability = std::numeric_limits<double>::quiet_NaN(); },
    [](Request& q) { q.scenarios[0].probability = std::numeric_limits<double>::infinity(); },
    [](Request& q) { q.scenarios[0].probability = 0; },
    [](Request& q) { q.scenarios[0].probability = 0.9; },
    [](Request& q) { q.scenarios[0].id = q.scenarios[1].id; },
    [](Request& q) { q.products[0].sku = q.products[1].sku; },
    [](Request& q) { q.products[0].inventory = -1; },
    [](Request& q) { q.products[0].previous_price = 0; },
    [](Request& q) { q.products[0].previous_price = 210; },
    [](Request& q) { q.products[0].unit_cost = -1; },
    [](Request& q) { q.products[0].affordability_ceiling = 0; },
    [](Request& q) { q.products[0].max_change_basis_points = 10001; },
    [](Request& q) { q.products[0].candidates[0].price = 0; },
    [](Request& q) { q.products[0].candidates[0].price = std::numeric_limits<Money>::max(); },
    [](Request& q) { q.products[0].candidates[0].price = q.products[0].candidates[1].price; },
    [](Request& q) { q.products[0].candidates[0].forecast_units.pop_back(); },
    [](Request& q) { q.products[0].candidates[0].forecast_units[0] = -1; },
    [](Request& q) { q.products[0].candidates[0].forecast_units[0] = 1000001; },
    [](Request& q) { q.products[0].candidates.clear(); },
    [](Request& q) { q.products.clear(); },
    [](Request& q) { q.scenarios.clear(); }
  };
  for (const auto& mutate : mutations) {
    auto invalid = r;
    mutate(invalid);
    check(!validate_request(invalid, now).ok(), "invalid input rejected");
    FixedBackend backend(valid_raw(r));
    const auto result = optimize(invalid, backend, [] { return now; });
    check(result.status == SolveStatus::invalid_input && !result.recommendation && backend.calls == 0,
          "invalid input never reaches backend");
  }
  check(!validate_request(r, now + 120).ok(), "input max age is exclusive");
  check(!validate_request(r, now + 300).ok(), "valid_until is exclusive");
  auto large = r;
  for (auto& p : large.products) {
    p.previous_price = 1000000000;
    p.affordability_ceiling = 1000000000;
    p.candidates = {{1000000000, {1000000, 1000000}}};
  }
  check(!validate_request(large, now).ok(), "aggregate arithmetic bound enforced before solve");
}

void test_solver_boundary() {
  const auto r = ph::price::example::synthetic_request(now);
  const auto raw = valid_raw(r);
  FixedBackend backend(raw);
  const auto result = optimize(r, backend, [] { return now; });
  check(result.status == SolveStatus::recommended && result.recommendation.has_value(), "validated optimal output accepted");
  for (const auto status : {BackendStatus::infeasible, BackendStatus::failed, BackendStatus::unavailable}) {
    backend.raw = raw;
    backend.raw.status = status;
    check(!optimize(r, backend, [] { return now; }).recommendation, "nonoptimal status cannot produce recommendation");
  }
  std::vector<std::function<void(RawSolution&)>> mutations = {
    [](RawSolution& q) { q.choices.pop_back(); },
    [](RawSolution& q) { q.choices[0] = 0.5; q.choices[1] = 0.5; },
    [](RawSolution& q) { q.choices[0] = 1; },
    [](RawSolution& q) { q.choices[1] = 0; },
    [](RawSolution& q) { q.choices[1] = -1; },
    [](RawSolution& q) { q.choices[1] = std::numeric_limits<double>::quiet_NaN(); },
    [](RawSolution& q) { q.choices[1] = std::numeric_limits<double>::infinity(); },
    [](RawSolution& q) { q.expected_worker_surplus += 100; },
    [](RawSolution& q) { q.expected_worker_surplus = std::numeric_limits<double>::quiet_NaN(); },
    [](RawSolution& q) { q.expected_absolute_balance += 100; },
    [](RawSolution& q) { q.expected_absolute_balance = std::numeric_limits<double>::quiet_NaN(); },
    [](RawSolution& q) { q.choices[1] = 0; q.choices[3] = 1; }
  };
  for (const auto& mutate : mutations) {
    backend.raw = raw;
    mutate(backend.raw);
    const auto rejected = optimize(r, backend, [] { return now; });
    check(rejected.status == SolveStatus::rejected_solution && !rejected.recommendation,
          "untrustworthy solver output rejected");
  }
  backend.raw = raw;
  int reads = 0;
  check(!optimize(r, backend, [&] { return reads++ == 0 ? now : now + 120; }).recommendation,
        "input expiring during solve rejected");
  reads = 0;
  check(!optimize(r, backend, [&] { return reads++ == 0 ? now : now - 1; }).recommendation,
        "clock rollback rejected");
  ThrowingBackend broken;
  check(!optimize(r, broken, [] { return now; }).recommendation, "backend exception fails closed");
  for (const auto* solver : {"cplex", "cbc", "gurobi", "highs arguments", "/path/to/highs", ""}) {
    auto unsupported = make_ampl_backend({"unused", solver});
    const auto rejected = optimize(r, *unsupported, [] { return now; });
    check(rejected.status == SolveStatus::solver_failed && !rejected.recommendation &&
          rejected.detail.find("supports only highs") != std::string::npos,
          "unreviewed solver rejected before starting any runtime");
  }
  auto unconfigured = make_ampl_backend({"", "highs"});
  check(optimize(r, *unconfigured, [] { return now; }).status == SolveStatus::solver_failed,
        "explicit runtime directory required");
  if (!ampl_backend_compiled()) {
    auto missing = make_ampl_backend({"unused", "highs"});
    const auto unavailable = optimize(r, *missing, [] { return now; });
    check(unavailable.status == SolveStatus::unavailable && !unavailable.recommendation,
          "disabled AMPL cannot silently run reference oracle");
  }
}

void test_backend_cannot_return_dominated_offer() {
  const auto request = test::affordability_fixtures(now).front().request;
  // A backend can report perfectly consistent accounts and the old, better
  // balance score while still violating the new affordability guard.
  FixedBackend backend({BackendStatus::optimal, {1, 0}, 600,
                        "synthetic backend returning the dominated 200 offer", 1600});
  const auto rejected = optimize(request, backend, [] { return now; });
  check(rejected.status == SolveStatus::rejected_solution && !rejected.recommendation,
        "accurate financial output cannot smuggle a dominated higher price past the core");
  backend.raw = {BackendStatus::optimal, {0, 1}, 1400,
                 "synthetic backend returning the affordable 160 offer", 2400};
  const auto accepted = optimize(request, backend, [] { return now; });
  check(accepted.status == SolveStatus::recommended && accepted.recommendation &&
        accepted.recommendation->public_prices == std::vector<Money>{160} &&
        accepted.recommendation->scenario_worker_surplus == std::vector<Money>{1400, 1400} &&
        accepted.recommendation->scenario_funding_balance == std::vector<Money>{2400, 2400},
        "cheaper no-worse output preserves exact surplus and funding accounts");
}

struct CommaDecimal : std::numpunct<char> {
  char do_decimal_point() const override { return ','; }
};
void test_numeric_ampl_serialization() {
  auto r = ph::price::example::synthetic_request(now);
  r.products[0].sku = "x'; shell 'evil'; #";
  r.forecast_source = "untrusted'; drop choose;";
  r.liquidity_buffer = 321;
  const auto old = std::locale();
  std::locale::global(std::locale(old, new CommaDecimal));
  const auto data = detail::ampl_data(r);
  std::locale::global(old);
  check(data.find("shell") == std::string::npos && data.find("drop") == std::string::npos,
        "untrusted strings never become AMPL statements");
  check(data.find("0.59999999999999998") != std::string::npos,
        "probability serialized with locale-independent round-trip precision");
  check(data.find("let candidate_count[2]") < data.find("let price[1,1]"),
        "all candidate counts initialized before dependent indexed data");
  check(data.find("let forecast_units[2,3,2] := 5;") != std::string::npos,
        "all scenario quantities encoded in stable index order");
  check(data.find("let hold_candidate[1] := 2;") != std::string::npos,
        "hold-price reference serialized by original candidate index");
  check(data.find("let liquidity_buffer := 321;") != std::string::npos,
        "continuity liquidity is serialized independently of the earned balance");
}
}

int main() {
  try {
    test_money_and_policy();
    test_invalid_inputs();
    test_solver_boundary();
    test_backend_cannot_return_dominated_offer();
    test_numeric_ampl_serialization();
    std::cout << assertions << " price-engine assertions passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAILED after " << assertions << " assertions: " << error.what() << '\n';
    return 1;
  }
}
