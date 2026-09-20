<!-- SPDX-License-Identifier: MIT -->
# post-profit-exchange: store operations, policy, objective, costs and evidence

Status: proposed application specification, 2026-09-20. This document defines
the next simulator and application; it does not change the implemented engine
or authorize live price publication. Application material is MIT. Calling the
[optimization engine](../../tools/price-optimization/README.md) remains subject
to its separate license and the applicable AMPL/solver terms.

Implementation note: the [standalone WebAssembly simulator](README.md#standalone-webassembly-simulation)
now exercises a bounded subset: synthetic daily procurement, sales, FIFO accounts,
cash, reserves and the existing expected-surplus pricing objective. Its
[contract](simulation/CONTRACT.md) identifies implemented rules and limits. The
broader requirements and sequential objective below remain a research specification.

The [research framing and adversarial-cooperation note](../../docs/adversarial-cooperation.md)
acknowledges the model's disruptive potential and records our intention to
construct a protocol addressing incumbent owners' interests alongside the
potential advantage of foregoing passive-owner profit extraction. This is a
future research track; it is not implemented by this specification or engine.

## Purpose and values

This specification formalizes a proposed **exchange** demonstration within
Praesidium Humanitatis's [post-profit economics program](../../docs/post-profit-economics.md).
The store studies exchange through its public-price mechanism, **distribution**
through stock and procurement, and
**allocation** through inventory access, protected budgets and worker-controlled
surplus. Its research question is whether these arrangements can sustain
accessible goods, decent work and continuity under declared operating conditions.

Operate a store that sustains decent work, makes useful goods
accessible, and retains its surplus under worker control. Automation serves
the workers; it receives no independent authority over them. No passive owner
receives a share of the surplus or controls the optimization policy.

The application encompasses stock, procurement, sales, economic and cash
accounts, compensation and continuity. Real-time pricing is its first optimized
decision: policy defines the feasible choices, costs and demand describe their
consequences, and the objective selects prices within those constraints. Prices
are an output of this formulation, not the purpose or full scope of the project.
This is an intended operating model; the current tool returns synthetic price
recommendations and does not implement the store's operational workflows.

Factory and laboratory operating models belong in separate application folders.
This specification is scoped to `post-profit-exchange`; shared numerical capabilities
remain in separately licensed tools.

Self-sufficiency means covering recurring obligations and maintaining inventory,
equipment and liquidity over a declared evaluation horizon. A profitable price
recommendation is not evidence of this. Startup capital, debt and subsidies
must be visible; continuing dependence on new funding must be reported.

The design has six separate parts:

| Part | Question it answers |
| --- | --- |
| Policy | What is permitted, required or forbidden? |
| Objective | Which permitted outcome should we prefer? |
| Costs and accounts | What resources and obligations does operating the store consume? |
| State and evidence | What do we know, how recent is it, and what is uncertain? |
| Authority and actions | Who may approve a change, and what may the software execute? |
| Evaluation | Did workers, customers and the productive unit actually benefit? |

## Policy: conditions that cannot be traded away

Each policy version has an identifier, content digest, scope, currency,
effective/expiry times, approving workers or authorized delegates, approval
evidence, and a revocation mechanism. It declares its planning horizon,
accounting period, refresh cadence and solve deadline separately. The
application authenticates these records before calling the engine.

Workers choose the rules through collective governance. Routine authority may
be delegated with explicit limits and recall. Neither a majority vote nor an
optimizer setting authorizes uses prohibited by the engine's license. Worker
approval is necessary but cannot alone establish that a policy is fair to
customers; provide an accessible complaint and correction process as well.

| Policy area | Required rule | Enforcement boundary |
| --- | --- | --- |
| Worker dignity | Work is voluntary. No coercion, retaliation, individual productivity scoring or surveillance-based pricing. | Organization and data-access controls; software cannot prove consent. |
| Compensation and workload | Approved pay and employer obligations are protected costs. Pricing cannot cut pay, remove benefits or silently demand more labor. Staffing and safety limits are fixed inputs to this first application. | Policy approval and accounts; the pricing solver has no employment action. |
| Public prices | One posted price per SKU and applicable period, with consistent checkout treatment. No personal willingness-to-pay or distress scoring. | Input contract, solver and publication service. |
| Affordability | Per-SKU ceilings and a defined essential basket; any basket ceiling is a hard constraint. Protect essentials during shocks with explicit emergency rules. | Solver plus independent validation; the ceilings need real-world evidence and periodic review. |
| Price stability | Bound price changes and their frequency. Measure limits against retained reference prices over specified rolling windows, so repeated small updates cannot evade a daily cap. | Historical state and publication controls. |
| Essential access | Define service targets, shortage reporting and fair manual contingency procedures. Do not conceal stockouts by reducing the demand forecast. | Simulator, inventory operations and worker review. |
| Financial continuity | Cover protected obligations, maintain a minimum usable cash balance and specified reserve balances, and honor payment dates under the approved stress scenarios. | Economic and cash ledgers plus scenario checks. |
| Surplus | Workers approve its use. No passive-owner distribution, disguised extraction through related-party charges, or automatic transfer of forecast surplus. | Governance, cost review and payment permissions. |
| Evidence and privacy | Use versioned, sufficiently fresh inventory, costs and aggregate demand evidence. Retain only necessary audit data with an approved retention period. | Data pipeline and access controls. |
| Human control | Workers can suspend automation, revoke policy, inspect recommendations and appeal decisions. Overrides are recorded and still checked against non-negotiable protections. | Application authorization and operating procedures. |

All financial obligations use the same declared horizon and currency. A
one-minute pricing refresh does not create a new monthly wage or reserve bill:
reconcile obligations already paid or funded and model the remaining period.
Accounting-period boundaries must not reset rolling price protections.

No feasible decision means no automatic publication. Report the conflicting
constraints and the evidence used. Route operation to the approved worker-led
contingency procedure. Do not silently reduce wages, raise affordability caps,
invent demand or spend unavailable reserves. An old price may be retained only
if it remains valid under current policy and state.

## Objective: shared benefit within policy

The proposed store default uses sequential optimization. Complete each stage,
then preserve its optimum, within an explicit policy-approved tolerance,
before solving the next. Do not collapse dignity, affordability and income
into an unexplained weighted score.

1. **Require policy feasibility.** Hard protections are constraints, not penalties
   that sufficient revenue can outweigh.
2. **Reduce shortfall against a worker-approved discretionary surplus target.**
   Guaranteed compensation is already protected; this target is additional
   worker benefit and may remain unmet without making a valid plan dishonest.
3. **Reduce the public price of a fixed essential basket.** Once the best
   attainable target performance is preserved, prefer greater customer access.
4. **Reduce avoidable waste**, then **unnecessary price changes**, using declared
   measures and the same preservation rule for earlier stages.
5. **Prefer additional worker-retained surplus** when the preceding outcomes
   are equivalent. Use a deterministic final tie-break for reproducibility.

For scenario `s`, let `A_s` be the full accounting period's projected economic
surplus after protected costs and required allocations: reconciled results
already earned plus the forecast for the remaining period. Let `T >= 0` be
the approved discretionary surplus target for that same full period, and
`rho_s` the scenario probability. Stage 2 is:

```text
shortfall_s = max(0, T - A_s)
minimize sum_s rho_s * shortfall_s
```

Already earned worker surplus counts toward target achievement even if workers
have received its distribution; that payment reduces cash, not earned progress.
Alternatively an implementation may use a remaining target and remaining
result, but it must reconcile progress exactly once. Never pair a full-period
target with only remaining-period earnings or restart the target at each
pricing refresh.

This measures shortfall within scenarios: a large upside in one scenario does
not erase a shortfall in another. Report expected shortfall and the worst
scenario separately. Probabilities and supplied scenarios are assumptions,
not guarantees about every possible future. If the target cannot be met,
report that result and continue only with hard protections intact.

For fixed policy basket quantities `b_i`, stage 3 minimizes:

```text
basket_price = sum_i b_i * public_price_i
```

Basket quantities are independent of predicted sales and remain fixed during
the solve. Otherwise, suppressing essential purchases could misleadingly
improve the affordability score. Prices used for this basket must reflect the
amount customers actually pay. Basket availability is measured separately;
cheap unavailable goods are not access.

Targets, basket contents, stage priorities and tolerances are public policy
choices, versioned and reviewable. Setting a very high surplus target can make
affordability improvement unreachable; show that tradeoff in simulation before
workers approve it. Waste measures must specify comparable units or actual
disposal/write-off costs. Price-change measures must declare their reference
window and scale. Never invent exchange rates between rights and money.

**Implemented engine behavior differs:** it currently maximizes expected worker
surplus after unit costs, fixed pay, operations and a reserve contribution,
subject to per-product affordability/change limits and coverage in every
supplied scenario. It has no surplus target, basket objective, waste model,
cash ledger or sequential objective stages. The proposed objective needs an
explicit AMPL/API change and new verification before the application can use it.

## Costs: complete, explicit and counted once

Keep an economic-result ledger and a cash ledger. Each cost has a category,
source, amount/currency, unit or period basis, recognition period, payment date,
uncertainty and approval record. Mark estimates as estimates. Changing a cost
classification cannot create surplus or remove an obligation.

| Category | Economic treatment | Cash treatment |
| --- | --- | --- |
| Merchandise | Landed cost of units sold; retain unsold inventory in the inventory ledger. | Supplier payments follow their actual due dates. |
| Waste and shrinkage | Record losses of stock once, plus distinct disposal costs. Never count the same unit as both sold and spoiled. | Inventory may have been paid for earlier; disposal may create a new payment. |
| Transaction costs | Payment processing, packaging and delivery attributable to sales. Model price-dependent fees explicitly. | Settlement deductions and invoices follow the relevant timing. |
| Worker compensation | Approved wages, benefits, paid leave and employer obligations. Guaranteed pay is not contingent on surplus. | Track payroll and other payment dates, including arrears. |
| Operations | Energy, occupancy, insurance, maintenance, software, accounting and other approved services, assigned to the proper period. | Bills may be paid before or after the expense is recognized. |
| Equipment | Recognize use/depreciation under the chosen accounting policy. Repairs and improvements need explicit classification. | Equipment purchases consume cash when paid. |
| Financing | Approved interest and financing fees are distinct from distributions to owners. | Principal repayment consumes cash but is not another operating expense. New borrowing is funding, not sales revenue. |
| Taxes and pass-through amounts | Separate amounts collected for others, recoverable amounts and actual store expenses. Treatment is deployment-specific configuration. | Record collection, recovery and remittance dates separately. |
| Reserves and reinvestment | Earmarking surplus is an allocation, not an additional expense. Recognize actual resulting expenses or assets separately. | Transfers between the store's own accounts do not reduce total cash; they change what is available to spend. |
| Worker surplus distributions | Allocation of realized distributable surplus after obligations; separate from protected compensation. | Pay only after reconciliation and liquidity checks, under worker authority. |

A purchase's freight cost, for example, belongs either in landed unit cost or
in a separate expense, never both. Document the inventory valuation basis and
show replacement-cost forecasts separately from recorded cost. Do not mix
customer tax-inclusive prices with tax-exclusive margins without a defined
conversion and reconciliation.

An economic view for each period/scenario is:

```text
net_revenue = sales excluding pass-through collections
economic_result = net_revenue
                  - cost_of_goods_sold - stock_losses - transaction_costs
                  - protected_worker_compensation - operating_expenses
                  - depreciation - financing_expenses - applicable_tax_expense
A = economic_result - required_reserve_contribution
                    - other_committed_surplus_allocations
```

The allocations in `A` must not duplicate costs already recognized. Their
amounts come from a reconciled funding plan; if planned equipment spending is
already funded, the optimizer must not fund it again on every refresh.

The cash view is separate:

```text
closing_cash = opening_cash + customer_receipts + approved_funding_inflows
               - supplier_payments - payroll_payments - operating_payments
               - tax_remittances - interest_and_fee_payments
               - debt_principal_payments - capital_purchases
               - worker_distributions
```

Cash categories must be disjoint; add other receipts/refunds explicitly when
applicable. Reconcile gross receipts and payment fees consistently. Check
usable cash against the required floor at each payment date, not just at the
end of the horizon. Profit can coexist with a cash shortage. Forecast surplus
is not authorization to pay a bonus or make a purchase.

In the current API, `unit_cost` is a constant per unit, `worker_wage_floor` and
`operating_cost` are fixed horizon amounts, and `reserve_floor` is a **new
contribution for that horizon**, not the balance of a reserve account. The
prototype's reported surplus is a contribution-based forecast, not a complete
accounting profit or cash balance. Do not claim the richer ledger above is
already implemented or hide unsupported costs in misleading inputs.

## State, decisions and evidence

The first application controls proposed public prices. Replenishment,
scheduling, employment, lending, transfers and distributions require separate
authority and are outside that action set. Simulating those events grants no
permission to execute them.

Every decision record should connect the policy version, inventory/cost/ledger
snapshots, forecast version, scenario assumptions, engine/model versions,
selected prices, objective-stage results, binding constraints, validation
results, approval, expiry and any later publication identifier.

Demand evidence must distinguish latent demand, fulfilled sales and stockouts.
A time-series forecast alone does not establish response to changing prices.
The simulator must model price response, substitution, delayed deliveries,
perishability and uncertainty separately from the planner's beliefs, with
held-out or deliberately adverse scenarios. Replaying the exact forecasts
used to optimize is only a consistency check.

The current engine rejects a candidate when its supplied demand exceeds stock
in any scenario. It does not clip demand to inventory or optimize lost sales.
The future simulator should track unmet demand explicitly; changing the engine
to choose sales quantities or replenishment is a separate model extension.

## Additional values and operational features

- **Resilience:** stress demand drops, cost increases, supplier delays and
  equipment failures. Report reserve runway, missed obligations and dependence
  on external funding; do not describe one successful solve as self-sufficiency.
- **Worker benefit:** measure whether pay was delivered on time, workloads
  stayed within policy, workers exercised control, and realized surplus was
  allocated as approved. No individual surveillance scores.
- **Customer access:** measure essential-basket price against the approved
  reference, availability, unfulfilled essential demand and complaint outcomes.
- **Environmental care:** report spoilage, disposal and measurable resource use.
  A worker-approved environmental limit is a constraint; an unvalidated impact
  estimate is evidence to improve, not a claim of proven benefit.
- **Supplier fairness:** expose overdue payments and related-party charges.
  Store viability must not be manufactured by silently shifting costs or
  payment risk onto workers or suppliers.
- **Explainability:** show the protected budgets, binding limits, remaining
  target shortfall and why alternatives were rejected. A useful infeasibility
  explanation is not permission to relax the conflicting protection.
- **Reliable execution:** enforce solve deadlines, independent checks, current
  state/policy revalidation, atomic publication, idempotency and checkout
  reconciliation. Worker suspension must prevent an already-running solve from
  publishing after authority has been revoked.

## Acceptance criteria for the simulator

1. Given identical policy, starting state, event stream and random seed, replay
   produces identical decisions and accounts. Store all required versions.
2. Every price satisfies hard policy, and each later objective stage preserves
   earlier stages within declared tolerances. Check small cases by enumeration.
3. Inventory and cash reconcile across purchases, sales, waste, returns and
   period boundaries. Refreshing prices does not duplicate obligations.
4. Simulated outcomes are evaluated against fixed approved prices and the
   current surplus-maximizing engine with the same exogenous shocks, underlying
   customer draws and starting resources. Regenerate fulfilled sales for each
   policy's prices and inventory; do not replay identical realized sales across
   different prices. Report customer and worker outcomes alongside economic results.
5. Stress cases include zero demand, stockouts, rapid cost changes, forecast
   error, negative cash despite positive profit, stale data, solver timeout,
   policy revocation and an unattainable discretionary surplus target.
6. A failed or invalid decision cannot reach publication or payments. The
   initial simulator and shadow mode have no such external action capability.

Numerical targets, essential basket contents, acceptable uncertainty, horizon,
liquidity floors and governance quorum remain decisions for the actual worker
group. Synthetic fixture values may exercise the implementation; they are not
an approved store policy. Build the simulator and reconciled accounts first,
then implement and verify the proposed objective, before a bounded live pilot.
