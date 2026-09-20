// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#pragma once

#include "ph/price/engine.hpp"
#include <cstddef>

namespace ph::price {

inline constexpr std::size_t kEnumerationCombinationLimit = 200000;

// Explicit small-model backend; never an automatic fallback from AMPL.
// max_combinations must be 1..kEnumerationCombinationLimit. The product of
// locally legal candidate counts is checked before the search starts.
// Equal computed objective values retain the lexicographically first vector
// of original candidate indices. Call through optimize() for freshness and
// independent final validation; direct solve_raw() does not read a clock.
std::unique_ptr<SolverBackend> make_enumeration_backend(
    std::size_t max_combinations = kEnumerationCombinationLimit);

}  // namespace ph::price
