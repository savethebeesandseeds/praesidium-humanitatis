// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#include "ph/price/engine.hpp"
#include "synthetic_request.hpp"
#include <iostream>

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    std::cerr << "Usage: price_ampl_demo <AMPL binary directory> [solver=highs]\n";
    return 2;
  }
  auto backend = ph::price::make_ampl_backend({argv[1], argc == 3 ? argv[2] : "highs"});
  const auto request = ph::price::example::synthetic_request(ph::price::unix_now());
  const auto solved = ph::price::optimize(request, *backend);
  if (!solved.recommendation) {
    std::cerr << "No recommendation: " << solved.detail << '\n';
    return 1;
  }
  const auto& result = *solved.recommendation;
  std::cout << "Synthetic AMPL recommendation for worker review:\n";
  for (std::size_t i = 0; i < result.skus.size(); ++i) {
    std::cout << result.skus[i] << ": " << result.public_prices[i] << " EUR cents\n";
  }
  std::cout << "Expected worker surplus after protected costs: " << result.expected_worker_surplus
            << " EUR cents\nExpected absolute funding balance: " << result.expected_absolute_balance
            << " EUR cents\nValid until (Unix seconds, exclusive): " << result.valid_until
            << "\nNo prices published.\n";
  return 0;
}
