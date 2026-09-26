// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#include "ph/price/engine.hpp"
#include "ph/price/enumeration.hpp"
#include "reference_oracle.hpp"
#include "feedback_fixtures.hpp"
#include "synthetic_request.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>

namespace {
ph::price::Request integer_infeasible_cycle(ph::price::Timestamp now) {
  // Coverage in paired scenarios requires x1+x2=1, x2+x3=1 and
  // x1+x3=1. The LP admits x1=x2=x3=0.5, but no binary price selection
  // exists. Unlike the excessive wage fixture, individual constraint
  // bounds do not prove infeasibility; HiGHS must resolve integrality.
  auto r = ph::price::example::synthetic_request(now);
  r.request_id = "synthetic-integer-infeasible-cycle";
  r.worker_wage_floor = 200;
  r.operating_cost = 0;
  r.reserve_floor = 0;
  r.funding_balance = 1;
  r.scenarios = {{"pair12", 1.0 / 6}, {"complement12", 1.0 / 6},
                 {"pair23", 1.0 / 6}, {"complement23", 1.0 / 6},
                 {"pair13", 1.0 / 6}, {"complement13", 1.0 / 6}};
  r.products = {
      {"p1", 0, 200, 200, 2, 5000,
       {{100, {2, 0, 0, 0, 2, 0}}, {200, {0, 1, 0, 0, 0, 1}}}},
      {"p2", 0, 200, 200, 2, 5000,
       {{100, {2, 0, 2, 0, 0, 0}}, {200, {0, 1, 0, 1, 0, 0}}}},
      {"p3", 0, 200, 200, 2, 5000,
       {{100, {0, 0, 2, 0, 2, 0}}, {200, {0, 0, 0, 1, 0, 1}}}}
  };
  return r;
}
}

int main(int argc, char** argv) {
  using namespace ph::price;
  if (argc != 3) {
    std::cerr << "Usage: price_ampl_test <AMPL binary directory> <solver>\n";
    return 2;
  }
  try {
    auto backend = make_ampl_backend({argv[1], argv[2]});
    auto enumeration = make_enumeration_backend();
    std::mt19937 random(3271);
    for (int fixture = 0; fixture < 14; ++fixture) {
      const auto now = unix_now();
      auto request = example::synthetic_request(now);
      if (fixture == 1) request.worker_wage_floor = 100000;
      if (fixture == 2) request = integer_infeasible_cycle(now);
      if (fixture == 3) {
        // One candidate fixes every decision variable; AMPL solves this
        // feasible model in presolve and returns 99/solved without HiGHS.
        request.request_id = "synthetic-presolve-solved";
        request.products.resize(1);
        request.products[0].candidates = {{200, {20, 20}}};
      }
      if (fixture >= 4) {
        for (auto& p : request.products) {
          for (auto& c : p.candidates) {
            for (auto& q : c.forecast_units) q = 3 + random() % 24;
          }
        }
        request.worker_wage_floor = 500 + random() % 1800;
        request.funding_balance = fixture % 2 ? -1000 : 1000;
        request.coverage_credit = request.funding_balance > 0 ? 500 : 0;
      }
      const auto reference = test::exhaustive_oracle(request, now);
      const auto actual = optimize(request, *backend);
      const auto enumerated = optimize(request, *enumeration);
      if (enumerated.status != actual.status ||
          (actual.recommendation && (!enumerated.recommendation ||
           std::abs(enumerated.recommendation->expected_absolute_balance - actual.recommendation->expected_absolute_balance) > 1e-6))) {
        throw std::runtime_error("bounded enumeration disagrees with validated AMPL result: " + enumerated.detail);
      }
      if (reference.status == BackendStatus::infeasible) {
        if (actual.status != SolveStatus::infeasible || actual.recommendation) {
          throw std::runtime_error("AMPL did not match oracle infeasibility: " + actual.detail);
        }
        if (fixture == 1 && actual.detail != "AMPL presolve reports infeasible (solve_result_num=299)") {
          throw std::runtime_error("expected AMPL presolve infeasibility confirmed by status 299: " + actual.detail);
        }
        if (fixture == 2 && actual.detail != "HiGHS reports infeasible under the protected constraints") {
          throw std::runtime_error("expected solver-detected integer infeasibility: " + actual.detail);
        }
      } else {
        if (!actual.recommendation ||
            std::abs(actual.recommendation->expected_absolute_balance - reference.expected_absolute_balance) > 1e-6) {
          throw std::runtime_error("AMPL disagrees with exhaustive oracle: " + actual.detail);
        }
        if (fixture == 3 && (actual.recommendation->public_prices != std::vector<Money>{200} ||
                            actual.recommendation->scenario_worker_surplus != std::vector<Money>{1000, 1000})) {
          throw std::runtime_error("AMPL presolve solution did not pass exact independent price/surplus checks");
        }
      }
    }
    for (const auto& fixture : test::feedback_fixtures(unix_now())) {
      const auto actual = optimize(fixture.request, *backend);
      const auto enumerated = optimize(fixture.request, *enumeration);
      if (!actual.recommendation || !enumerated.recommendation ||
          actual.recommendation->public_prices != std::vector<Money>{fixture.expected_price} ||
          std::abs(actual.recommendation->expected_absolute_balance - fixture.expected_absolute_balance) > 1e-6 ||
          std::abs(actual.recommendation->expected_absolute_balance - enumerated.recommendation->expected_absolute_balance) > 1e-6) {
        throw std::runtime_error("AMPL feedback fixture failed " + fixture.request.request_id + ": " + actual.detail);
      }
    }
    for (const auto& fixture : test::liquidity_fixtures(unix_now())) {
      const auto actual = optimize(fixture.request, *backend);
      const auto enumerated = optimize(fixture.request, *enumeration);
      if (!actual.recommendation || !enumerated.recommendation ||
          actual.recommendation->public_prices != std::vector<Money>{fixture.expected_price} ||
          std::abs(actual.recommendation->expected_absolute_balance - fixture.expected_absolute_balance) > 1e-6 ||
          actual.recommendation->scenario_worker_surplus != std::vector<Money>{-1000, -1000}) {
        throw std::runtime_error("AMPL liquidity fixture failed " + fixture.request.request_id + ": " + actual.detail);
      }
      auto short_cash = fixture.request;
      --short_cash.liquidity_buffer;
      if (optimize(short_cash, *backend).status != SolveStatus::infeasible ||
          optimize(short_cash, *enumeration).status != SolveStatus::infeasible) {
        throw std::runtime_error("insufficient liquidity did not fail exactly: " + fixture.request.request_id);
      }
    }
    for (const auto& fixture : test::affordability_fixtures(unix_now())) {
      const auto actual = optimize(fixture.request, *backend);
      const auto enumerated = optimize(fixture.request, *enumeration);
      if (!actual.recommendation || !enumerated.recommendation ||
          actual.recommendation->public_prices != std::vector<Money>{fixture.expected_price} ||
          enumerated.recommendation->public_prices != std::vector<Money>{fixture.expected_price} ||
          std::abs(actual.recommendation->expected_absolute_balance - fixture.expected_absolute_balance) > 1e-6 ||
          std::abs(enumerated.recommendation->expected_absolute_balance - fixture.expected_absolute_balance) > 1e-6) {
        throw std::runtime_error("AMPL affordability fixture failed " + fixture.request.request_id + ": " + actual.detail);
      }
    }
    // A genuine runtime startup error must never be relabeled infeasible.
    // Inspect only; the deliberately missing path is never created.
    const auto missing = std::filesystem::path(argv[1]) / "ph-intentionally-missing-runtime";
    if (std::filesystem::exists(missing)) throw std::runtime_error("missing-runtime test path unexpectedly exists");
    auto unavailable = make_ampl_backend({missing.string(), argv[2]});
    const auto startup_failure = optimize(example::synthetic_request(unix_now()), *unavailable);
    if (startup_failure.status != SolveStatus::solver_failed || startup_failure.recommendation ||
        startup_failure.detail.find("AMPL failure: ") != 0) {
      throw std::runtime_error("runtime startup error was not kept as a failure: " + startup_failure.detail);
    }
    std::cout << "14 AMPL fixtures, 7 sales-feedback fixtures, 6 affordability fixtures and 3 liquidity fixtures (plus their insufficient-cash variants) agree with bounded enumeration; AMPL presolve status 299 and solver infeasibility distinguished; "
                 "presolve success 99 independently validated; "
                 "startup failure remains a failure; outputs independently validated\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
