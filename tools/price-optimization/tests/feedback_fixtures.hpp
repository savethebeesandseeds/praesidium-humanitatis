// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#pragma once
#include "synthetic_request.hpp"

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
}  // namespace ph::price::test
