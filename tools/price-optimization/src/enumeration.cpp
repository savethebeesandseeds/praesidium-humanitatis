// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#include "ph/price/enumeration.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace ph::price {
namespace {

struct LocalChoice {
  std::size_t original_index;
  std::vector<Money> contribution;
};

struct Branch {
  std::size_t product_index;
  std::vector<LocalChoice> choices;
};

class EnumerationBackend final : public SolverBackend {
 public:
  explicit EnumerationBackend(std::size_t max_combinations)
      : max_combinations_(max_combinations) {}

  RawSolution solve_raw(const Request& request) override {
    if (max_combinations_ == 0 || max_combinations_ > kEnumerationCombinationLimit) {
      return {BackendStatus::failed, {}, 0,
              "enumeration limit must be between 1 and 200000 combinations"};
    }
    // optimize() owns actual clock-based freshness before and after solve.
    // A direct backend invocation must still reject malformed/unsafe numbers
    // before any arithmetic or traversal. One structural validation suffices.
    const auto validation = validate_request(request, request.as_of);
    if (!validation.ok()) {
      return {BackendStatus::failed, {}, 0,
              "invalid enumeration input: " + validation.errors.front()};
    }

    std::vector<std::vector<std::size_t>> allowed(request.products.size());
    std::size_t combinations = 1;
    for (std::size_t i = 0; i < request.products.size(); ++i) {
      const auto& product = request.products[i];
      const auto hold = std::find_if(product.candidates.begin(), product.candidates.end(),
          [&](const Candidate& candidate) { return candidate.price == product.previous_price; });
      for (std::size_t k = 0; k < product.candidates.size(); ++k) {
        const auto& candidate = product.candidates[k];
        if (candidate.price > product.affordability_ceiling ||
            (request.funding_balance > 0 && candidate.price > product.previous_price) ||
            (request.funding_balance < 0 && candidate.price < product.previous_price) ||
            (request.funding_balance == 0 && candidate.price != product.previous_price) ||
            std::abs(candidate.price - product.previous_price) * 10000 >
                product.previous_price * product.max_change_basis_points ||
            std::any_of(candidate.forecast_units.begin(), candidate.forecast_units.end(),
                        [&](auto quantity) { return quantity > product.inventory; })) {
          continue;
        }
        bool harmful_increase = false;
        if (request.funding_balance < 0 && candidate.price > product.previous_price) {
          for (std::size_t s = 0; s < request.scenarios.size(); ++s) {
            if ((candidate.price - product.unit_cost) * candidate.forecast_units[s] <
                (product.previous_price - product.unit_cost) * hold->forecast_units[s]) harmful_increase = true;
          }
        }
        if (harmful_increase) continue;
        allowed[i].push_back(k);
      }
      if (allowed[i].empty()) {
        return {BackendStatus::infeasible, {}, 0,
                "no candidate satisfies price, feedback and inventory policy for product " + std::to_string(i)};
      }
      // Division guards both the configured budget and size_t multiplication
      // overflow. Never begin an incomplete search or return an incumbent.
      if (combinations > max_combinations_ / allowed[i].size()) {
        return {BackendStatus::failed, {}, 0,
                "locally legal candidate combinations exceed the enumeration limit"};
      }
      combinations *= allowed[i].size();
    }

    const auto scenario_count = request.scenarios.size();
    const Money protected_cost = request.worker_wage_floor + request.operating_cost + request.reserve_floor;
    std::vector<Money> surplus(scenario_count, -protected_cost);
    std::vector<std::size_t> indices(request.products.size());
    std::vector<Branch> branches;
    for (std::size_t i = 0; i < request.products.size(); ++i) {
      const auto& product = request.products[i];
      Branch branch{i, {}};
      for (const auto k : allowed[i]) {
        const auto& candidate = product.candidates[k];
        LocalChoice choice{k, std::vector<Money>(scenario_count)};
        for (std::size_t s = 0; s < scenario_count; ++s) {
          choice.contribution[s] = (candidate.price - product.unit_cost) * candidate.forecast_units[s];
        }
        branch.choices.push_back(std::move(choice));
      }
      if (branch.choices.size() == 1) {
        // Fold fixed decisions once. Long runs of single-candidate products
        // must not multiply work at every leaf of a small branching search.
        indices[i] = branch.choices.front().original_index;
        for (std::size_t s = 0; s < scenario_count; ++s) surplus[s] += branch.choices.front().contribution[s];
      } else {
        branches.push_back(std::move(branch));
      }
    }

    // A separate best remaining contribution in each scenario is an upper
    // bound, even if no single selection attains all those maxima together.
    // Thus this pruning cannot discard a feasible protected-cost outcome.
    std::vector<std::vector<Money>> remaining_upper(
        branches.size() + 1, std::vector<Money>(scenario_count, 0));
    for (std::size_t depth = branches.size(); depth-- > 0;) {
      for (std::size_t s = 0; s < scenario_count; ++s) {
        Money best = branches[depth].choices.front().contribution[s];
        for (const auto& choice : branches[depth].choices) best = std::max(best, choice.contribution[s]);
        remaining_upper[depth][s] = remaining_upper[depth + 1][s] + best;
      }
    }

    bool found = false;
    double best_expected = 0, best_financial = 0;
    std::vector<std::size_t> best_indices;
    std::function<void(std::size_t)> visit = [&](std::size_t depth) {
      for (std::size_t s = 0; s < scenario_count; ++s) {
        if (surplus[s] + remaining_upper[depth][s] + request.coverage_credit + request.liquidity_buffer < 0) return;
      }
      if (depth == branches.size()) {
        long double expected = 0, financial = 0;
        for (std::size_t s = 0; s < scenario_count; ++s) {
          expected += static_cast<long double>(request.scenarios[s].probability) *
              std::abs(request.funding_balance + surplus[s]);
          financial += static_cast<long double>(request.scenarios[s].probability) * surplus[s];
        }
        // Match the objective representation exposed by evaluate_selection:
        // accumulate in long double, then report/compare the rounded double.
        // Visit original indices in ascending lexicographic order and replace
        // only on improvement, giving a deterministic tie choice.
        const auto objective = static_cast<double>(expected);
        if (!found || objective < best_expected) {
          found = true;
          best_expected = objective;
          best_financial = static_cast<double>(financial);
          best_indices = indices;
        }
        return;
      }
      const auto& branch = branches[depth];
      for (const auto& choice : branch.choices) {
        indices[branch.product_index] = choice.original_index;
        for (std::size_t s = 0; s < scenario_count; ++s) surplus[s] += choice.contribution[s];
        visit(depth + 1);
        for (std::size_t s = 0; s < scenario_count; ++s) surplus[s] -= choice.contribution[s];
      }
    };
    visit(0);
    if (!found) {
      return {BackendStatus::infeasible, {}, 0,
              "no enumerated selection covers protected wages, operating costs and reserve in every scenario"};
    }

    RawSolution result{BackendStatus::optimal, {}, best_financial,
                       "bounded exact enumeration of " + std::to_string(combinations) +
                           " locally legal combinations; awaiting independent verification", best_expected};
    for (std::size_t i = 0; i < request.products.size(); ++i) {
      for (std::size_t k = 0; k < request.products[i].candidates.size(); ++k) {
        result.choices.push_back(k == best_indices[i] ? 1.0 : 0.0);
      }
    }
    return result;
  }

 private:
  std::size_t max_combinations_;
};

}  // namespace

std::unique_ptr<SolverBackend> make_enumeration_backend(std::size_t max_combinations) {
  return std::make_unique<EnumerationBackend>(max_combinations);
}

}  // namespace ph::price
