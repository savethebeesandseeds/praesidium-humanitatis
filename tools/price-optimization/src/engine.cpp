// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#include "ph/price/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <set>
#include <sstream>

namespace ph::price {
namespace {
constexpr Money kMaxMoney = 1000000000;
constexpr std::int64_t kMaxUnits = 1000000;
// Keep every integer monetary sum exactly representable by the AMPL double
// representation, with headroom for the independent arithmetic below.
constexpr Money kMaxAggregate = (Money{1} << 50);
constexpr std::size_t kMaxProducts = 256;
constexpr std::size_t kMaxScenarios = 32;
constexpr std::size_t kMaxCandidates = 64;

bool money_valid(Money value) { return value >= 0 && value <= kMaxMoney; }
bool units_valid(std::int64_t value) { return value >= 0 && value <= kMaxUnits; }
bool reference_valid(const std::string& value) {
  if (value.empty() || value.size() > 256) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return c >= 32 && c != 127;
  });
}
std::string describe(const Validation& v) {
  std::ostringstream out;
  for (std::size_t i = 0; i < v.errors.size(); ++i) {
    if (i) out << "; ";
    out << v.errors[i];
  }
  return out.str();
}
SolveResult failure(SolveStatus status, std::string detail) {
  return {status, std::move(detail), std::nullopt};
}
}  // namespace

Validation validate_request(const Request& r, Timestamp now) {
  Validation v;
  auto require = [&](bool good, const std::string& message) {
    if (!good) v.errors.push_back(message);
  };
  require(reference_valid(r.request_id), "request_id is required (max 256, no controls)");
  require(reference_valid(r.forecast_source), "forecast_source is required (max 256, no controls)");
  require(reference_valid(r.worker_policy_reference), "worker_policy_reference is required (max 256, no controls)");
  require(r.currency.size() == 3 && std::all_of(r.currency.begin(), r.currency.end(),
      [](char c) { return c >= 'A' && c <= 'Z'; }), "currency must be three uppercase letters");
  require(r.as_of > 0 && now >= r.as_of, "input timestamp is invalid or in the future");
  require(r.valid_until > r.as_of && now < r.valid_until, "input validity has expired or is invalid");
  require(r.max_input_age_seconds > 0 && r.max_input_age_seconds <= 86400,
          "maximum input age must be 1..86400 seconds");
  // Subtraction is safe after positive timestamp/order checks.
  require(r.as_of > 0 && now >= r.as_of && r.max_input_age_seconds > 0 &&
          now - r.as_of < r.max_input_age_seconds, "input is stale");
  require(money_valid(r.worker_wage_floor) && r.worker_wage_floor > 0,
          "worker wage floor must be positive and at most 1000000000 minor units");
  require(money_valid(r.operating_cost) && money_valid(r.reserve_floor),
          "operating cost and reserve must be 0..1000000000 minor units");
  require(r.funding_balance >= -kMaxAggregate && r.funding_balance <= kMaxAggregate,
          "funding balance must be within the signed exact aggregate bound");
  require(r.coverage_credit >= 0 && r.coverage_credit <= kMaxAggregate,
          "coverage credit must be within the nonnegative exact aggregate bound");
  require(r.liquidity_buffer >= 0 && r.liquidity_buffer <= kMaxAggregate,
          "liquidity buffer must be within the nonnegative exact aggregate bound");
  require(r.funding_balance > 0 || r.coverage_credit == 0,
          "coverage credit requires a positive funding balance");
  require(!r.scenarios.empty() && r.scenarios.size() <= kMaxScenarios,
          "request needs 1..32 scenarios");
  require(!r.products.empty() && r.products.size() <= kMaxProducts,
          "request needs 1..256 products");
  if (r.scenarios.size() > kMaxScenarios || r.products.size() > kMaxProducts) return v;

  std::set<std::string> scenario_ids;
  double probability_sum = 0;
  for (const auto& s : r.scenarios) {
    require(reference_valid(s.id) && scenario_ids.insert(s.id).second,
            "scenario IDs must be valid and unique");
    require(std::isfinite(s.probability) && s.probability > 0 && s.probability <= 1,
            "scenario probabilities must be finite, positive and at most one");
    probability_sum += s.probability;
  }
  require(std::isfinite(probability_sum) && std::abs(probability_sum - 1.0) <= 1e-9,
          "scenario probabilities must sum to one");

  std::set<std::string> skus;
  Money aggregate_bound = 0;
  for (std::size_t i = 0; i < r.products.size(); ++i) {
    const auto& p = r.products[i];
    const auto label = "product " + std::to_string(i) + ": ";
    require(reference_valid(p.sku) && skus.insert(p.sku).second,
            label + "SKU must be valid and unique");
    require(money_valid(p.unit_cost) && money_valid(p.previous_price) && p.previous_price > 0 &&
            money_valid(p.affordability_ceiling) && p.affordability_ceiling > 0,
            label + "invalid cost, previous price, or affordability ceiling");
    require(units_valid(p.inventory), label + "inventory must be 0..1000000 units");
    require(p.max_change_basis_points >= 0 && p.max_change_basis_points <= 10000,
            label + "price change cap must be 0..10000 basis points");
    require(!p.candidates.empty() && p.candidates.size() <= kMaxCandidates,
            label + "needs 1..64 candidates");
    if (p.candidates.size() > kMaxCandidates) continue;
    std::set<Money> prices;
    Money max_contribution = 0;
    for (const auto& c : p.candidates) {
      require(money_valid(c.price) && c.price > 0 && prices.insert(c.price).second,
              label + "candidate prices must be positive, bounded, and unique");
      require(c.forecast_units.size() == r.scenarios.size(), label + "forecast scenario count mismatch");
      if (c.forecast_units.size() != r.scenarios.size()) continue;
      for (const auto q : c.forecast_units) {
        require(units_valid(q), label + "forecast quantity must be 0..1000000 units");
        if (money_valid(c.price) && money_valid(p.unit_cost) && units_valid(q)) {
          max_contribution = std::max(max_contribution, std::max(c.price, p.unit_cost) * q);
        }
      }
    }
    require(prices.count(p.previous_price) != 0, label + "must include the previous price as a hold candidate");
    if (max_contribution > kMaxAggregate - aggregate_bound) {
      require(false, "aggregate monetary amount exceeds exact arithmetic bound");
      return v;
    }
    aggregate_bound += max_contribution;
  }
  return v;
}

bool locally_admissible_candidate(const Request& r, const Product& p, std::size_t k) {
  if (k >= p.candidates.size()) return false;
  const auto& c = p.candidates[k];
  if (c.price > p.affordability_ceiling ||
      std::abs(c.price - p.previous_price) * 10000 > p.previous_price * p.max_change_basis_points ||
      (r.funding_balance > 0 && c.price > p.previous_price) ||
      (r.funding_balance < 0 && c.price < p.previous_price) ||
      (r.funding_balance == 0 && c.price != p.previous_price)) return false;
  const auto hold = std::find_if(p.candidates.begin(), p.candidates.end(),
      [&](const Candidate& candidate) { return candidate.price == p.previous_price; });
  for (std::size_t s = 0; s < r.scenarios.size(); ++s) {
    if (c.forecast_units[s] > p.inventory) return false;
    if (r.funding_balance < 0 && c.price > p.previous_price &&
        (c.price - p.unit_cost) * c.forecast_units[s] <
            (p.previous_price - p.unit_cost) * hold->forecast_units[s]) return false;
  }
  return true;
}

std::optional<std::size_t> affordable_alternative(const Request& r, const Product& p,
                                                std::size_t k) {
  if (!locally_admissible_candidate(r, p, k)) return std::nullopt;
  const auto& current = p.candidates[k];
  std::optional<std::size_t> best;
  for (std::size_t j = 0; j < p.candidates.size(); ++j) {
    const auto& alternative = p.candidates[j];
    if (alternative.price >= current.price ||
        (best && alternative.price >= p.candidates[*best].price) ||
        !locally_admissible_candidate(r, p, j)) continue;
    bool preserves_provision_and_funding = true;
    for (std::size_t s = 0; s < r.scenarios.size(); ++s) {
      if (alternative.forecast_units[s] < current.forecast_units[s] ||
          (alternative.price - p.unit_cost) * alternative.forecast_units[s] <
              (current.price - p.unit_cost) * current.forecast_units[s]) {
        preserves_provision_and_funding = false;
        break;
      }
    }
    if (preserves_provision_and_funding) best = j;
  }
  return best;
}

Evaluation evaluate_selection(const Request& r, const std::vector<std::size_t>& indices,
                              Timestamp now) {
  Evaluation evaluation{validate_request(r, now), std::nullopt};
  if (!evaluation.validation.ok()) return evaluation;
  auto& errors = evaluation.validation.errors;
  if (indices.size() != r.products.size()) {
    errors.push_back("selection must contain exactly one candidate index per SKU");
    return evaluation;
  }
  Recommendation result;
  result.request_id = r.request_id;
  result.worker_policy_reference = r.worker_policy_reference;
  result.currency = r.currency;
  result.checked_at = now;
  // A recommendation cannot outlive the input-age policy even if valid_until
  // extends further. as_of + max_age is guarded against signed overflow.
  const auto age_expiry = r.as_of > std::numeric_limits<Timestamp>::max() - r.max_input_age_seconds
      ? std::numeric_limits<Timestamp>::max() : r.as_of + r.max_input_age_seconds;
  result.valid_until = std::min(r.valid_until, age_expiry);
  result.candidate_indices = indices;
  result.scenario_worker_surplus.assign(r.scenarios.size(),
      -(r.worker_wage_floor + r.operating_cost + r.reserve_floor));
  for (std::size_t i = 0; i < r.products.size(); ++i) {
    const auto& p = r.products[i];
    if (indices[i] >= p.candidates.size()) {
      errors.push_back("candidate index outside product " + std::to_string(i));
      continue;
    }
    const auto& c = p.candidates[indices[i]];
    if (const auto alternative = affordable_alternative(r, p, indices[i])) {
      errors.push_back("affordable alternative " + std::to_string(*alternative) +
                       " preserves scenario provision and contribution for " + p.sku);
    }
    if (c.price > p.affordability_ceiling) errors.push_back("affordability ceiling violated for " + p.sku);
    if (std::abs(c.price - p.previous_price) * 10000 > p.previous_price * p.max_change_basis_points) {
      errors.push_back("price change cap violated for " + p.sku);
    }
    if ((r.funding_balance > 0 && c.price > p.previous_price) ||
        (r.funding_balance < 0 && c.price < p.previous_price) ||
        (r.funding_balance == 0 && c.price != p.previous_price)) {
      errors.push_back("operating balance price direction violated for " + p.sku);
    }
    const auto hold = std::find_if(p.candidates.begin(), p.candidates.end(),
        [&](const Candidate& candidate) { return candidate.price == p.previous_price; });
    result.skus.push_back(p.sku);
    result.public_prices.push_back(c.price);
    for (std::size_t s = 0; s < r.scenarios.size(); ++s) {
      const auto q = c.forecast_units[s];
      if (q > p.inventory) errors.push_back("inventory exceeded for " + p.sku);
      if (r.funding_balance < 0 && c.price > p.previous_price &&
          (c.price - p.unit_cost) * q < (p.previous_price - p.unit_cost) * hold->forecast_units[s]) {
        errors.push_back("price increase reduces scenario contribution for " + p.sku);
      }
      result.scenario_worker_surplus[s] += (c.price - p.unit_cost) * q;
    }
  }
  long double expected = 0, absolute_balance = 0;
  for (std::size_t s = 0; s < r.scenarios.size(); ++s) {
    if (result.scenario_worker_surplus[s] + r.coverage_credit + r.liquidity_buffer < 0) {
      errors.push_back("wages, operating costs, and reserve not covered in scenario " + r.scenarios[s].id);
    }
    expected += static_cast<long double>(r.scenarios[s].probability) * result.scenario_worker_surplus[s];
    const auto balance = r.funding_balance + result.scenario_worker_surplus[s];
    result.scenario_funding_balance.push_back(balance);
    absolute_balance += static_cast<long double>(r.scenarios[s].probability) * std::abs(balance);
  }
  result.expected_worker_surplus = static_cast<double>(expected);
  result.expected_absolute_balance = static_cast<double>(absolute_balance);
  if (errors.empty()) evaluation.recommendation = std::move(result);
  return evaluation;
}

Timestamp unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

SolveResult optimize(const Request& request, SolverBackend& backend, const Clock& clock) {
  try {
    const auto started = clock();
    const auto initial = validate_request(request, started);
    if (!initial.ok()) return failure(SolveStatus::invalid_input, describe(initial));
    const auto raw = backend.solve_raw(request);
    const auto finished = clock();
    if (finished < started) return failure(SolveStatus::rejected_solution, "clock moved backwards during solve");
    const auto fresh = validate_request(request, finished);
    if (!fresh.ok()) return failure(SolveStatus::rejected_solution, describe(fresh));
    if (raw.status == BackendStatus::infeasible) return failure(SolveStatus::infeasible, raw.detail);
    if (raw.status == BackendStatus::unavailable) return failure(SolveStatus::unavailable, raw.detail);
    if (raw.status != BackendStatus::optimal) return failure(SolveStatus::solver_failed, raw.detail);
    std::size_t expected_count = 0;
    for (const auto& p : request.products) expected_count += p.candidates.size();
    if (raw.choices.size() != expected_count) {
      return failure(SolveStatus::rejected_solution, "solver returned an unexpected number of choices");
    }
    std::vector<std::size_t> selected;
    std::size_t offset = 0;
    for (const auto& p : request.products) {
      std::size_t chosen = p.candidates.size();
      for (std::size_t k = 0; k < p.candidates.size(); ++k) {
        const double value = raw.choices[offset++];
        if (!std::isfinite(value)) return failure(SolveStatus::rejected_solution, "non-finite solver choice");
        if (std::abs(value - 1.0) <= 1e-6) {
          if (chosen != p.candidates.size()) return failure(SolveStatus::rejected_solution, "multiple prices for one SKU");
          chosen = k;
        } else if (std::abs(value) > 1e-6) {
          return failure(SolveStatus::rejected_solution, "fractional or invalid solver choice");
        }
      }
      if (chosen == p.candidates.size()) return failure(SolveStatus::rejected_solution, "missing price for a SKU");
      selected.push_back(chosen);
    }
    auto evaluated = evaluate_selection(request, selected, finished);
    if (!evaluated.validation.ok()) return failure(SolveStatus::rejected_solution, describe(evaluated.validation));
    const auto expected = evaluated.recommendation->expected_worker_surplus;
    if (!std::isfinite(raw.expected_worker_surplus) ||
        std::abs(raw.expected_worker_surplus - expected) > std::max(1e-5, std::abs(expected) * 1e-10)) {
      return failure(SolveStatus::rejected_solution, "solver objective disagrees with independent recomputation");
    }
    const auto expected_absolute = evaluated.recommendation->expected_absolute_balance;
    if (!std::isfinite(raw.expected_absolute_balance) ||
        std::abs(raw.expected_absolute_balance - expected_absolute) > std::max(1e-5, std::abs(expected_absolute) * 1e-10)) {
      return failure(SolveStatus::rejected_solution, "solver balance objective disagrees with independent recomputation");
    }
    return {SolveStatus::recommended, "feasible recommendation; worker review required before publication",
            std::move(evaluated.recommendation)};
  } catch (const std::exception& error) {
    return failure(SolveStatus::solver_failed, error.what());
  } catch (...) {
    return failure(SolveStatus::solver_failed, "unknown backend or clock failure");
  }
}
}  // namespace ph::price
