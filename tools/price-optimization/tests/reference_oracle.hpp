// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#pragma once
// Exhaustive reference is TEST-ONLY, bounded to small fixtures; never linked
// into the production library and never used as an AMPL fallback.
#include "ph/price/engine.hpp"
#include <functional>
#include <stdexcept>

namespace ph::price::test {
inline RawSolution exhaustive_oracle(const Request& request, Timestamp now) {
  if (!validate_request(request, now).ok()) throw std::invalid_argument("invalid oracle fixture");
  std::size_t combinations = 1;
  for (const auto& p : request.products) {
    if (combinations > 100000 / p.candidates.size()) throw std::invalid_argument("oracle fixture too large");
    combinations *= p.candidates.size();
  }
  std::optional<Recommendation> best;
  std::vector<std::size_t> indices(request.products.size());
  std::function<void(std::size_t)> visit = [&](std::size_t i) {
    if (i == request.products.size()) {
      auto candidate = evaluate_selection(request, indices, now);
      if (candidate.recommendation && (!best || candidate.recommendation->expected_worker_surplus > best->expected_worker_surplus)) {
        best = std::move(candidate.recommendation);
      }
      return;
    }
    for (std::size_t k = 0; k < request.products[i].candidates.size(); ++k) {
      indices[i] = k;
      visit(i + 1);
    }
  };
  visit(0);
  if (!best) return {BackendStatus::infeasible, {}, 0, "test-only exhaustive oracle: infeasible"};
  RawSolution raw{BackendStatus::optimal, {}, best->expected_worker_surplus, "test-only exhaustive oracle"};
  for (std::size_t i = 0; i < request.products.size(); ++i) {
    for (std::size_t k = 0; k < request.products[i].candidates.size(); ++k) {
      raw.choices.push_back(k == best->candidate_indices[i] ? 1.0 : 0.0);
    }
  }
  return raw;
}
}
