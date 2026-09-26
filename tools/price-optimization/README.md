<!-- SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0 -->
# Price optimization engine

A C++17 prototype for recommending public prices under worker and customer protections. The intended consumer is [post-profit-exchange](../../projects/post-profit-exchange/README.md), a separate application for researching and running store operations. Prices are one optimized decision within that application. This tool uses an AMPL mixed-integer model when a separately licensed AMPL C++ API, runtime, and solver are supplied. It does not run a store, infer price elasticity, prove worker ownership, transfer money, or publish prices.

This engine is research infrastructure within Praesidium Humanitatis's
[post-profit economics program](../../docs/post-profit-economics.md). It supports
the store demonstration's public-price **exchange** mechanism within a broader
model of **distribution** and **allocation**, without passive-owner profit
extraction. We acknowledge the model's potential to
disrupt incumbent stores, factories and their owners' interests. Our
[Adversarial Cooperation work](https://github.com/savethebeesandseeds/adversarial-cooperation)
is the related research direction for those participants; a
[future protocol](../../docs/adversarial-cooperation.md) is intended to address
their interests in relation to any advantage the proposed operating model
demonstrates. The protocol is not implemented here.

This directory is governed by [its own license](LICENSE), not the repository's MIT license. AMPL and solvers retain their separate terms. Vendor binaries and license files are excluded from the source release. The locally staged evaluation runtime remains in ignored `.build/deps/` under the vendor's bundled Demo license. AMPL is a modeling system that invokes a solver; this project does not relicense AMPL or provide an operational entitlement.

## Decision and protections

Each request covers one currency and one decision horizon, with candidate prices and whole-unit demand forecasts supplied for each product and scenario. The model chooses exactly one posted price per SKU, shared across scenarios and customers. Among candidates allowed by the protections below, it minimizes the probability-weighted absolute projected funding balance: `sum(probability[s] * abs(B + contribution[s] - wages - operating_cost - reserve_requirement))`. `B` is the signed feedback adjustment supplied from reconciled actual operating history, optionally spread over a stated recovery horizon. Financial surplus remains an accounting measure, not a reward to maximize.

A positive balance permits price reductions or holding; a negative balance permits increases or holding; zero holds the previous prices. An increase is excluded if it reduces forecast contribution in any supplied scenario. Every SKU must include its previous price among the candidates. These rules express the exchange project's mechanism: good sales fund lower prices, while poor sales call for a useful increase to cover the gap. Limits or demand response can prevent correction; residual imbalance is reported.

The following are hard constraints:

- Every selected price stays below the supplied affordability ceiling and within the allowed change from the previous public price.
- Forecast units at that candidate fit inventory in **every** supplied scenario. Excess demand is rejected, not silently clipped to stock.
- A candidate is excluded when a strictly cheaper candidate for the same SKU is otherwise locally legal and supplies at least as many forecast purchases and at least as much contribution `(price - unit_cost) * units` in **every** supplied scenario. The cheaper alternative must itself obey the ceiling, change cap, inventory, direction and safe-increase rules. Extra projected surplus alone cannot justify rejecting this alternative. This protects a specific forecast-supported improvement; it does not choose the cheapest feasible basket or compare tradeoffs across SKUs.
- Sales contribution plus explicit earned coverage credit and additional operating liquidity covers wages, operating costs, and the scheduled reserve requirement in **every** scenario. Credit must be zero for a nonpositive feedback balance. Additional liquidity may come from initial assets and is available at any balance sign. The caller must establish that the two cash amounts are spendable and disjoint; neither is added to earned feedback or the objective.

This affordability protection is objective `operating_balance_tracking` version `2`, engine `0.5.0`, model `public-prices.v4`. The JSON shape stays `ph.price.v3`, but objective version `1` requests are rejected. The remaining financial score can be higher than under version 1 because a formerly preferred higher price is now excluded. These comparisons depend on supplied forecasts, not established customer behavior.

There is no owner payout or wage reduction variable. The output surplus is an accounting recommendation for worker retention; the engine cannot enforce where an operator actually sends money. Worker governance must set appropriate ceilings, costs, reserves, scenarios, and wage requirements. A caller's policy reference is an audit label, not authenticated consent. The prototype does not verify fair wages, real-world affordability, forecast truth, ownership, competition law, or compliance with its license.

Amounts are integer minor currency units; products use whole units. The current bounds are 256 products, 64 candidates per product, 32 scenarios, 1,000,000 units per quantity, and 1,000,000,000 minor units per individual amount. A conservative aggregate bound keeps integer monetary calculations exactly representable in the AMPL numeric representation. Fractional quantities, tax calculations, multiple currencies, inventory replenishment, demand substitution between products, and multi-period planning require a further design.

## Failure behavior

The core rejects missing provenance references, duplicate identifiers or prices, out-of-range inputs, malformed scenario arrays, non-finite probabilities, probability totals different from one, future timestamps, and stale input. The feedback balance may be signed, bounded by ±2^50; coverage credit and liquidity buffer are each nonnegative and at most 2^50. Other money and quantities retain their nonnegative bounds. The maximum input age is explicit and capped at 24 hours; deployment policy should choose a suitably short value. All input state shares the request timestamp. Validity and maximum age have exclusive expiry boundaries.

`optimize` checks freshness before and after solving, rejects clock rollback, and returns no recommendation on solver failure, unavailable dependencies, nonoptimal status, or infeasibility. It does not relax protected constraints or fall back to an alternate optimizer. Every returned binary choice, price, inventory limit, scenario cost coverage, and reported objective is independently checked in C++. Monetary feasibility is recomputed exactly after rounding only choices already within the narrow binary tolerance. Reported optimality is still a solver claim; independent checks establish feasibility, not a general proof of optimality.

The AMPL model is embedded at build time from [`models/public_prices.mod`](models/public_prices.mod). Request strings never enter AMPL code; only validated numerical data and generated numerical indices are serialized. Runtime directories and the solver executable are administrator-controlled configuration. Output is a recommendation with an expiry time. A future publisher must revalidate against current inventory and approved policy and obtain worker authorization. No publisher exists here.

The `ph.price.v3` contract exposes continuity liquidity explicitly. Available operating cash can fund a forecast shortfall while the financial deficit and earned feedback remain visible. This does not relax price, inventory, wage or reserve constraints. The application owns assurance warnings and actual cash-exhaustion decisions; optimizer infeasibility does not by itself mean a store should close.

## Offline build and checks

Requires CMake 3.25 or later and a C++17 compiler. Neither Torch nor AMPL is needed for the default build. From the repository root:

```sh
cmake -S tools/price-optimization -B .build/price-optimization \
  -DCMAKE_BUILD_TYPE=Release -DPH_PRICE_WITH_AMPL=OFF
cmake --build .build/price-optimization --parallel
ctest --test-dir .build/price-optimization --output-on-failure
.build/price-optimization/price_guardrail_demo
```

The guardrail demo validates the explicit synthetic prices of 200 and 300 euro cents. Their scenario surpluses after protected costs are 2,400 and 1,400 cents, with an expected surplus of 2,000 cents. It then demonstrates rejection of an excessive price. **This demo does not optimize.**

The tests cover exact financial arithmetic, every constraint, worst-scenario coverage, invalid and stale input, fractional/malformed solver output, exceptions, missing dependencies, expiration during solve, clock rollback, and numeric-only AMPL serialization. A bounded exhaustive oracle exists only under `tests/`; it is not linked into the production libraries.

Validation on 2026-09-20 in the approved, separate Debian 12 container: the SDK-free build passed 116 core assertions and both CTest entries with GCC 12.2. The AMPL-enabled build passed all three CTest entries, including 115 core assertions and 14 real AMPL/HiGHS fixtures checked against exhaustive enumeration. The standalone AMPL demo recommended 200 and 300 euro cents with expected worker surplus of 2,000 cents. These were synthetic evaluations under the vendor's bundled Demo license. See the [verification record](../../docs/price-optimization-verification.md).

The separate Debian container procedure is in [`environment/README.md`](environment/README.md). [`tasks.sh`](tasks.sh) runs project operations inside that environment. Dependency installation belongs in `environment/setup.sh`; container lifecycle belongs in its separate launcher.

## Dashboard integration

The [exchange application](../../projects/post-profit-exchange/README.md) also
has a standalone browser simulator. It explicitly uses the
[bounded enumeration backend](docs/enumeration.md) compiled with this C++ core
to WebAssembly. This solves small finite candidate models without AMPL and
never acts as an automatic fallback from the AMPL backend. The browser bundle
preserves the engine's separate license and includes corresponding source.

The Linux [`price_backend` service](docs/backend.md) provides a versioned JSON
Lines boundary with separate policy, cost, catalog and demand inputs, real
worker deadlines and cancellation, and independently recomputed financial
explanations. See the [request/result contract](docs/contract.md). It invokes
the same AMPL model; no additional objective, governance or publication action
is introduced. The default offline build includes contract/process tests and
an AMPL-disabled worker; actual optimization still requires the enabled vendor
backend. The new JSON dependency is vendored under its own MIT license.

The [integration verification record](../../docs/optimizer-integration-verification.md)
records seven passing AMPL-enabled suites, six passing SDK-free suites, and
the actual JSON request/result for the synthetic store example.

## Optional AMPL backend

Stage a legitimately obtained C++ API SDK with `include/ampl/ampl.h` and the matching `libampl` library. Stage the AMPL runtime and its HiGHS driver separately, with an entitlement permitting the intended use. The initial adapter supports **only `highs`**; other solver names are rejected before starting AMPL because their status-code mappings have not been reviewed. No automatic Community Edition activation is performed. In particular, Community Edition terms restrict using prototype results to improve business operations or validate commercial systems; an ownerless or nonprofit store should not assume it is exempt. Review the current [AMPL EULA](https://ampl.com/terms-eula/) with the vendor before operational use.

```sh
cmake -S tools/price-optimization -B .build/price-optimization-ampl \
  -DCMAKE_BUILD_TYPE=Release -DPH_PRICE_WITH_AMPL=ON \
  -DAMPLAPI_ROOT=/workspace/praesidium-humanitatis/.build/deps/amplapi
cmake --build .build/price-optimization-ampl --parallel
.build/price-optimization-ampl/price_ampl_demo \
  /workspace/praesidium-humanitatis/.build/deps/ampl highs
```

The adapter uses the documented [AMPL C++ API](https://ampl.com/api/latest/cpp/classes/ampl.html), creates an environment using the specified runtime directory, loads the embedded model, selects HiGHS, solves, and retrieves the choices and objective. It accepts HiGHS's normal optimal status (`solve_result_num = 0`, `solve_result = solved`) or AMPL's documented presolve solution (`99`, `solved`), then independently checks the selection. AMPL presolve infeasibility (`299`, `infeasible`) is recognized even when the API raises a generic exception. The presolve codes are documented in the [AMPL changelog](https://dev.ampl.com/releases/ampl.html), entry 2021-05-31. Ensure the platform dynamic loader can find the SDK's shared libraries. The low-level library call is synchronous and its freshness check does not interrupt a blocked solver. Use the supervised dashboard backend for process deadlines and cancellation. Solver-specific tuning and a hard real-time guarantee are not provided.

To register the real integration test, configure with both the SDK and runtime:

```sh
cmake -S tools/price-optimization -B .build/price-optimization-ampl \
  -DPH_PRICE_WITH_AMPL=ON -DAMPLAPI_ROOT=/workspace/praesidium-humanitatis/.build/deps/amplapi \
  -DPH_AMPL_BINARY_DIR=/workspace/praesidium-humanitatis/.build/deps/ampl -DPH_AMPL_SOLVER=highs
cmake --build .build/price-optimization-ampl --parallel
ctest --test-dir .build/price-optimization-ampl --output-on-failure
```

The integration test compares AMPL with bounded enumeration on 14 general fixtures, seven feedback fixtures and three liquidity fixtures with insufficient-cash variants. Cases include AMPL presolve success, presolve infeasibility, HiGHS integer infeasibility, earned discounts, useful increases, blocked harmful increases, and continued coverage from disjoint operating cash. It also checks that a missing runtime produces a failure without a recommendation. It is not registered in an SDK-free build. Vendor dependencies remain locally staged in ignored `.build/deps/`. Passing these synthetic Demo evaluations does not establish entitlement or readiness for operational store pricing. Verification is recorded in the [exchange verification record](../../docs/exchange-simulation-verification.md); the earlier records describe superseded formulations.

## C++ boundary

[`include/ph/price/engine.hpp`](include/ph/price/engine.hpp) defines requests, validation, backend results, and recommendations. Link `ph::price_core` for validation; link `ph::price_ampl` to create the AMPL backend. With AMPL disabled, that factory produces an explicitly unavailable backend. A backend result never bypasses independent validation when called through `optimize`.

Before a real store pilot, the project still needs empirical demand estimation with uncertainty, worker-authenticated policy approval, persistent audit records, protected essential-goods policy, secure store integration, publication approval and rollback, monitored solve deadlines, operational/legal review, and evidence that the assumptions hold. Calling this library repeatedly is not yet a safe autonomous store.
