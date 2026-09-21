<!-- SPDX-License-Identifier: MIT -->
# Development handoff: post-profit-exchange

Updated 2026-09-21. Start with this file, the [project README](../README.md),
[store specification](../STORE_SPECIFICATION.md), [simulation contract](../simulation/CONTRACT.md),
[models](MODELS.md), [file records](FILES.md) and current
[verification record](../../../docs/exchange-simulation-verification.md).
The verification record, not this handoff, is authoritative for checks performed.

## Purpose and decisions to preserve

The project is **post-profit-exchange**. The store is the exchange demonstration,
not the name or scope of the whole economics program. It studies public prices,
distribution through stock/procurement, and allocation through budgets and
inventory. Workers govern the productive unit and its surplus; passive-owner
profit extraction is outside the intended model. Incumbent owners' interests and
the proposed adversarial-cooperation research remain acknowledged in the README.

Prices respond to actual funding results. Earned positive funding permits lower
or held prices; a realized gap permits higher or held prices only when an increase
preserves forecast contribution in every supplied scenario. Zero history holds
the starting prices. Salaries are fixed inputs. Do not replace this with expected
profit maximization, a static cheapest-basket objective or the abandoned staged
surplus-target formulation.

Keep these quantities separate:

- Raw earned funding history: realized FIFO economic result less newly scheduled
  reserve funding; opening resources are not earnings.
- Amortized history adjustment: raw history divided by `feedback_recovery_days`,
  with nonzero sign preserved, used by the price objective.
- Earned cash credit and other operating liquidity: disjoint coverage resources
  that do not change the objective's signed history or price direction.
- Reserve target, cumulative scheduled requirement and actual cash earmark:
  releasing or rebuilding an earmark must not charge an operating loss twice.
- Forecast replacement-cost contribution and realized FIFO economic result:
  their difference can reflect cost basis as well as forecast error.

Financial forecast infeasibility alone does not close the exchange. The explicit
application continuity rule retains valid existing public prices and requests
assurance, without claiming a solver recommendation. Technical model failure is
different: roll back that day's uncommitted purchases/scheduled requirement and
terminate with a model-error record. Zero cash after obligations or unpaid fixed
obligations produces simulated insolvency and stops that policy path. The fixed
comparator can end on a different date; never hide that difference in totals.

## Components and boundaries

| Location | Responsibility |
| --- | --- |
| `configs/exchange.cfg` | Central strict-JSON research configuration, including all product/settings values. Its simulation section supplies build-time embedded defaults. |
| `records/empty-history.json` | Default empty observations; no fabricated operating history is loaded. |
| `records/sample-history.json` | Opt-in synthetic negative-day history: 14 rows, seven per SKU; never describe it as customer observations. |
| `simulation/models.hpp/.cpp` | Exchange adapters for forecast configuration, JSON, stockouts and declared price response, plus the seeded binary-logit consumer generator. |
| `simulation/simulation.cpp` | Store operations, FIFO accounts, feedback, reserve stress, continuity and terminal/event records. |
| `simulation/files.hpp/.cpp` | Native `.cfg`/history loading and new JSON run directories; no database or network service. |
| `simulation/main.cpp` | Native CLI entry point. |
| `web/` | Plain standalone controls, SVG charts, inspectable records and explicit file import/export; no JavaScript replacement simulator. |
| `build-wasm.sh`, `package-html.py` | Pinned compiler build and single-file HTML/source/license packaging. |
| `tools/exponential-smoothing/` | Reusable MIT simple exponential smoothing (EWMA) core, pure C++17 target `ph::exponential_smoothing`, header `ph/exponential_smoothing/ewma.hpp`; no JSON or exchange dependency. |
| `tools/temporal-fusion-transformer/` | Separate MIT candidate forecasting tool; no exchange adapter, training or fitted exchange checkpoint. |
| `tools/price-optimization/` | Separately licensed pricing core, finite enumeration and AMPL backend. |

The application schema is `exchange.sim.v3`. Native run manifests use
`exchange.run.v1`, input history uses `exchange.history.v1`, events use
`exchange.assurance.v1`, and output manifest/log schemas are documented in
FILES.md. The pricing API is `ph.price.v3`, model `public-prices.v3`, engine
`0.4.0`, objective `operating_balance_tracking` version 1. Its required feedback
has `funding_balance`, `coverage_credit` and `liquidity_buffer`. Expected absolute
balance is the score; expected worker surplus is a diagnostic.

The browser runs C++/WebAssembly with an explicit bounded exact enumeration
backend. It does not run AMPL. The native AMPL interface remains separate with
its existing supervision and vendor terms. Do not claim that a browser worker
test is an AMPL test or that native/WASM parity verifies visual interactions.

The application permits up to 12 SKUs, 16 candidates per SKU, 365 days and at most
200,000 combinations. Validate the whole grid; reject an excessive grid rather
than silently searching only part. A strict input contract does not silently
fill missing fields or convert older schemas. Operating configuration comes
from the supplied `.cfg`; browser example defaults are a build snapshot, not
automatic access to local files.

## Model assumptions

Forecasts update from prior uncensored sales only, normalized by declared price
response. Score the observation before updating. A stockout, including zero stock,
does not train level or errors. Imported history may warm up forecasts but never
adds cash, stock or earned funding. There is no implicit TFT training or connection.
The EWMA baseline does not model trend, seasonality or censored latent demand.

Default history is empty. Initial bread/beans levels are declared `base_demand`
priors of 20/14 units, with an assumed 3-unit initial error and continuing floor.
Eligible prior sales update the level/error with configured weight 0.25. During
a simulation those observations come from synthetic consumers. The seven-point
warmup flag is only a diagnostic state, not an accuracy or data-sufficiency gate.
The 14-row synthetic history file remains available only for explicit examples
and tests. Never mix it with real records without preserving that distinction.

Consumer arrivals, budgets, needs and choices are synthetic. Integer draws keyed
by seed/day/customer/SKU/event pair policies without shifting after different
choices. Purchase probability uses a declared binary-logit response; SKU order
allocates budgets and is a documented limitation. No persistent households,
cross-product substitution or real-customer coefficients are present.

Consumer generation and price normalization use the same declared response
priors. Evaluation against that simulator checks a controlled model, not observed
customer behavior or causal price response. Future TFT work needs a documented
data/feature adapter and past-only comparisons against simpler forecasts on real
observations. No universal minimum count or few-shot reliability is claimed; see
the [model notes and primary references](MODELS.md#future-tft-integration).

Sigma is smoothed RMS one-step error with a configured prior/floor. Report cold
start, warmup, MAE, RMSE, eligible coverage and censoring counts. Sigma bands and
the reserve's common-adverse-error rule are conditional stress assumptions, not
absolute worst cases, 99.7% promises or calibrated insurance prices. Review raw
forecasts and physical/saleable caps together.

**Post-Profit Continuity Assurance** is currently a provider contract name and
an unfunded event record, documented in the separate
[post-profit-assurance placeholder](../../post-profit-assurance/README.md).
A request neither contacts anyone nor transfers money.
No actual insurer, premium, pooled reserve, payout engine or legal coverage is
implemented. A later assurance project must define those responsibilities and
their evidence without retroactively inventing funding in exchange runs.
Preserve canonical event identity `(run_id, policy, event.id)` when exporting
or aggregating logs. The local event ID is only policy/day/type; run IDs are
caller-assigned and not globally enforced. No idempotent payout mechanism exists.

## Build, run and preserve records

Use the already approved optimizer Debian container and its documented launcher
in the [environment guide](../environment/README.md). Do not create a substitute
container, change mounts or install unrelated dependencies. Native commands:

```sh
cmake -S projects/post-profit-exchange -B .build/post-profit-exchange/native -DCMAKE_BUILD_TYPE=Release
cmake --build .build/post-profit-exchange/native --parallel 2
ctest --test-dir .build/post-profit-exchange/native -R '^(exponential_smoothing_ewma|exchange_(models|simulation|files))$' --output-on-failure
.build/post-profit-exchange/native/exchange_simulation --config projects/post-profit-exchange/configs/exchange.cfg
bash projects/post-profit-exchange/build-wasm.sh
```

The runner refuses any existing output path. Select a new path in the `.cfg` for
each saved run; never erase an old run to make a test pass. A manifest is written
last, and a failed write preserves incomplete output for inspection. Replay a
saved `request.json` through stdin without overwriting records. Browser import
must explicitly select `.cfg` and history files; browser paths are not arbitrary
filesystem authority.

Regenerate the HTML and corresponding source archive after source/config changes.
Keep operational run directories and imported history out of release source
archives; only the empty default and declared synthetic sample are intentional
record examples. Include the reusable exponential-smoothing tool's source and MIT license
in the corresponding-source archive. Embedded
application MIT notices do not relicense the restricted engine or third parties.

## Next work requiring evidence

Check current verification results before treating a build as complete. Preserve
past-only forecast tests, paired draws, exact cash/stock/FIFO identities, bounded
enumeration, continuity versus technical failure, insolvency stopping, record
preservation and native/WASM consistency when extending the code.

Economic claims need observed data, honest stockout metadata, held-out forecast
evaluation and demand-response identification; synthetic output is insufficient.
Further work includes richer cost/tax/payment timing, replenishment delays,
forecast misspecification, worker governance and approvals, publication/checkout
interfaces, and the separate assurance operating model. No task here establishes
self-sufficiency, owner-removal benefit or authorization for a live deployment.
