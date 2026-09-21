# post-profit-exchange

License: MIT for this application; the
[price optimization engine](../../tools/price-optimization/README.md) has its
own [Worker Protection License](../../tools/price-optimization/LICENSE).

This project is an **exchange** demonstration in Praesidium Humanitatis's
[post-profit economics program](../../docs/post-profit-economics.md), with
connected models of **distribution** through stock and procurement and
**allocation** of inventory, budgets and surplus. It researches how to run a self-sufficient
store for the benefit of workers and the people it serves.

Its scope is the operation of the store: stock,
procurement, sales, accounts, protected worker compensation and continuity.
Real-time public prices are a decision produced by solving an optimization
formulation within that operation. In the proposed model, workers collectively
govern the store, approve its operating policy and decide the use of surplus.
There is no passive owner entitled to extract profits. Legal ownership
arrangements depend on the eventual jurisdiction; software cannot itself create
an ownerless legal entity.

"Post-profit" means operating without passive-owner profit extraction. Worker
pay, necessary costs, reserves and worker-controlled surplus remain essential.

The research question is whether this model can sustain protected work,
accessible goods and operational continuity under declared resource limits and
shocks. Formalization covers the store's governance, flows and accounts,
constraints and objectives. The demonstration must compare operating policies
and measure unmet demand, affordability, worker outcomes and continuity. Its
current stage includes a C++/WebAssembly simulator with synthetic consumers,
forecasting from prior uncensored sales, funding feedback, dynamic reserve stress
and explicit continuity/insolvency records. Economic viability remains to be evaluated.

We acknowledge that this model could disrupt existing stores and their owners'
income and control. We refer incumbent store and factory owners to our
[Adversarial Cooperation work](https://github.com/savethebeesandseeds/adversarial-cooperation)
and the [planned protocol research note](../../docs/adversarial-cooperation.md).
We intend to construct a protocol that addresses their interests alongside the
potential advantage of foregoing passive-owner profit extraction. That advantage
has not been demonstrated by the synthetic optimizer tests. Worker compensation,
operating costs, reserves and worker-controlled surplus remain part of the model.

The [store specification](STORE_SPECIFICATION.md) defines the implemented
feedback mechanism and the broader operating requirements: hard protections,
fixed worker compensation, separate economic and cash accounts, worker
authority, and evidence required to assess outcomes. Earned funding permits
price reductions; a realized shortfall permits increases only when those
increases preserve forecast contribution in every supplied scenario.

The optimizer now has a [supervised JSON backend](../../tools/price-optimization/docs/backend.md)
for the synthetic dashboard example. That boundary separates policy, costs,
candidates and forecasts and returns validated explanations. It implements
the same `operating_balance_tracking` objective used by the simulator.

## Project and tool boundary

This project owns the store's operating model, state, workflows, simulator and
plain browser controls/charts. The browser build calls the shared C++ pricing
core through an explicitly selected bounded enumeration backend. A future
Linux service adapter can use the optimizer's `ph.price.v3` AMPL interface
(engine `0.4.0`, model `public-prices.v3`, objective version `1`).
The reusable tool owns the pricing formulation, solving and independent result
checks; it has no dependency on this application's code.

The independent MIT [simple exponential smoothing tool](../../tools/exponential-smoothing/README.md) supplies
the EWMA core through `ph::exponential_smoothing` and `ph/exponential_smoothing/ewma.hpp`. It is pure
C++17, used by both native and WebAssembly builds. This project retains the
consumer model, price-response assumptions, stockout handling, JSON adapters and
reserve policy. The separate [TFT tool](../../tools/temporal-fusion-transformer/README.md)
remains available for future evaluation; exchange does not call or train it.

Policy, objective and costs define the optimization problem. Prices are selected
decision values returned by a solve and reconsidered as store conditions change.
The application is responsible for deciding whether a recommendation may be
applied to the current store state. A price recommendation alone does not run
the store or establish its self-sufficiency.

This folder contains the research definition, store specification and an offline
simulator with inventory, cash, FIFO economic accounts and paired price-policy
comparisons. Configuration and persistent research records are ordinary local
files, with no database. Operational integration and authenticated governance
are later work.

Factory and laboratory demonstrations would study **production** in separate
projects under `projects/`, with their own operating specifications and tool
integrations. They are outside this store project's scope; no such project
folders are created yet.

## Standalone WebAssembly simulation

Open [post-profit-exchange.html](dist/post-profit-exchange.html) directly in a
modern browser. It embeds the compiled C++ WebAssembly, controls and SVG charts;
it needs no server, CDN, AMPL installation or network connection. The file also
contains component licenses and a downloadable ZIP of corresponding source.

Change the numeric inputs, then use **Step one day** or **Run all days**. **Stop**
terminates the simulation worker. The page exposes all parameters, exact daily
ledgers, product flows, price and inventory charts, cash, reserves, unpaid wages,
waste and unmet demand. JSON/CSV exports preserve results. Values are integer
minor currency units; one simulated period is one day, with one pricing update.

Use **Open central .cfg** and **Open history records .json** to load a saved run
definition and its separate observation file. Browser file access is explicit;
the page cannot silently read neighboring paths. The controls and JSON records
expose consumer assumptions, forecasts, sigma stress, reserve targets and unfunded
assurance events. No real customer history is supplied by this repository.

The default run starts with empty history. Bread and beans begin at declared
reference-price demand priors of 20 and 14 units, with an assumed error floor of
3 units. Eligible simulated sales then update those levels with weight 0.25.
The seven-observation warmup flag does not establish data sufficiency or accuracy.
The optional 14-row history example is synthetic and must be selected explicitly.
Because forecast normalization and consumer generation share price-response
assumptions, good simulation scores do not establish empirical or causal accuracy.

The C++ simulator supports 1–12 products, up to 16 candidate prices each and 365
days. Its offline backend solves the finite feedback pricing formulation by
[bounded enumeration](../../tools/price-optimization/docs/enumeration.md),
with a configured maximum of 200,000 price combinations and shared core
validation. A grid beyond the chosen bound is a model error, not a truncated
search. **AMPL does not run in this HTML.** The existing
AMPL/HiGHS backend remains separate. The simulator compares balancing feedback
and fixed public prices under the same seeded external demand draws and scheduled
shocks. Both see matching visit, budget, need and choice draws, then fulfill
purchases at their own prices and inventory. Product order affects budget use.

Opening cash and stock are explicit endowments. Procurement is a cash-limited
rule, not an optimized decision. Actual inventory uses FIFO; planner forecasts
use replacement costs. The signed accumulated funding balance records actual FIFO
economic result minus newly scheduled reserve funding. Positive means ahead;
negative means shortfall. The `feedback_recovery_days` control spreads that
history over the next decision's adjustment; it does not erase the history.

The reserve target combines a configured minimum with a forecast stress estimate
of fixed-cost shortfalls and adverse contribution errors. Sigma is learned from
past one-step errors, with a configured prior/floor; it is not a worst-case or
insurance guarantee. New funding toward the target above initial reserve is
scheduled cumulatively.
Releasing and re-earmarking reserve cash changes available cash, not expenses or
earned funding, and does not charge the same target again after an operating loss.
Every period reconciles cash, stock, inventory value and net assets.

A financial forecast shortfall does not automatically close the store. Available
operating liquidity can support protected forecast coverage without becoming an
earned-surplus signal. If no coverage-feasible pricing decision exists, the
declared continuity policy retains the current public prices and emits an
unfunded [Post-Profit Continuity Assurance](../post-profit-assurance/README.md)
request. Cash exhaustion after
fixed obligations, or unpaid obligations, produces insolvency and ends that
policy path. Technical model errors terminate separately. The other path may
continue, so compare terminal dates along with totals. No assurance payout or
external funding is invented.

The [simulation contract](simulation/CONTRACT.md) states every input, limit,
daily rule and accounting identity under `exchange.sim.v3`. The simulation has no live
publication, payments, authenticated governance or owner-return comparison.
The [model notes](docs/MODELS.md) explain cold-start priors, EWMA learning,
censoring, binary-logit consumers, sigma assumptions and their limitations.

## Central configuration and run records

The [example .cfg](configs/exchange.cfg) is strict JSON containing complete
simulation settings and paths to history and a new output directory. Paths
resolve relative to the configuration file. The [file contract](docs/FILES.md)
describes schemas, replay and failure handling. In the existing Linux environment:

```sh
.build/post-profit-exchange/native/exchange_simulation --config projects/post-profit-exchange/configs/exchange.cfg
```

The runner writes configuration, history and request snapshots, the full result,
per-policy daily JSON files, assurance events and a manifest written last. It
refuses an existing output directory and preserves incomplete output on error.
Use a new output path for another run. Imported negative-day sales train the
forecaster; they do not add cash, inventory or earned feedback. The default
configuration points to [empty-history.json](records/empty-history.json).
[sample-history.json](records/sample-history.json) is an opt-in synthetic fixture,
not a source of real observations. A following development session should begin with the
[handoff record](docs/HANDOFF.md).

Build instructions and pinned compiler provenance are in the
[environment guide](environment/README.md). In that existing Linux environment:

```sh
cmake -S projects/post-profit-exchange -B .build/post-profit-exchange/native -DCMAKE_BUILD_TYPE=Release
cmake --build .build/post-profit-exchange/native --parallel 2
ctest --test-dir .build/post-profit-exchange/native -R '^(exponential_smoothing_ewma|exchange_(models|simulation|files))$' --output-on-failure
bash projects/post-profit-exchange/build-wasm.sh
```

Native and browser builds use the same simulator source. The root CMake build
can optionally include the native application with `-DPH_BUILD_EXCHANGE=ON`.
The [verification record](../../docs/exchange-simulation-verification.md)
records native/WASM parity and the current visual-review limitation.

## Price decisions within store operations

Each pricing cycle receives a versioned inventory snapshot, candidate public
prices, procurement costs, demand scenarios, worker-approved affordability and
price-change limits, and a budget for pay, operations, and reserves. Demand
responses to price must be estimated and evaluated separately: a time-series
forecast alone does not establish price elasticity.

The current engine selects one public price per product, subject to the
approved bounds and projected budget coverage in every supplied scenario.
Its objective minimizes expected absolute funding balance after adding that
period's forecast surplus to the signed history adjustment. Wages are fixed
inputs. Earned, cash-backed funding can support a current shortfall through a
separate coverage credit. Other available cash is an explicit liquidity buffer,
also usable for temporary forecast coverage. Neither becomes earned history or
changes the signed feedback direction.

With positive history, prices may decrease or hold; with negative history they
may increase or hold; with zero history they hold. Day one therefore holds the
reference prices while collecting actual operating evidence. An increase must
preserve forecast contribution relative to holding in every scenario, so weak
sales do not trigger a rise that would worsen the modeled funding gap. Caps,
inventory or demand response can block movement or recovery; the result makes
holds and failed decisions visible. The goal is to approach funding balance,
not maximize surplus or minimize a static basket price.

The engine has no customer identity,
individual willingness-to-pay, worker scoring, or competitor-coordination input.
Scenario feasibility is conditional on those forecasts, not a guarantee of
future sales or self-sufficiency.

The C++/AMPL interface produces recommendations. A live application must
revalidate a recommendation against current inventory, policy, and time before
publication. Infeasible, stale, invalid, or failed solves produce no new optimizer
recommendation. The simulator's explicit continuity rule is an application
decision, not a certified solver result or a general authorization for a live
store to retain prior prices.

## Decisions still required for a live pilot

1. Worker governance: collective policy approval, democratic recall, appeals,
   manual suspension, and surplus allocation after agreed obligations.
2. Store scope: currency and tax treatment, suppliers, inventory and payment
   interfaces, essential-goods definition, and lawful affordability rules.
3. Demand evidence: price-response estimation, uncertainty, stockouts, waste,
   substitutions, seasonality, and evaluation on held-out observations.
4. Publication service: authenticated policy versions, atomic price updates,
   idempotency, audit records, reconciliation, and a worker-controlled stop.
5. Real-time requirements: maximum data age, solve budget and measured latency
   on realistic product counts. The current library is synchronous and has no
   hard real-time guarantee.
6. Appropriate AMPL/solver rights for the actual operational deployment.

Begin in shadow mode, measure projected versus realized costs and outcomes, then
let workers approve a bounded pilot. Factory and laboratory projects may reuse
appropriate tools later, with their own scheduling, resource and safety models.
