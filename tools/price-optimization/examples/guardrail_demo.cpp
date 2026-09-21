// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#include "ph/price/engine.hpp"
#include "synthetic_request.hpp"
#include <iostream>

int main() {
  const auto now = ph::price::unix_now();
  const auto request = ph::price::example::synthetic_request(now);
  const auto checked = ph::price::evaluate_selection(request, {1, 1}, now);
  if (!checked.recommendation) {
    for (const auto& error : checked.validation.errors) std::cerr << error << '\n';
    return 1;
  }
  std::cout << "Synthetic validation demo: a supplied fixed selection, NOT an optimization.\n";
  const auto& result = *checked.recommendation;
  for (std::size_t i = 0; i < result.skus.size(); ++i) {
    std::cout << result.skus[i] << ": " << result.public_prices[i] << " EUR cents\n";
  }
  std::cout << "Expected worker surplus after protected costs: " << result.expected_worker_surplus
            << " EUR cents\nExpected absolute funding balance: " << result.expected_absolute_balance
            << " EUR cents\nNo prices published. Worker review is still required.\n";
  const auto rejected = ph::price::evaluate_selection(request, {3, 1}, now);
  if (rejected.recommendation) return 2;
  std::cout << "Above-ceiling candidate rejected: " << rejected.validation.errors.front() << '\n';
  return 0;
}
