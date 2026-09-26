// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#pragma once
#include "synthetic_request.hpp"
#include <algorithm>

namespace ph::price::test {
struct FeedbackFixture {
  Request request;
  Money expected_price;
  double expected_absolute_balance;
};

inline std::vector<FeedbackFixture> feedback_fixtures(Timestamp now) {
  auto base = example::synthetic_request(now);
  base.products = {{"staple", 100, 200, 250, 100, 5000,
                    {{150, {10, 10}}, {200, {10, 10}}, {250, {10, 10}}}}};
  base.worker_wage_floor = 800;
  base.operating_cost = 200;
  base.reserve_floor = 0;
  std::vector<FeedbackFixture> fixtures;
  fixtures.push_back({base, 200, 0});
  base.request_id = "earned-positive-balance-lowers-price";
  base.funding_balance = 500;
  base.coverage_credit = 500;
  fixtures.push_back({base, 150, 0});
  base.request_id = "positive-balance-without-cash-holds-price";
  base.coverage_credit = 0;
  fixtures.push_back({base, 200, 500});
  base.request_id = "negative-balance-raises-price";
  base.funding_balance = -500;
  fixtures.push_back({base, 250, 0});
  base.request_id = "bounded-recovery-keeps-unrecovered-deficit";
  base.funding_balance = -1000;
  fixtures.push_back({base, 250, 500});

  // The high price would exactly cancel B, but destroys contribution relative
  // to holding. This must be rejected even though every expense is covered.
  base.request_id = "harmful-increase-holds-price";
  base.worker_wage_floor = 500;
  base.operating_cost = 0;
  base.funding_balance = -250;
  base.products[0].candidates[2].forecast_units = {5, 5};
  fixtures.push_back({base, 200, 250});

  // Lower price has projected balances -100,+100: abs(E[B]) would be zero,
  // but E[abs(B)] is 100. Holding has +20,+20 and is correctly preferred.
  base.request_id = "absolute-error-before-scenario-expectation";
  base.worker_wage_floor = 200;
  base.funding_balance = 100;
  base.coverage_credit = 200;
  base.scenarios = {{"low", 0.5}, {"high", 0.5}};
  base.products = {{"staple", 0, 120, 120, 2, 5000,
                    {{100, {0, 2}}, {120, {1, 1}}}}};
  fixtures.push_back({base, 120, 20});
  return fixtures;
}

inline std::vector<FeedbackFixture> liquidity_fixtures(Timestamp now) {
  auto base = example::synthetic_request(now);
  base.products = {{"no-sales-staple", 100, 200, 250, 100, 5000,
                    {{200, {0, 0}}}}};
  // No sales, but existing operating cash exactly covers the fixed horizon.
  // Asset cash is not earned history: neutral feedback must still hold.
  base.request_id = "zero-balance-cash-funded-continuity";
  base.liquidity_buffer = 1000;
  std::vector<FeedbackFixture> fixtures{{base, 200, 1000}};
  base.request_id = "negative-balance-cash-funded-continuity";
  base.funding_balance = -500;
  fixtures.push_back({base, 200, 1500});
  base.request_id = "disjoint-earned-credit-and-operating-liquidity";
  base.funding_balance = 100;
  base.coverage_credit = 250;
  base.liquidity_buffer = 750;
  fixtures.push_back({base, 200, 900});
  return fixtures;
}

inline std::vector<FeedbackFixture> affordability_fixtures(Timestamp now) {
  // Supplied hypothetical forecasts, not an empirical customer-response model.
  // At 200, contribution is 1000; at 160 it is 1800. After protected costs
  // of 400 and prior balance of 1000, scores are 1600 and 2400 respectively.
  // The cheaper, no-worse offer must win despite the worse balance score.
  auto base = example::synthetic_request(now);
  base.request_id = "cheaper-more-provision-more-surplus";
  base.worker_wage_floor = 400;
  base.operating_cost = 0;
  base.reserve_floor = 0;
  base.funding_balance = 1000;
  base.products = {{"staple", 100, 200, 200, 100, 2000,
                    {{200, {10, 10}}, {160, {30, 30}}}}};
  std::vector<FeedbackFixture> fixtures{{base, 160, 2400}};
  std::reverse(base.products[0].candidates.begin(), base.products[0].candidates.end());
  base.request_id += "-reversed";
  fixtures.push_back({base, 160, 2400});

  // Equal contribution (1200), but 20 units supplied at 160 versus 12 at 200.
  base.request_id = "cheaper-equal-contribution";
  base.products[0].candidates = {{200, {12, 12}}, {160, {20, 20}}};
  fixtures.push_back({base, 160, 1800});
  std::reverse(base.products[0].candidates.begin(), base.products[0].candidates.end());
  base.request_id += "-reversed";
  fixtures.push_back({base, 160, 1800});

  // Both increases improve on holding (600 contribution). The smaller rise
  // supplies 30 units with 2400 contribution; the larger supplies 20 with 2000.
  // Their scores are 800 and 400, so balance tracking alone prefers 200.
  base.request_id = "smaller-safe-increase-dominates-larger";
  base.funding_balance = -1200;
  base.products = {{"staple", 100, 160, 200, 100, 2500,
                    {{160, {10, 10}}, {200, {20, 20}}, {180, {30, 30}}}}};
  fixtures.push_back({base, 180, 800});

  // The exact contributions differ by one minor currency unit near the
  // aggregate bound. A floating tolerance must not erase that improvement.
  base.request_id = "one-cent-contribution-improvement-at-large-scale";
  base.worker_wage_floor = 1;
  base.funding_balance = 1;
  base.products = {{"large-scale", 0, 999999001, 999999001, 1000000, 1,
                    {{999999001, {999998, 999998}}, {999998001, {999999, 999999}}}}};
  fixtures.push_back({base, 999998001, 999997001001999.0});
  return fixtures;
}
}  // namespace ph::price::test
