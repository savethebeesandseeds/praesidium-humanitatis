<!-- SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0 -->
# Bounded enumeration for browser research

The optional C++ backend in `ph::price_enumeration` performs exhaustive finite-candidate search for small research simulations, including WebAssembly builds. It is independent of AMPL and uses the same `Request`, scenario accounting, constraints, and independent final validator. It is not a scalable replacement for a general MIP solver and is never selected automatically when AMPL fails.

```cpp
#include "ph/price/enumeration.hpp"

auto backend = ph::price::make_enumeration_backend(); // Default: 200,000 combinations.
auto result = ph::price::optimize(request, *backend);
```

Link `ph_price_enumeration`, or its CMake alias `ph::price_enumeration`. It requires only the existing C++17 core. The AMPL backend, dependency requirements, and failure behavior are unchanged.

Before any traversal, the backend validates input structure and numeric bounds once, removes candidates that independently violate affordability, price stability, feedback direction, safe-increase contribution, or inventory in any scenario, and checks the product of the remaining candidate counts. Multiplication is guarded against overflow. A requested limit must be between 1 and the hard maximum of **200,000**. Exceeding the limit returns a failure with no partial recommendation. Having no legal candidate for a product returns infeasible.

Single-candidate products are folded into the scenario totals once. The remaining choices are visited in original product/candidate order. A branch is skipped only when even the maximum remaining contribution plus independently supplied coverage credit and disjoint operating liquidity cannot cover protected costs in some scenario. Every feasible choice covers wages, operating costs, and the period's reserve contribution from those three sources. The minimum probability-weighted absolute projected funding balance wins: `E[abs(funding_balance + scenario_worker_surplus)]`. Neither cash source is added to that balance or objective. A positive funding balance permits only holding or reducing prices; a negative balance permits holding or increases that do not reduce contribution in any scenario; zero requires holding. Equal computed objectives retain the lexicographically first vector of original candidate indices.

The search is exhaustive over the finite supplied candidates, with exact integer monetary feasibility. The expected absolute balance and separately reported financial surplus use the core's floating-point scenario probabilities and accumulation semantics; these are not exact rational calculations. It does not infer demand or search prices outside the supplied set. Actual request freshness is enforced by `optimize` before and after search, and the selected output is independently recomputed by the existing validator. Callers should not publish or rely on a raw backend result without that validation.

The core's product, scenario, quantity, and monetary bounds also apply. The combination cap is a work bound, not a wall-clock deadline or a guarantee of browser responsiveness. A browser application should run calculations in an interruptible worker and keep publication outside the research simulation.

`price_enumeration` tests cover hand-calculated outcomes, guarded candidates, robust scenario coverage, loss-leading products, deterministic ties, limit boundaries, fixed-product folding, unsafe inputs, expiry, sales feedback, exact liquidity coverage, and 120 seeded comparisons against a separate test oracle. When the AMPL integration test is enabled, its 14 general fixtures, seven sales-feedback fixtures, and three liquidity fixtures with insufficient-cash variants also compare validated AMPL and enumeration outcomes. The backend retains the engine's Worker Protection license; this additional algorithm does not change other components' licenses or claim to contain AMPL.
