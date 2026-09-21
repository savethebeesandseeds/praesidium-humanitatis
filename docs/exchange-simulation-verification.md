<!-- SPDX-License-Identifier: MIT -->
# Exchange simulation v3 verification

Verified on 2026-09-21 in the existing approved Debian 12 optimizer container,
ID `5738ae552c575e91e0d4308b8aa2c66aa170e8dd817282fd7abd5817907d3ff9`.
Its container definition, mount, volumes, devices and published ports were unchanged.
The store remains [post-profit-exchange](../projects/post-profit-exchange/README.md);
the `post-profit-distribution` placeholder and its zero-byte README were preserved.

This record covers `exchange.sim.v3`, pricing schema `ph.price.v3`, model
`public-prices.v3`, engine `0.4.0`, and objective `operating_balance_tracking`
version 1. The browser uses the explicitly selected bounded enumeration backend;
the native AMPL interface is tested separately. The application remains MIT and
the pricing engine retains its separate license.

## Checks completed

| Check | Current result |
| --- | --- |
| AMPL-enabled optimizer | **8/8 CTest entries passed, 8.26 seconds.** Includes 14 general, seven feedback and three liquidity fixtures, plus insufficient-cash variants, checked against bounded enumeration. |
| Native rebuild after EWMA extraction | **11/11 CTest entries passed, 3.04 seconds.** Includes 44 file-record checks, 1,128 forecasting/consumer checks, 4,989 simulation checks and the independent exponential-smoothing suite. |
| Standalone exponential-smoothing tool | **1/1 CTest entry passed, 37 checks.** Built independently without exchange, JSON, optimizer or LibTorch dependencies. |
| Extraction compatibility | Complete cold-start and explicit synthetic-history results exactly match saved pre-extraction output, including every forecast, diagnostic, consumer outcome, price and account. |
| Native versus WebAssembly | **26 fixtures passed**, including 365 days, 12 products, imported history, censoring, continuity, insolvency and strict invalid inputs. |
| Packaged standalone HTML | **Passed** embedded-worker/native parity for history and ledgers, replay, strict JSON import helpers, source/config/license ZIP contents, static element IDs and absence of remote assets. |
| Browser visual review | **Not performed.** Browser Use rejected the local URL under its URL security policy. No browser-surface or local-server workaround was used. Node worker checks do not establish visual or interactive correctness. |

The suite checks cash, FIFO valuation, stock and net-asset reconciliation;
actual-sales feedback directions; disjoint earned credit and operating liquidity;
history-only forecasting; and rollback of unexecuted purchases and reserve
requirements after a bounded-search refusal. CTest evidence is retained in
`.build/post-profit-exchange/native/Testing/Temporary/LastTest.log` and the
optimizer build's corresponding `Testing/Temporary/LastTest.log`.

The central `.cfg` supplies all products and operation/run settings; the native
`--example-config` output matches its complete embedded document, including run ID
and record paths. Persistent records are JSON files, without a database.
The reusable MIT EWMA core now lives in `tools/exponential-smoothing`; exchange retains
its consumer, price-response, stockout and JSON adapters. Default history is
empty. The fourteen-row synthetic history remains an explicit opt-in fixture,
and no TFT training or new training dataset was introduced by this extraction.

The previously verified CLI `--config` run using the explicit synthetic fixture is preserved at
`.build/post-profit-exchange/verification-v3-final-model-run/`: **66 files**,
including daily records, retained inputs/result, assurance log and final manifest.
All JSON and manifest byte counts were checked. Replaying its exact `request.json`
reproduced the result. Repeating the persisted run was refused, with every existing
file hash unchanged. Manifest byte counts themselves are not cryptographic proofs.

## Observed behavior

Both policies completed the default 30-day run with no missing trading days and
no assurance events. The first five optimized bread/beans prices, in euro cents,
were **200/300, 160/240, 130/205, 110/175, 110/160**. Final optimized cash was
**114,935** cents and earned funding balance **1,285**; fixed-price cash was
**172,860** and funding balance **62,910**. Cold-start and supplied sample-history
runs had the same totals in this fixture, while their forecast diagnostics differed.

A no-visitor stress case with 2,500 cents opening cash and 1,000 cents daily wages
ended on **day 3**, with cash **0** and wage arrears **500**. Assurance requests
remained explicitly unfunded; no external payout or invisible financing occurred.
Forecast funding shortfalls now invoke a declared continuity rule that retains
existing public prices, replacing the previous forecast-induced trading gaps.
Continuity is labeled separately from a solver recommendation. Actual cash
exhaustion ends the policy path; technical model failure rolls back the unexecuted
day and is reported separately.

The objective minimizes expected absolute projected funding imbalance, not profit.
Earned positive history permits reductions or holding; negative history permits
contribution-preserving increases or holding; zero holds. Salaries remain fixed.
Operating liquidity can fund continuity but is neither earnings nor a price signal.
Scheduled new reserve funding remains distinct from cash earmarks and releases.
Forecast contribution uses replacement costs; realized economics uses FIFO costs.

## Repeat the checks

Run from the repository root inside the documented
[existing environment](../projects/post-profit-exchange/environment/README.md):

```sh
bash tools/price-optimization/tasks.sh test-ampl
cmake -S projects/post-profit-exchange -B .build/post-profit-exchange/native -DCMAKE_BUILD_TYPE=Release
cmake --build .build/post-profit-exchange/native --parallel 2
ctest --test-dir .build/post-profit-exchange/native --output-on-failure
bash projects/post-profit-exchange/build-wasm.sh
node_bin=.build/deps/emsdk-4.0.15/node/22.16.0_64bit/bin/node
"$node_bin" projects/post-profit-exchange/simulation/wasm_test.cjs \
  .build/post-profit-exchange/wasm/exchange-runtime.js \
  .build/post-profit-exchange/native/exchange_simulation
"$node_bin" projects/post-profit-exchange/simulation/standalone_test.cjs \
  projects/post-profit-exchange/dist/post-profit-exchange.html \
  .build/post-profit-exchange/native/exchange_simulation
.build/post-profit-exchange/native/exchange_simulation \
  < .build/post-profit-exchange/verification-v3-final-model-run/request.json
```

For another persisted `--config` run, choose a new output directory in the `.cfg`;
the runner never overwrites an existing path. See the
[file contract](../projects/post-profit-exchange/docs/FILES.md).

## Evidence limits and handoff

These are synthetic implementation and consistency checks. EWMA, declared consumer
priors, sigma bands and reserve stress are conditional assumptions, not validated
worst cases, calibrated insurance probabilities or evidence of store viability.
There is no implicit TFT training, live publication, payment integration or actual
assurance provider. The checks do not establish dynamic stability, demand-response
accuracy, a benefit from removing owner returns, or entitlement for operational AMPL use.

Continue from the [handoff](../projects/post-profit-exchange/docs/HANDOFF.md),
[model definitions](../projects/post-profit-exchange/docs/MODELS.md),
[simulation contract](../projects/post-profit-exchange/simulation/CONTRACT.md),
and separate [assurance contract](../projects/post-profit-assurance/CONTRACT.md).
