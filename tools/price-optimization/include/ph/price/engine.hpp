// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ph::price {

// A single currency and one decision horizon per request. Monetary values are
// integer minor currency units, e.g. euro cents. Quantities are whole units.
using Money = std::int64_t;
using Timestamp = std::int64_t;

struct Scenario {
  std::string id;
  double probability = 0;
};

struct Candidate {
  Money price = 0;
  // Externally supplied quantities for each scenario, in Scenario order.
  // No elasticity estimation, truncation to inventory, or customer profiling.
  std::vector<std::int64_t> forecast_units;
};

struct Product {
  std::string sku;
  Money unit_cost = 0;
  Money previous_price = 0;
  Money affordability_ceiling = 0;
  std::int64_t inventory = 0;
  int max_change_basis_points = 0;
  std::vector<Candidate> candidates;
};

struct Request {
  std::string request_id;
  std::string currency;
  std::string forecast_source;
  // An audit reference supplied by the caller, not proof of worker consent.
  std::string worker_policy_reference;
  Timestamp as_of = 0;
  Timestamp valid_until = 0;  // Exclusive; input inventory/costs share as_of.
  std::int64_t max_input_age_seconds = 0;
  Money worker_wage_floor = 0;  // Protected, cannot be reduced by the solver.
  Money operating_cost = 0;     // Excludes wages and per-unit cost.
  Money reserve_floor = 0;
  std::vector<Scenario> scenarios;
  std::vector<Product> products;
  // Reconciled signed operating balance (or an explicitly amortized control
  // target). Positive allows reductions, negative allows safe increases,
  // zero holds the previous prices. Each SKU must include its hold candidate.
  Money funding_balance = 0;
  // Independently verified, cash-backed prior earned surplus available to
  // cover a period shortfall. Must be zero when funding_balance <= 0.
  Money coverage_credit = 0;
  // Additional spendable operating cash, disjoint from coverage_credit.
  // May come from initial assets and may fund continuity at any balance sign.
  // It never increases earned funding_balance or changes price direction.
  Money liquidity_buffer = 0;
};

struct Validation {
  std::vector<std::string> errors;
  bool ok() const { return errors.empty(); }
};

struct Recommendation {
  std::string request_id;
  std::string worker_policy_reference;
  std::string currency;
  Timestamp checked_at = 0;
  Timestamp valid_until = 0;
  std::vector<std::string> skus;
  std::vector<std::size_t> candidate_indices;
  std::vector<Money> public_prices;
  std::vector<Money> scenario_worker_surplus;
  double expected_worker_surplus = 0;
  std::vector<Money> scenario_funding_balance;
  double expected_absolute_balance = 0;
};

struct Evaluation {
  Validation validation;
  std::optional<Recommendation> recommendation;
};

// Validates structure, numeric bounds, provenance references, and freshness.
Validation validate_request(const Request& request, Timestamp now);

// Independently recomputes every constraint using exact integer money. A
// feasible selection is not by itself a claim of optimality or permission to
// publish prices. Every candidate index is zero-based.
Evaluation evaluate_selection(const Request& request,
                              const std::vector<std::size_t>& indices,
                              Timestamp now);

enum class BackendStatus { optimal, infeasible, unavailable, failed };

struct RawSolution {
  BackendStatus status = BackendStatus::failed;
  // Flattened in product order, then candidate order; untrusted solver output.
  std::vector<double> choices;
  double expected_worker_surplus = 0;
  std::string detail;
  double expected_absolute_balance = 0;
};

class SolverBackend {
 public:
  virtual ~SolverBackend() = default;
  virtual RawSolution solve_raw(const Request& request) = 0;
};

enum class SolveStatus {
  recommended, invalid_input, infeasible, unavailable, solver_failed,
  rejected_solution
};

struct SolveResult {
  SolveStatus status = SolveStatus::solver_failed;
  std::string detail;
  std::optional<Recommendation> recommendation;
};

using Clock = std::function<Timestamp()>;
Timestamp unix_now();
// No fallback, price publication, account operation, or network API exists.
// A fresh clock reading after solve guards against stale returned prices.
SolveResult optimize(const Request& request, SolverBackend& backend,
                     const Clock& clock = unix_now);

struct AmplConfig {
  std::string binary_directory;
  // Initial adapter supports only "highs". Other drivers need a separately
  // reviewed status-code and optimality-policy mapping before being enabled.
  std::string solver = "highs";
};

// The default build returns an unavailable backend. PH_PRICE_WITH_AMPL enables
// the real C++ API adapter; AMPL, API, and solver have separate vendor terms.
std::unique_ptr<SolverBackend> make_ampl_backend(AmplConfig config);
bool ampl_backend_compiled();

}  // namespace ph::price
