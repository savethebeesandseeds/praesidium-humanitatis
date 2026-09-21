<!-- SPDX-License-Identifier: MIT -->
# post-profit-exchange: store operations, feedback, accounts and evidence

Status: research specification, 2026-09-21. The bounded C++/WebAssembly simulator
implements the daily accounting and feedback rules below under `exchange.sim.v3`.
Its [contract](simulation/CONTRACT.md) is the detailed input/output reference.
Governance, live store interfaces and publication remain application requirements,
not implemented capabilities. Application material is MIT; the
[pricing engine](../../tools/price-optimization/README.md) retains its separate
license. The AMPL server backend also requires applicable vendor rights.

The [adversarial-cooperation research note](../../docs/adversarial-cooperation.md)
acknowledges potential disruption to incumbent owners' income and control and
records the intention to address their interests alongside the potential benefit
of foregoing passive-owner extraction. That protocol is future research.

## Purpose and scope

This **exchange** demonstration belongs to the
[post-profit economics program](../../docs/post-profit-economics.md). The store
also models distribution through procurement and stock, and allocation through
inventory and operating budgets. Its purpose is accessible goods, protected
work and operational continuity, with decisions and surplus under worker control.
There is no passive owner entitled to extract profit or direct the workers.
Software itself cannot establish worker consent or an ownerless legal entity.

The store encompasses procurement, stock, sales, wages and operating obligations,
cash, inventory value and reserves. Prices are one decision within that operation.
Salaries are fixed, protected inputs; the optimizer neither selects nor reduces
them. Factory and laboratory operations belong in separate applications.

Self-sufficiency must be evaluated over a declared horizon, including resource
endowments, missed obligations, inventory and liquidity. One feasible price
decision or positive forecast is insufficient evidence. The current simulator
has explicit opening cash and stock, no new borrowing or outside funding, and
no matched passive-owner-return model. Its fixed-price comparison does not prove
an advantage from removing owner extraction.

## Policy and authority

The implemented price model uses one public price per SKU, fixed wages and
operating costs, a new reserve funding requirement, affordability ceilings,
per-update price-change limits, scenario inventory bounds and feedback rules.
These constrain optimizer recommendations. A policy identifier in the engine request is only
an audit label; the current code does not authenticate approval.

For a live deployment, workers must approve versioned policy, retain recall and
suspension authority, and control realized surplus after obligations. Approval
records need scope, currency, effective and expiry times, evidence of authority
and a revocation mechanism. Live publication must revalidate current state and
authority, including after a solve has started. An old price is not automatically
safe when costs, inventory or policy change.

| Area | Required operating protection | Current boundary |
| --- | --- | --- |
| Worker dignity | Voluntary work, no coercion, retaliation or individual surveillance scoring. | Organizational governance and access controls remain necessary. |
| Compensation | Fixed approved wages and employer obligations; no reduction of pay or covert increase of workload to improve a score. | Daily wages are fixed inputs; staffing and benefits workflows are not modeled. |
| Public prices | Consistent posted prices without identity, distress or willingness-to-pay profiling. | Engine selects one price per SKU; checkout/publication is outside the simulator. |
| Affordability | Worker-approved per-SKU ceilings supported by evidence and customer complaint/correction processes. | Candidate ceilings are enforced; actual accessibility needs evaluation. |
| Price stability | Limits on change and frequency with suitable historical references. | Current cap is per update, not a rolling multiday limit. |
| Continuity | Explicit obligations, reserves, arrears, liquidity and fair shortage procedures. | Daily accounts expose shortages; forecasts do not guarantee future cash. |
| Surplus | Worker authority over realized surplus; no passive-owner extraction or disguised related-party extraction. | No distributions or external payments are executed. |
| Evidence | Fresh, versioned inventory, costs and aggregate demand with appropriate retention. | Synthetic inputs are inspectable; operational data pipelines remain future work. |

All obligations belong to one stated currency and accounting horizon. Repeated
price refreshes must not create another salary bill or restart an already funded
reserve target. An infeasible or invalid decision produces no optimizer
recommendation. The explicitly declared continuity rule below handles financial
infeasibility separately from technical model failure. Explanations must
distinguish local candidate exclusions from a proven
global infeasibility cause; the engine does not provide the latter.

## Price feedback from actual operating results

Good actual sales that leave the store ahead of required funding permit prices
to fall. A realized funding gap permits prices to rise when the demand model
shows that an increase will preserve contribution. The controller aims to bring
funding toward balance. It does not maximize expected surplus, minimize a static
basket price, or run a sequence of surplus-target and basket objectives.

Let raw history `B` start at zero and accumulate the actual FIFO economic result
minus newly scheduled reserve funding. Positive means ahead of required funding;
negative means a shortfall. Opening cash, inventory and endowed reserves do not
count as earnings. For the configured `feedback_recovery_days` horizon, derive
the next signed adjustment `b` by rounding `B / feedback_recovery_days` to cents,
preserving any nonzero sign with a minimum magnitude of one cent. Default is
three days, with a supported range of one through thirty. This changes the
response horizon without resetting the actual history.

For scenario `s`, candidate public prices and supplied quantities yield:

```text
m_s = forecast_sales_contribution_s - fixed_wages - fixed_operations
      - new_reserve_requirement
g_s = b + m_s
minimize sum_s probability_s * abs(g_s)
subject to m_s + C + L >= 0, for every scenario
```

`C` is separately established, cash-backed earned funding available for coverage.
Let liquid cash be cash after procurement less existing wage/operating arrears,
bounded below by zero. `C` is zero unless `b > 0`; otherwise it is the smaller
of positive raw history and liquid cash less existing reserve, bounded below by
zero. `L` is the remaining liquid cash after subtracting `C`. This disjoint
liquidity buffer can include endowed cash and reserves available for obligations;
it supports continuity without being recorded as earned funding. `C` can exceed
the amortized adjustment because control response and liquid earned funding
have different roles. Neither credit nor buffer replaces `b` in the objective.
The engine validates bounds and requires zero credit when `b <= 0`; the caller
establishes the accounting and liquidity evidence.

Every SKU also obeys these hard rules:

- `b > 0`: decrease or hold the previous price.
- `b < 0`: increase or hold. An increase is eligible only when its forecast
  contribution `(price - replacement_unit_cost) * forecast_units` is at least
  the hold candidate's contribution in every supplied scenario.
- `b == 0`: hold. Day one therefore uses the configured reference prices to
  collect actual operating evidence.

The candidate grid must contain the previous price and its scenario forecasts.
Affordability, price-change and inventory constraints still apply. They can block
useful movement or any feasible trade. Holding can be the best feasible balance;
a funding gap is not a promise that increasing prices can recover it. Reductions
also depend on feasible coverage and inventory. The objective cannot assume
demand that the supplied scenarios do not support.

Expected absolute balance differs from the absolute value of expected balance:
opposite scenario deviations cannot cancel. Report signed scenario balances,
the absolute score, coverage slack and realized history separately. Expected
worker surplus remains a diagnostic, not the optimization objective. The model
does not guarantee a deterministic tie choice across solver builds.

The server contract is `ph.price.v3`, engine `0.4.0`, model `public-prices.v3`,
with objective `{id: "operating_balance_tracking", version: 1}`. Its required
`feedback` object carries `funding_balance: b`, `coverage_credit: C` and `liquidity_buffer: L`.
`recommendation.expected.absolute_funding_balance` is the minimized score;
each scenario's `funding_balance` is `b + m_s` and coverage slack is `m_s + C + L`.
The browser uses the same C++ selection checks and an explicitly selected bounded
enumeration backend, with a configured ceiling of at most 200,000 price combinations.
A combination grid beyond that ceiling is a model error, not a partially searched
optimum. AMPL does not run in
the standalone HTML.

## Reserve funding and cash earmarking

Reserve funding requirements and the reserve cash account are separate. Each
day, forecasts from prior observations at currently posted prices estimate
expected contribution and an adverse error amount. Let `F` be fixed daily wages
plus operations, `m_i` current replacement-cost margins, `mu_i` forecast units,
`sigma_i` their one-step RMS errors, `H` the reserve horizon and `z` the configured
sigma multiplier. The dynamic target is:

```text
target = max(configured_reserve_target,
    ceil(H * max(0, F - sum(m_i * mu_i))
         + z * H * sum(abs(m_i) * sigma_i)))
```

For this calculation, means are capped by the smaller of the physical customer
unit maximum and `max(current_stock, target_stock)`; sigma is capped by the
physical maximum. The stock target is a supply heuristic, not proof that the
store can finance every replenishment. The error aggregation assumes common
adversity across products and days, without diversification. It is a conditional
stress target, not an absolute worst-case guarantee or a calibrated insurance
probability. [Model notes](docs/MODELS.md) distinguish it from the forecaster's
uncorrelated-innovation horizon calculation.

The simulator schedules new funding toward that target above endowed reserve:

```text
new_reserve_requirement = min(reserve_contribution,
    max(0, target - initial_reserve - cumulative_scheduled_requirement))
B_next = B + actual_FIFO_economic_result - new_reserve_requirement
```

Scheduling the requirement records what operating results must fund; it does
not assert that the funds have been earned or placed in reserve. A shortfall
remains in `B`. A rising target can schedule additional funding; a falling target
does not erase earlier requirements or invent a refund. Releasing reserve to pay obligations or later re-earmarking cash
does not restart the schedule, create earnings or change `B`. An operating loss
already entered in history must not be charged again as a new reserve target.

Actual reserve is a subset of total cash. After obligations, if no arrears remain,
the daily allocation is limited by the configured contribution, remaining actual
target and cash not already reserved. This may restore an earmark using earlier
funding even when today's economic result is zero. Earmarking opening cash does
not turn an endowment into earned feedback. Transfers change available cash,
not total cash or economic result.

## Daily operations and accounts

The simulator runs one pricing update per day, using integer minor currency
units and whole product units. The full bounds and rounding rules are in the
[simulator contract](simulation/CONTRACT.md).

1. Observe the day's replacement costs and calculate forecasts from prior
   eligible sales. The configured demand shock changes actual arrivals and is
   not supplied to the forecaster in advance. Recalculate the reserve stress target.
2. Replenish toward target stock in configured product order, protecting reserve,
   existing arrears and today's wages/operations. Cash and daily purchase budget
   limit spending; deliveries are immediate. Procurement is a rule, not an
   optimized variable, and happens before price selection.
3. Schedule new reserve funding and solve using stock, replacement costs, fixed
   obligations, three demand scenarios and actual-history feedback. The fixed
   comparator holds its literal configured prices, with its own forecast/history
   and coverage calculation.
4. Use a recommended price or, on financial infeasibility, retain the existing
   valid public price under the declared continuity policy. Generate visiting
   consumers, needs and purchase choices; sell only budget-affordable units in
   stock. A technical model error rolls back that day's procurement/scheduled
   funding and ends the path, without a simulated trading day.
5. Spoil remaining stock at the configured rate. Value sold and spoiled units
   using FIFO inventory lots, and accrue wages and operating costs once.
6. Pay wage arrears first and operating arrears second, releasing reserve if
   needed. Unpaid amounts remain liabilities; cash never goes negative and no
   debt is invented.
7. Score forecasts before learning from the day's uncensored sales. Update raw
   funding history, re-earmark reserve when possible and reconcile accounts.
   Record assurance requests and stop the path if cash is exhausted or fixed
   obligations remain unpaid.

Opening stock is a separate endowment valued at the unshocked unit cost; it is
not purchased again from opening cash. Purchases reduce cash and add inventory.
Expense recognition occurs when goods are sold or spoiled. Paying an old arrear
settles its liability without recognizing the expense again.

```text
actual_contribution = revenue - FIFO_cost_of_goods_sold - FIFO_waste_cost
economic_result = actual_contribution - newly_due_wages - newly_due_operations
actual_required = newly_due_wages + newly_due_operations + new_reserve_requirement
actual_funding_result = actual_contribution - actual_required
closing_cash = opening_cash - procurement + revenue - wages_paid - operations_paid
closing_stock = opening_stock + purchases - sales - spoilage
closing_inventory_value = opening_inventory_value + purchase_cost
                          - cost_of_goods_sold - waste_cost
cash + inventory_value - wage_arrears - operating_arrears
    = initial_cash + initial_inventory_value + cumulative_economic_result
```

Forecasts value contribution at current replacement costs; realized accounts use
FIFO historical costs. A cost shock can therefore produce different signs in
forecast surplus and realized economic result even without demand error. Display
both cost bases, not a single interchangeable profit measure.

The research ledger includes merchandise, spoilage, fixed wages, fixed operations,
reserve earmarking and arrears. A live application additionally needs explicit
tax, payment-fee, benefits, depreciation, financing, supplier-due-date and worker
distribution treatment where applicable. These are not independently implemented
ledger categories today. Amounts must be classified once, with source, currency,
recognition period and payment timing; avoid disguising unsupported obligations
inside an unrelated field. Forecast feasibility is not a full liquidity plan.

## Forecasts, consumers and comparison

The standalone consumer module uses arrival, budget, need and binary-logit
purchase/no-purchase gates. It exposes budget rejections and stock-lost units.
Its reference demand and utility slopes are declared cold-start priors, not
measurements of real customers. Configured SKU order allocates budget first to
earlier products; there is no persistent household, substitution or joint-basket
choice model.

Each SKU has an EWMA level of price-normalized sales and a smoothed one-step
squared-error estimate. Only prior, open, uncensored sales update the model.
Predictions are scored before updating; sellouts and zero-stock observations are
excluded. Warmup, MAE, RMSE and configured-band coverage remain visible, including
the selection bias from excluding stockouts. `z` sigma is an adversity assumption,
not a claimed coverage probability. Imported negative-day observations train
forecasts without altering opening resources or earned funding. No TFT is
silently trained or invoked.

Three scenario offsets around each forecast mean use configured sigma stress
and probability weights. The application exposes demand before stock limits,
capped by physical visitor/unit capacity, separately from saleable scenario units
capped to available inventory. The engine receives the saleable units and checks
them; it does not invent latent demand or decide replenishment. Realized unmet
demand remains visible independently.

Both policies start with identical resources and use matching random keys for
day, customer, SKU and event. They realize purchases at their own prices and
stock, then learn from their own resulting sales. Future demand shocks and
future actual choices are unavailable to forecasts. The comparison therefore
does not replay identical sales across different policies; their forecasts,
cash, procurement, inventory and terminal dates can diverge.

## Continuity, assurance and terminal outcomes

Temporary projected losses can use a disclosed cash buffer. If pricing remains
financially infeasible, the research continuity rule retains the current public
prices and records that forecast coverage was not certified. It does not call
that result an optimizer recommendation. Technical invalid/unavailable/failed
solves are separate model errors and do not trigger this continuation.

The application emits `exchange.assurance.v1` events addressed by contract name
to **Post-Profit Continuity Assurance**. Coverage failure, a configured fixed-cost
cash-buffer breach or exhausted cash can trigger `assurance_requested`. Each
event records the reason, requested support, cash, reserve, inventory value,
arrears, funding history and forecast evidence. Settlement is explicitly
`unfunded_request`; the prototype neither contacts a provider nor receives money.

Zero opening cash ends the path at day zero. Subsequently, zero cash after fixed
obligations or remaining wage/operating arrears records `insolvency_declared` and
ends that policy. No extra days are fabricated. A model-error path terminates
separately. The comparator may run longer; totals over unequal observed horizons
must not be presented as an equal-duration treatment effect. This simulated
insolvency rule is a model condition, not a legal determination.

## Records and inspectability

The central `.cfg` uses strict JSON and contains all operational settings and
products. The checked-in example is the source of defaults embedded at build
time; a supplied `.cfg` is authoritative for a native run. History and output
paths are explicit. [File records](docs/FILES.md) include the complete request,
configuration/history snapshots, result, daily records, assurance events and a
completion manifest. Existing output paths are refused. There is no database.

The standalone interface exposes all inputs, signed accumulated funding with a
zero target, actual contribution versus requirements, prices, stock, cash,
reserves, unpaid obligations, waste and unmet demand. Daily and product ledgers,
scenario scores and movement reasons remain inspectable and exportable. A hold
reason is explanatory evidence, not proof of global infeasibility.

Live decision records must additionally connect authenticated policy and data
versions, engine/model versions, selected prices, objective score, constraints,
approval, expiry and publication identifiers. A supervised server solve has
deadline and cancellation controls; those do not implement checkout, worker
authority or atomic publication.

## Evaluation and acceptance criteria

1. Replay identical configuration and seeded draws consistently. Check bounded
   decisions against independent enumeration and the AMPL formulation where used.
2. With forecasts unchanged, positive actual funding can lower prices; a realized
   gap can raise prices only when contribution is preserved in every scenario.
   Zero history holds. Test cases must also expose blocked recovery and infeasibility.
3. Keep wages fixed. Do not satisfy a score by reducing compensation, raising a
   configured affordability cap or inventing liquid coverage credit/buffer.
4. Reconcile purchases, FIFO sales, spoilage, cash and arrears exactly. Schedule
   reserve target funding once; releasing/rebuilding the cash earmark must not
   duplicate an operating loss or treat opening funds as earnings.
5. Compare feedback with fixed public prices using paired external draws and
   price-specific sales. Report affordability, access, wages paid, continuity,
   cash and stock alongside economic result and funding balance.
6. Stress no visits, no purchases, budget/stock limits, unrevealed demand shocks,
   observed cost shocks, forecast error, FIFO cost differences and exhausted cash.
   Distinguish continuity trades from certified recommendations, and technical
   model errors from insolvency. Assurance requests cannot fabricate funding.

The [verification record](../../docs/exchange-simulation-verification.md) states
the checks actually performed and their limits. Further research must evaluate
demand misspecification, delayed supplies, workload, environmental costs and
customer access. Operational integration needs authenticated governance, real
accounting/data evidence, worker-controlled suspension and a bounded shadow-mode
pilot before live prices. Synthetic settings are not approved store policy or
evidence of self-sufficiency.
