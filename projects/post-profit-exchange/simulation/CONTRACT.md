<!-- SPDX-License-Identifier: MIT -->
# Exchange research simulator contract

`exchange.sim.v3` models daily store operations around the separately licensed
pricing core. The application and its forecast/consumer modules are MIT. The
browser uses bounded C++ enumeration in WebAssembly; AMPL is a separate native
backend. The objective remains `operating_balance_tracking`, with salaries fixed
as inputs. This version adds historical forecasting, synthetic consumers,
liquidity-supported continuity, dynamic reserve stress and terminal events.
The EWMA core is the standalone MIT `tools/exponential-smoothing` library. This application
retains stockout classification, price-response assumptions and JSON adapters;
the Temporal Fusion Transformer remains a separate, unconnected tool.

## Commands

`ph::exchange::run_json(command)` and the native stdin interface accept one JSON
command. WebAssembly exports `exchange_run(const char*)`; copy its returned text
before calling again because the same response storage is reused. Calls are
synchronous; the browser executes them in a worker.

```json
{"op":"defaults"}
```

This returns `{schema_version:"exchange.sim.v3", status:"ok", config:{...}}`.
The default configuration is the build-time simulation section of the central
[example configuration](../configs/exchange.cfg), embedded by the native/WASM
build. It is a research example, not an approved operating policy.
The native example configuration references an empty history file by default.
Synthetic example history remains an explicit opt-in fixture, never a source of
observed store evidence. See the [record provenance note](../records/README.md).

```text
{"op":"simulate", "config":<complete config>, "history":<history wrapper>}
```

The placeholders above describe the shape, not literal valid JSON. `history` is
optional and defaults to an empty `exchange.history.v1` wrapper. No other fields
are optional. Unknown/missing fields, duplicate keys, noninteger money/counts,
invalid probabilities and out-of-range values are rejected. Input is limited to
8 MiB and 24 nested containers. A validation error returns `{schema_version,
status:"error", error:{code:"invalid_simulation",message}}`, without a trajectory.
A simulated terminal outcome is instead a successful response with an explicit
per-policy terminal status.

Version 2 configurations require explicit migration: `demand_noise_bps` is removed,
scenario factors become sigma offsets, and consumers, forecast, assurance and
combination-limit objects are required. Unsupported fields are never ignored.
The native `.cfg` runner and record schemas are documented in [FILES.md](../docs/FILES.md).

## Configuration

Money uses integer minor units in one currency; EUR defaults mean cents. Product
quantities are whole units, and each period is one day. Product order controls
procurement priority and consumer budget allocation. Basis-point probabilities
use 10,000 for one; 30,000 sigma-multiplier basis points means three sigma.

| Field | Meaning and bounds |
| --- | --- |
| `periods` | 1–365 days. |
| `seed` | Unsigned 32-bit seed, 0–4,294,967,295. |
| `currency` | Exactly three uppercase letters. |
| `initial_cash` | Opening money, 0–100,000,000; opening inventory is a separate endowment. |
| `initial_reserve` | Earmarked subset of opening cash, never greater than cash. |
| `procurement_budget` | Daily purchase cap, 0–100,000,000. |
| `worker_wages` | Fixed daily wages newly due, 1–100,000,000. |
| `operating_cost` | Fixed daily operating expense newly due, 0–100,000,000. |
| `reserve_contribution` | Daily cap on new target funding and separately on actual cash earmarking, 0–100,000,000. |
| `reserve_target` | Configured minimum target, 0–100,000,000; forecast stress can require more. |
| `feedback_recovery_days` | 1–30, default 3; signed history adjustment uses this response horizon. |
| `max_price_combinations` | Exact enumeration grid limit, 1–200,000. A larger grid is a model error, not an approximate solve. |
| `consumers` | All seven fields in the [consumer contract](../docs/MODELS.md#interfaces-and-configuration). |
| `forecast` | All six EWMA/stress fields in the [forecast contract](../docs/MODELS.md#interfaces-and-configuration). |
| `assurance` | `{provider:"post_profit_continuity_assurance", trigger_buffer_days:1..365}`. |
| `shock` | `{start_day,end_day,demand_factor_bps,cost_factor_bps}`; 0/0 disables, otherwise inclusive days 1–365 with end ≥ start; factors 0–30,000. |
| `scenarios` | Exactly three `{id,sigma_offset_bps,probability}` objects; distinct IDs, offsets −10,000 through 10,000, finite positive probabilities at most one, total within 1e-9 of one. |
| `products` | 1–12 product objects below. |

Each product requires:

| Field | Meaning and bounds |
| --- | --- |
| `sku`, `label` | 1–64 bytes without controls; SKUs unique. |
| `unit_cost` | Unshocked replacement cost and opening-stock valuation, 0–1,000,000. |
| `reference_price` | Initial balancing-policy price and consumer-prior reference, 1–1,000,000. |
| `fixed_price` | Initial and continuing fixed-policy price, 1–1,000,000; need not appear in the candidate grid. |
| `initial_stock`, `target_stock` | Whole units, 0–100,000. |
| `base_demand` | Declared cold-start forecast level and reference consumer-demand prior, 0–10,000. |
| `elasticity_bps` | Declared logit price-response slope, 0–30,000; not an empirically estimated elasticity. |
| `spoilage_bps` | Fraction of remaining stock spoiled daily, 0–10,000. |
| `affordability_ceiling` | Maximum posted price, 1–1,000,000; reference and fixed prices must respect it. |
| `max_change_bps` | Per-update limit from previous price, 0–10,000; no rolling-window policy is inferred. |
| `candidate_prices` | 1–16 distinct positive integer prices at most 1,000,000, including reference price. Later held prices come from this grid. |

Scenario IDs use the same string bound. Inputs are research assumptions, not
empirical evidence or authenticated worker approval. Every candidate count can
be individually valid while their Cartesian product exceeds the configured
combination limit.

## Historical observations

```json
{"schema_version":"exchange.history.v1","observations":[
  {"day":-2,"sku":"bread","price":200,"sales_units":18,"stockout":false},
  {"day":-1,"sku":"bread","price":200,"sales_units":20,"stockout":false}
]}
```

The simulator accepts at most 12,000 observations. Days range from −1,000,000
through −1 and strictly increase for each known SKU; interleaving SKUs is allowed.
Price is 1–1,000,000, sales 0–1,000,000 and `stockout` must be boolean. The history
is applied to a separate forecaster for each policy before day one, using the
same declared price-response normalization. Stockout observations are counted
but excluded from learning/error metrics. History affects forecasts only; it
does not add cash, inventory or earned funding. Empty history retains explicit
cold-start priors. The repository's sample history is synthetic.

## Daily ordering and accounting

1. Observe current replacement costs. Forecast from past uncensored sales and
   calculate the dynamic reserve target. The demand shock affects future realized
   arrivals; its upcoming values are not supplied to the forecaster.
2. Schedule new reserve funding, then replenish toward target stock in product
   order. Protect existing reserves, arrears and today's wages/operations; enforce
   cash and procurement-budget limits. Deliveries are immediate.
3. Construct candidate forecasts from EWMA mean and configured sigma offsets.
   Cap demand by physical visitor/unit capacity, retain that demand separately,
   and supply saleable units capped to current stock to the pricing engine.
   Optimize balancing prices or independently evaluate the literal fixed price.
4. A certified recommendation selects public prices. Financial infeasibility uses
   the declared continuity rule: retain current prices and mark uncertified
   coverage. Technical model error rolls back that day's purchases and scheduled
   requirement, records a terminal error, and produces no daily trading row.
5. Generate paired synthetic consumer events; fulfill affordable purchases up to
   stock. Spoil remaining units with nearest-unit rounding, remove FIFO cost lots
   and accrue fixed obligations exactly once.
6. Pay wage arrears first, then operating arrears. Release earmarked reserve if
   total cash falls below it. Outstanding amounts remain liabilities; no debt or
   external income is invented.
7. Score forecasts before updating from uncensored sales. Update actual funding
   history, re-earmark cash if no arrears remain, reconcile the ledgers and emit
   assurance/terminal events where required.

Opening stock is an endowment, not another purchase from opening cash. Purchases
add inventory; cost is expensed only on sale or spoilage. An old arrear payment
settles a liability without creating another expense. Reserve transfers do not
change total cash or economic result.

```text
actual_contribution = revenue - FIFO_cost_of_goods_sold - FIFO_waste_cost
economic_result = actual_contribution - wages_due - operating_cost_due
actual_required = wages_due + operating_cost_due + new_reserve_requirement
actual_funding_result = actual_contribution - actual_required
closing_cash = opening_cash - procurement + revenue - wages_paid - operations_paid
closing_stock = opening_stock + purchased_units - sales_units - waste_units
closing_inventory_value = opening_inventory_value + purchase_cost
                          - cost_of_goods_sold - waste_cost
cash + inventory_value - wage_arrears - operating_arrears
    = initial_cash + initial_inventory_value + cumulative_economic_result
```

Forecast contribution uses replacement costs; realized contribution uses FIFO
acquisition costs. A shock can make their signs differ independently of demand
error. Returned FIFO lots retain unit cost and acquisition day. Neither cash
nor forecast surplus is interchangeable with realized economic result.

## Funding feedback and reserves

Raw funding history `B` starts at zero. Add actual FIFO economic result and
subtract only newly scheduled reserve funding. The engine's signed adjustment
`b` is `B / feedback_recovery_days`, rounded to the nearest cent with nonzero sign
preserved at a minimum magnitude of one cent. Endowments never enter `B`.

For scenario surplus `m_s` after fixed obligations and today's reserve requirement,
the objective is `sum(probability_s * abs(b + m_s))`. Positive `b` permits lower
or held prices; negative permits higher or held prices; zero holds. Increases
must preserve the product's contribution relative to holding in every supplied
saleable-unit scenario. Wages are fixed. Limits can block useful price recovery.

Let `liquid = max(0,cash_after_procurement-wage_arrears-operating_arrears)`.
Earned credit `C` is zero unless `b > 0`; then it is
`min(max(B,0),max(liquid-reserve,0))`. Other liquidity is `L = liquid-C`.
Protected scenario coverage is `m_s + C + L >= 0`. Both can support a temporary
forecast deficit, but neither replaces signed history in the objective or turns
opening cash into earned funding. This is the `ph.price.v3`/`public-prices.v3`
engine `0.4.0` interface, with objective `operating_balance_tracking` version 1.

The dynamic reserve target combines the configured minimum with `H` days of
expected fixed-cost shortfall and a `z`-sigma common adverse contribution shock.
The exact formula, mean/sigma caps and assumptions are in [MODELS.md](../docs/MODELS.md#uncertainty-and-reserve-use).
This is a conditional stress heuristic, not an absolute worst case or a guarantee
of sufficient cash. New funding is scheduled as:

```text
requirement = min(reserve_contribution,
    max(0, dynamic_target-initial_reserve-cumulative_scheduled_requirement))
B_next = B + actual_FIFO_economic_result - requirement
```

A higher target can create new requirements. A lower target does not refund
previous requirements. Releasing/rebuilding the cash earmark never restarts the
schedule or duplicates a loss. Actual allocation is limited by daily contribution,
remaining cash target and cash not already reserved, with no outstanding arrears.

## Forecast and consumer boundaries

[MODELS.md](../docs/MODELS.md) specifies all update equations and diagnostics.
Each SKU predicts from its prior plus earlier eligible sales; sellouts, closed
periods and future consumer demand never train the current forecast. Sigma is
smoothed RMS one-step error with a configured prior/floor. Warmup is a visible
status, not evidence that a model has become accurate. Prediction bands and
scenario probabilities are configured assumptions, not calibrated tail coverage.

Consumers independently draw visit/skip, budget, per-SKU need and binary-logit
purchase/no-purchase choices. Draws are keyed by seed/day/customer/SKU/event.
Both policies share underlying draws but spend their own budgets at their own
prices and inventory. SKU order affects budget allocation. No persistent
households, substitution or measured real-customer coefficients are modeled.
The forecaster's declared price response does not learn cross-product budget
competition, so model mismatch remains possible.

## Assurance and termination

Events use `exchange.assurance.v1` with `id`, `policy`, `type`, `provider`, `day`,
`reason`, `currency`, `required_support`, `cash`, `reserve`, `inventory_value`,
`funding_balance`, `wage_arrears`, `operating_arrears`, `forecast` and
`settlement_status:"unfunded_request"`. The provider identifier is
`post_profit_continuity_assurance`; it is a proposed integration contract, not
an active insurer or funded service. Recording events sends no message or money.

An event's `id` is currently `policy-day-type`, unique within its simulated path,
not across runs. Persisted identity is `(run_id, policy, event.id)` using the
`exchange.assurance-log.v1` envelope. Callers aggregating records must choose
distinct run IDs in their namespace; the file runner does not enforce that
across directories. The direct simulator has no run ID and returns local events.
No receiver or idempotent payout exists. The proposed
[assurance contract](../../post-profit-assurance/CONTRACT.md) preserves this boundary.

`assurance_requested` is emitted for continuity coverage failure or a fixed-cost
cash-buffer shortfall. Requested support is the nonnegative gap between cash and
`fixed_cost * trigger_buffer_days + arrears`; it can be zero when a request is
raised because modeled coverage failed. `insolvency_declared` accompanies zero
opening cash, zero cash after fixed obligations, or unpaid fixed obligations.
That path ends immediately, retaining its actual rows and inventory. The other
policy can continue. Model failure produces `terminal_status:"model_error"`
with an assurance event and no invented trading result for that day. This
insolvency rule is a simulation condition, not a legal determination.

## Results and replay

Success contains `schema_version`, `status:"ok"`, complete `config` and `history`,
`engine`, `objective`, `forecast_cost_basis:"current_replacement_cost"`,
`realized_cost_basis:"FIFO"`, `optimized`, `fixed`, `comparison` and `limitations`.
Each path has `{mode, summary, rows, events}`. Rows are chronological and products
retain configured order. `optimized` is the API key for balancing feedback.

Daily rows retain the complete money and quantity ledger; raw funding before/
after, adjustment, earned credit and liquidity buffer; scheduled/actual reserves;
expected scenario surplus, scenario funding balances and expected absolute score;
consumers, forecast evidence, FIFO lots, price changes and reconciliation errors.
`status` is `recommended` or `continuity`, with separate `optimization_status`.
A continuity row has real trades but null optimizer scores/forecast selection;
it is not a fallback solver recommendation.

Per-product records include purchase flows, current replacement cost, acquisition
lots, previous/selected prices, forecast mean/sigma/bands and candidate scenarios,
pre-update forecast and observation diagnostic, actual affordable demand, sales,
budget rejections, stock-lost units, spoilage, FIFO cost, revenue and closing value.
`requested_units` in this product ledger is procurement requested; consumer
`requested_units` is affordable shopping demand. Do not conflate the two.

Summary records cumulative revenue, procurement, costs, wages and operations due/
paid, sales, waste, unmet demand and visitor gates; final cash, reserve, arrears,
stock value, funding history and scheduled reserve requirements. It also reports
per-SKU summaries and forecast diagnostics, `terminal_status` (`completed`,
`insolvent`, `model_error`), `terminal_day`, and assurance-request count.
`successful_periods` counts actual recommended plus continuity trading days;
`optimized_periods` and `continuity_periods` separate them. Technical failure
has a terminal event, not a fabricated daily row or a synthetic trade count.

`comparison.optimized_minus_fixed` reports economic result, closing cash, wages
paid, unmet demand, waste and funding differences. `equal_observed_horizons`
flags matching row counts. Positive is not universally better, and unequal
terminal horizons must accompany any comparison. Forecast, consumer and
accounting records support deterministic replay under the same configuration,
history and build; they do not establish economic viability or owner-removal
benefit. See the [verification record](../../../docs/exchange-simulation-verification.md)
for checks actually performed rather than inferring browser tests from C++ tests.
