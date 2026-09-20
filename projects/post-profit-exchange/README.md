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
current stage includes a synthetic C++/WebAssembly simulator using the existing
pricing objective; economic viability remains to be evaluated.

We acknowledge that this model could disrupt existing stores and their owners'
income and control. We refer incumbent store and factory owners to our
[Adversarial Cooperation work](https://github.com/savethebeesandseeds/adversarial-cooperation)
and the [planned protocol research note](../../docs/adversarial-cooperation.md).
We intend to construct a protocol that addresses their interests alongside the
potential advantage of foregoing passive-owner profit extraction. That advantage
has not been demonstrated by the synthetic optimizer tests. Worker compensation,
operating costs, reserves and worker-controlled surplus remain part of the model.

The [store specification](STORE_SPECIFICATION.md) defines the
next simulator and application: hard protections, a worker-approved surplus
target followed by essential-basket affordability, separate economic and cash
accounts, worker authority, and evidence required to assess outcomes. These
are proposed application requirements; the existing engine behavior is
described below.

The optimizer now has a [supervised JSON backend](../../tools/price-optimization/docs/backend.md)
for the synthetic dashboard example. That boundary separates policy, costs,
candidates and forecasts and returns validated explanations. It implements
the existing engine objective, not the proposed application's later stages.

## Project and tool boundary

This project owns the store's operating model, state, workflows, simulator and
plain browser controls/charts. The browser build calls the shared C++ pricing
core through an explicitly selected bounded enumeration backend. A future
Linux service adapter can use the optimizer's `ph.price.v1` AMPL interface.
The reusable tool owns the pricing formulation, solving and independent result
checks; it has no dependency on this application's code.

Policy, objective and costs define the optimization problem. Prices are selected
decision values returned by a solve and reconsidered as store conditions change.
The application is responsible for deciding whether a recommendation may be
applied to the current store state. A price recommendation alone does not run
the store or establish its self-sufficiency.

This folder contains the research definition, store specification and an offline
simulator with inventory, cash, FIFO economic accounts and paired price-policy
comparisons. Operational integration, governance and broader risk management
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

The C++ simulator supports 1–3 products, up to 9 candidate prices each and 365
days. Its offline backend solves the existing finite pricing formulation by
[bounded enumeration](../../tools/price-optimization/docs/enumeration.md),
with shared core validation. **AMPL does not run in this HTML.** The existing
AMPL/HiGHS backend remains separate. The simulator compares optimized and fixed
public prices under the same seeded external demand draws and scheduled shocks.

Opening cash and stock are explicit endowments. Procurement is a cash-limited
rule, not an optimized decision. Actual inventory uses FIFO; planner forecasts
use replacement costs. Reserve allocation changes earmarked cash, not expenses.
Every period reconciles cash, stock, inventory value and net assets. Failed
pricing decisions trade nothing; outstanding obligations and unmet demand remain
visible. These are synthetic assumptions, not calibrated store evidence.

The [simulation contract](simulation/CONTRACT.md) states every input, limit,
daily rule and accounting identity. The proposed sequential surplus-target and
essential-basket objective remains future work. The simulation has no live
publication, payments, authenticated governance or owner-return comparison.

Build instructions and pinned compiler provenance are in the
[environment guide](environment/README.md). In that existing Linux environment:

```sh
cmake -S projects/post-profit-exchange -B .build/post-profit-exchange/native -DCMAKE_BUILD_TYPE=Release
cmake --build .build/post-profit-exchange/native --parallel 2 --target exchange_simulation exchange_simulation_test
ctest --test-dir .build/post-profit-exchange/native -R '^exchange_simulation$' --output-on-failure
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
approved bounds and projected budget coverage in every supplied scenario. It seeks expected
worker-retained surplus after those obligations. It has no customer identity,
individual willingness-to-pay, worker scoring, or competitor-coordination input.
Scenario feasibility is conditional on those forecasts, not a guarantee of
future sales or self-sufficiency.

The initial C++/AMPL prototype produces recommendations. The application must
revalidate a recommendation against current inventory, policy, and time before
publication. Infeasible, stale, invalid, or failed solves produce no new price
recommendation; route them to worker review. A prior price is not automatically
safe to retain when costs or policy have changed.

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
