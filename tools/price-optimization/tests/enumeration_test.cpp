// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#include "ph/price/enumeration.hpp"
#include "reference_oracle.hpp"
#include "feedback_fixtures.hpp"
#include "synthetic_request.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

namespace {
using namespace ph::price;
constexpr Timestamp kNow = 1800000000;
int assertions = 0;

void check(bool condition, const char* message) {
  ++assertions;
  if (!condition) throw std::runtime_error(message);
}

SolveResult solve(const Request& request, std::size_t limit = kEnumerationCombinationLimit) {
  auto backend = make_enumeration_backend(limit);
  return optimize(request, *backend, [] { return kNow; });
}

void compare_oracle(const Request& request) {
  const auto reference = test::exhaustive_oracle(request, kNow);
  auto backend = make_enumeration_backend();
  const auto raw = backend->solve_raw(request);
  check(raw.status == reference.status, "enumeration feasibility agrees with exhaustive reference");
  if (reference.status == BackendStatus::optimal) {
    check(std::abs(raw.expected_worker_surplus - reference.expected_worker_surplus) < 1e-8,
          "enumeration financial accounting agrees with exhaustive reference");
    check(std::abs(raw.expected_absolute_balance - reference.expected_absolute_balance) < 1e-8,
          "enumeration balance objective agrees with exhaustive reference");
    check(raw.choices == reference.choices, "enumeration deterministic choice agrees with exhaustive reference");
    const auto verified = optimize(request, *backend, [] { return kNow; });
    check(verified.status == SolveStatus::recommended && verified.recommendation.has_value(),
          "enumeration result independently accepted by core");
  } else {
    check(raw.choices.empty(), "infeasible enumeration returns no candidate prices");
  }
}

void test_known_result_and_guards() {
  auto request = example::synthetic_request(kNow);
  const auto result = solve(request);
  check(result.recommendation.has_value(), "synthetic enumeration has a recommendation");
  check(result.recommendation->public_prices == std::vector<Money>{200, 300}, "hand-calculated optimal prices");
  check(result.recommendation->scenario_worker_surplus == std::vector<Money>{2400, 1400},
        "hand-calculated protected scenario surplus");
  check(result.recommendation->expected_worker_surplus == 2000, "hand-calculated expected surplus");
  compare_oracle(request);

  request.products[0].inventory = 15;
  request.funding_balance = -100;
  request.products[0].candidates[2].forecast_units = {14, 10};
  const auto inventory_limited = solve(request);
  check(inventory_limited.recommendation && inventory_limited.recommendation->candidate_indices[0] == 2,
        "over-inventory candidates are eliminated without reindexing output choices");
  compare_oracle(request);

  request = example::synthetic_request(kNow);
  request.products[0].affordability_ceiling = 170;
  check(solve(request).status == SolveStatus::infeasible, "no locally affordable candidate is infeasible");
  request = example::synthetic_request(kNow);
  request.products[0].previous_price = 250;
  request.products[0].max_change_basis_points = 0;
  check(solve(request).status == SolveStatus::invalid_input, "missing hold-price reference is invalid input");
  request = example::synthetic_request(kNow);
  request.worker_wage_floor = 100000;
  check(solve(request).status == SolveStatus::infeasible, "unfunded protected wage floor is infeasible");

  request = example::synthetic_request(kNow);
  request.funding_balance = 1;
  request.products = {{"staple", 100, 220, 220, 100, 1000,
                       {{200, {100, 0}}, {220, {10, 10}}}}};
  const auto protected_result = solve(request);
  check(protected_result.recommendation && protected_result.recommendation->public_prices == std::vector<Money>{220},
        "higher expectation cannot override a failed low-demand wage scenario");
  compare_oracle(request);

  // A below-cost product is allowed if the complete store still covers every
  // protected cost. Scenario pruning must not reject negative partial sums.
  request = example::synthetic_request(kNow);
  request.products = {{"subsidized", 200, 100, 100, 1, 0, {{100, {1, 1}}}},
                      {"funding", 0, 500, 500, 4, 0, {{500, {4, 4}}}}};
  const auto loss_leader = solve(request);
  check(loss_leader.recommendation && loss_leader.recommendation->scenario_worker_surplus == std::vector<Money>{900, 900},
        "negative product contribution may be covered by another product");
  compare_oracle(request);
}

void test_deterministic_ties() {
  auto request = example::synthetic_request(kNow);
  request.worker_wage_floor = 200;
  request.operating_cost = 0;
  request.reserve_floor = 0;
  request.funding_balance = 1;
  request.products = {{"tie", 0, 400, 400, 2, 5000,
                       {{400, {1, 1}}, {200, {2, 2}}}}};
  const auto result = solve(request);
  check(result.recommendation && result.recommendation->candidate_indices == std::vector<std::size_t>{0},
        "objective tie chooses first original candidate, not lowest price");
  compare_oracle(request);
  for (int repeat = 0; repeat < 4; ++repeat) {
    check(solve(request).recommendation->public_prices == std::vector<Money>{400}, "repeated tie choice is deterministic");
  }
  // Different scenario totals have the same reported double objective.
  // Native long-double intermediates must not break the core's tie policy.
  request.worker_wage_floor = 1;
  request.products = {{"rounded-tie", 0, 2, 2, 1004, 5000,
                       {{1, {1000, 1001}}, {2, {499, 502}}}}};
  const auto rounded_tie = solve(request);
  check(rounded_tie.recommendation && rounded_tie.recommendation->candidate_indices == std::vector<std::size_t>{0},
        "tie comparison matches the core's reported objective precision");
  compare_oracle(request);
}

void test_limits_and_fixed_products() {
  auto request = example::synthetic_request(kNow);
  request.funding_balance = 1;
  request.products[0].previous_price = 220;
  request.products[1].previous_price = 330;
  for (auto& product : request.products) product.max_change_basis_points = 3000;
  // Bread has three locally legal choices, beans three. The fourth bread
  // candidate violates policy and does not consume search capacity.
  check(solve(request, 9).status == SolveStatus::recommended, "exact effective combination limit accepted");
  check(solve(request, 8).status == SolveStatus::solver_failed, "limit exceeded fails without an incumbent");
  check(solve(request, 0).status == SolveStatus::solver_failed, "zero limit is invalid");
  check(solve(request, kEnumerationCombinationLimit + 1).status == SolveStatus::solver_failed,
        "caller cannot raise hard combination cap");
  check(solve(request, std::numeric_limits<std::size_t>::max()).status == SolveStatus::solver_failed,
        "oversized configuration rejected without integer overflow");

  auto large = request;
  large.products.clear();
  for (std::size_t i = 0; i < 256; ++i) {
    large.products.push_back({"fixed-" + std::to_string(i), 100, 200, 220, 1, 1000,
                              {{200, {1, 1}}, {300, {1, 1}}}});
  }
  const auto fixed = solve(large, 1);
  check(fixed.recommendation && fixed.recommendation->candidate_indices.size() == 256,
        "256 fixed choices are folded once despite huge unfiltered cardinality");
  check(fixed.recommendation->expected_worker_surplus == 24600,
        "fixed-product accounting includes every product");

  // 2^18 exceeds the hard cap, without approaching machine integer limits.
  // No branch search or partial recommendation may be returned.
  large.products.resize(18);
  large.funding_balance = -1;
  for (auto& product : large.products) product.candidates[1].price = 201;
  const auto too_many = solve(large);
  check(too_many.status == SolveStatus::solver_failed && !too_many.recommendation,
        "excessive local Cartesian product is refused before search");
}

void test_validation_and_freshness() {
  auto request = example::synthetic_request(kNow);
  request.scenarios[0].probability = std::numeric_limits<double>::quiet_NaN();
  check(solve(request).status == SolveStatus::invalid_input, "core rejects non-finite probability");
  auto backend = make_enumeration_backend();
  check(backend->solve_raw(request).status == BackendStatus::failed, "direct raw call also validates malformed input");
  request = example::synthetic_request(kNow);
  request.products[0].candidates[0].forecast_units[0] = std::numeric_limits<std::int64_t>::max();
  check(backend->solve_raw(request).status == BackendStatus::failed, "unsafe arithmetic rejected before contribution computation");
  request = example::synthetic_request(kNow);
  check(optimize(request, *backend, [] { return kNow + 120; }).status == SolveStatus::invalid_input,
        "stale request rejected before enumeration");
  int reads = 0;
  const auto expired = optimize(request, *backend, [&] { return reads++ == 0 ? kNow : kNow + 120; });
  check(expired.status == SolveStatus::rejected_solution && !expired.recommendation,
        "enumeration cannot bypass post-solve expiry");
}

void test_operating_balance_feedback() {
  for (const auto& fixture : test::feedback_fixtures(kNow)) {
    const auto result = solve(fixture.request);
    check(result.recommendation && result.recommendation->public_prices == std::vector<Money>{fixture.expected_price},
          "sales feedback chooses the independently calculated public price");
    check(std::abs(result.recommendation->expected_absolute_balance - fixture.expected_absolute_balance) < 1e-9,
          "feedback objective is expected absolute remaining funding balance");
    compare_oracle(fixture.request);
  }
  auto request = test::feedback_fixtures(kNow)[0].request;
  check(!evaluate_selection(request, {0}, kNow).recommendation &&
        !evaluate_selection(request, {2}, kNow).recommendation,
        "zero funding balance independently requires holding the price");
  request.funding_balance = 500;
  request.coverage_credit = 500;
  check(!evaluate_selection(request, {2}, kNow).recommendation,
        "positive funding balance cannot raise price");
  const auto lowered = evaluate_selection(request, {0}, kNow);
  check(lowered.recommendation && lowered.recommendation->scenario_worker_surplus == std::vector<Money>{-500, -500} &&
        lowered.recommendation->scenario_funding_balance == std::vector<Money>{0, 0},
        "cash-backed credit funds a reported current deficit without hiding it");
  request.funding_balance = -500;
  request.coverage_credit = 0;
  check(!evaluate_selection(request, {0}, kNow).recommendation,
        "negative funding balance cannot lower price");
  request.worker_wage_floor = 100;
  request.operating_cost = 0;
  request.products[0].candidates[2].forecast_units = {20, 6};
  check(!evaluate_selection(request, {2}, kNow).recommendation,
        "an increase harming one scenario is rejected despite higher expected contribution and covered wages");
  request = test::feedback_fixtures(kNow)[0].request;
  request.funding_balance = Money{1} << 50;
  request.coverage_credit = Money{1} << 50;
  check(solve(request).recommendation.has_value(), "positive aggregate balance and credit boundary are safe");
  request.funding_balance = -(Money{1} << 50);
  request.coverage_credit = 0;
  check(solve(request).recommendation.has_value(), "negative aggregate balance boundary is safe");
}

void test_continuity_liquidity() {
  for (const auto& fixture : test::liquidity_fixtures(kNow)) {
    const auto result = solve(fixture.request);
    check(result.recommendation && result.recommendation->public_prices == std::vector<Money>{fixture.expected_price},
          "existing operating cash permits a protected no-sales horizon at every balance sign");
    check(result.recommendation->scenario_worker_surplus == std::vector<Money>{-1000, -1000} &&
          result.recommendation->expected_absolute_balance == fixture.expected_absolute_balance,
          "cash funding never disguises a realized shortfall or becomes earned feedback");
    compare_oracle(fixture.request);
    auto short_cash = fixture.request;
    --short_cash.liquidity_buffer;
    check(solve(short_cash).status == SolveStatus::infeasible &&
          !evaluate_selection(short_cash, {0}, kNow).recommendation,
          "one minor unit below required disjoint cash coverage fails exactly");
  }
  auto boundary = test::liquidity_fixtures(kNow).back().request;
  boundary.coverage_credit = Money{1} << 50;
  boundary.liquidity_buffer = Money{1} << 50;
  const auto funded = solve(boundary);
  check(funded.recommendation && funded.recommendation->expected_absolute_balance == 900,
        "both exact aggregate cash bounds are accepted without adding them to the objective");
  boundary.funding_balance = -500;
  boundary.coverage_credit = 0;
  check(solve(boundary).recommendation.has_value(), "maximum liquidity is independent of negative feedback");
}

void test_random_oracle_agreement() {
  std::mt19937 random(69317);
  for (int fixture = 0; fixture < 120; ++fixture) {
    auto request = example::synthetic_request(kNow);
    for (auto& product : request.products) {
      product.unit_cost = 50 + random() % 220;
      product.inventory = 8 + random() % 17;
      for (auto& candidate : product.candidates) {
        for (auto& quantity : candidate.forecast_units) quantity = random() % 25;
      }
    }
    request.worker_wage_floor = 1 + random() % 2000;
    request.reserve_floor = random() % 300;
    request.funding_balance = fixture % 3 == 0 ? 0 :
        (fixture % 3 == 1 ? Money{1000} : Money{-1000});
    request.coverage_credit = request.funding_balance > 0 ? random() % 1500 : 0;
    request.liquidity_buffer = fixture % 4 == 0 ? random() % 1500 : 0;
    compare_oracle(request);
  }
}
}

int main() {
  try {
    test_known_result_and_guards();
    test_deterministic_ties();
    test_limits_and_fixed_products();
    test_validation_and_freshness();
    test_operating_balance_feedback();
    test_continuity_liquidity();
    test_random_oracle_agreement();
    std::cout << assertions << " enumeration assertions passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAILED after " << assertions << " assertions: " << error.what() << '\n';
    return 1;
  }
}
