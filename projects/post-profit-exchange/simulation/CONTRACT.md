<!-- SPDX-License-Identifier: MIT -->
# Exchange research simulator contract

This MIT application models daily store operations around the separately licensed price-optimization tool. It is a bounded, synthetic research simulator. It supplies no governance, tax, debt, payment, or live price-publication capability. The browser uses explicit enumeration of the existing pricing formulation, with the same core input and selection checks; AMPL itself does not execute inside the standalone page. The AMPL backend remains the separately tested server implementation.

## Calling the simulator

Native C++ calls `ph::exchange::run_json(command)`. The native executable reads one JSON command from standard input and writes one JSON response. WebAssembly exports `exchange_run(const char*)`, returning a pointer to a serialized response held until the next call. Its caller must copy the returned text before another call. Calls are synchronous and should run in a browser worker.

```json
{"op":"defaults"}
```

This returns `{schema_version: "exchange.sim.v1", status: "ok", config: {...}}`. The complete authoritative defaults are provided by that response, rather than maintained as a second independent JavaScript preset.

```json
{"op":"simulate","config":{"...":"every required field listed below"}}
```

The ellipsis above illustrates the command wrapper only; it is not a valid configuration. Send the full defaults configuration after applying edits. Unknown/missing fields, duplicate JSON keys, noninteger money/counts, invalid scenario weights and values beyond bounds are errors. No unknown option is ignored and no input is silently clamped. JSON input is limited to 256 KiB and 24 nested containers. Errors are `{schema_version, status: "error", error: {code: "invalid_simulation", message}}`; no partial trajectory is returned.

## Configuration

All money is integer minor currency units (cents in the EUR default). Every period represents one day. Product order is significant for procurement priority. Basis-point factors use 10,000 for 1×, 8,000 for 0.8×, and 12,000 for 1.2×.

| Field | Meaning and bounds |
| --- | --- |
| `periods` | Integer 1–365 days. |
| `seed` | Integer 0–4,294,967,295 for deterministic demand noise. |
| `currency` | Exactly three uppercase letters; no currency conversion occurs. |
| `initial_cash` | 0–100,000,000 cents; opening inventory is separately supplied. |
| `initial_reserve` | Earmarked subset of opening cash, never greater than it. |
| `procurement_budget` | Maximum daily purchase spending, 0–100,000,000 cents. |
| `worker_wages` | Protected wages newly due each day, 1–100,000,000 cents. |
| `operating_cost` | Operating expense newly due each day, 0–100,000,000 cents. |
| `reserve_contribution` | Maximum new daily reserve allocation, 0–100,000,000 cents. |
| `reserve_target` | Desired earmarked cash balance, 0–100,000,000 cents. |
| `demand_noise_bps` | 0–10,000; realized demand factor is drawn from `[10000-noise, 10000+noise]`. |
| `shock` | Exactly `{start_day, end_day, demand_factor_bps, cost_factor_bps}`. Days are inclusive, 1–365 with end ≥ start; `0/0` disables the shock. Factors are integers 0–30,000. A scheduled shock outside the simulated horizon has no effect. |
| `scenarios` | Exactly three `{id, factor_bps, probability}` objects; distinct nonempty IDs, factors 0–30,000, probabilities finite in `(0,1]` and summing to one within `1e-9`. |
| `products` | One to three product objects described below. |

Initial reserve and all global monetary knobs other than positive wages are bounded by 0–100,000,000 cents. These are synthetic assumptions, not approved compensation, affordability or reserve policies.

Each product requires:

| Field | Meaning and bounds |
| --- | --- |
| `sku`, `label` | Nonempty strings of at most 64 bytes without control characters; SKUs are unique. |
| `unit_cost` | Unshocked replacement cost and opening inventory unit valuation, 0–1,000,000 cents. |
| `reference_price` | Initial previous price and reference for the demand curve, 1–1,000,000 cents. |
| `fixed_price` | Literal fixed-baseline public price, independently subject to the same policy checks; it need not be in the optimization grid. |
| `initial_stock`, `target_stock` | Integer whole units, 0–100,000. |
| `base_demand` | Daily demand at reference price before factors, 0–10,000 whole units. |
| `elasticity_bps` | Synthetic linear price-response slope, 0–30,000. This is not an estimated elasticity. |
| `spoilage_bps` | Fraction of remaining stock spoiled after sales, 0–10,000 basis points. |
| `affordability_ceiling` | Maximum public price, 1–1,000,000 cents. |
| `max_change_bps` | Allowed price change from the last successfully selected price, 0–10,000 basis points. It is a per-update limit, not a rolling multiday cap. |
| `candidate_prices` | One to nine distinct integer prices, each 1–1,000,000 cents. |

Labels and scenario IDs share the 64-byte bound. Monetary outputs remain exact integers within JavaScript's exact-integer range under these input bounds. Scenario-weighted expected surplus is floating point.

## Daily sequence and accounting

1. Apply the known shock to replacement unit costs and the assumed demand level. Forecasts know this configured shock; this is not an experiment in forecasting an unexpected regime change.
2. Replenish each product toward target stock, in product order. Spending is capped by the daily procurement budget and cash remaining after existing reserve, existing arrears and the day's wages/operations have been protected. Purchases happen before price selection and therefore can precede an infeasible pricing decision. Deliveries are immediate. Procurement is a deterministic rule, not an optimized variable.
3. Build the existing price model for that day's stocked inventory, replacement costs, three demand scenarios, wages, operations and the unfunded portion of the daily reserve contribution. The optimized path calls `optimize` with the explicit enumeration backend, at most 729 price combinations. The fixed path independently evaluates its literal fixed prices. Candidates with scenario demand exceeding stock are rejected by the shared core; forecasts are never clipped to inventory to force feasibility.
4. If a selection passes, calculate realized demand using the selected prices and sell at most available stock. If it fails, trade nothing, output `selected_price: null`, and do not silently reuse the previous recommendation. On closed days, unserved demand is estimated at the configured reference price and explicitly labeled `reference_price_while_closed`; it is not demand at an unselected price.
5. Spoil the configured fraction of remaining stock, rounded to the nearest whole unit. Record its inventory cost as a loss.
6. Accrue the day's wages and operating expenses once. Pay wage arrears first, then operating arrears, from available cash. Unpaid amounts remain explicit liabilities. Cash never becomes negative, and no debt or external funding is invented. Existing reserve can be released to pay these obligations.
7. If no arrears remain, earmark a new reserve allocation limited by the contribution setting, target shortfall, positive realized economic result, and cash not already reserved. This transfer changes available cash but does not change total cash or economic result.

Opening stock is an explicit resource endowment valued at its unshocked unit cost; it is not silently purchased out of opening cash. Inventory uses FIFO cost lots. Thus the planner's replacement cost can differ from the actual FIFO cost of units sold or spoiled after a cost shock. Both are exposed. Daily economic result is:

```text
revenue - cost_of_goods_sold - waste_cost - newly_due_wages - newly_due_operations
```

Procurement changes cash and inventory; it is not also charged as an immediate economic expense. Paying old arrears settles a liability; it is not a second expense. Reserve earmarking is not an expense. Each period checks exact identities:

```text
closing_cash = opening_cash - procurement + revenue - wages_paid - operations_paid
closing_stock = opening_stock + purchases - sales - spoilage
closing_stock_value = opening_stock_value + purchase_cost - cost_of_goods_sold - waste_cost
cash + inventory_value - wage_arrears - operating_arrears
  = initial_cash + initial_inventory_value + cumulative_economic_result
```

The pricing model covers that day's forecast contribution obligations, not a complete future cash-flow plan. The simulator reveals realized arrears and cash shortages instead of claiming scenario feasibility guarantees liquidity.

## Demand and paired comparison

The synthetic response uses a linear factor `max(0, 10000 - elasticity_bps * (price-reference_price) / reference_price)`, with integer division truncating toward zero. Base demand is multiplied by this price-response factor, the day's shock factor, and either a scenario factor or an independent realized noise factor. Factors are applied with nearest-integer rounding while retaining a 10,000 scale until the final whole-unit rounding. This explicit arithmetic is shared by native and browser builds.

Realized noise comes from an unsigned 32-bit linear congruential generator (`state = state*1664525 + 1013904223` modulo 2³²), mapped into the bounded factor range. It is synthetic pseudorandom noise, not a calibrated probability model or cryptographic generator. Scenario probabilities weight the optimization objective; they do not select the realized noise distribution.

Both paths start with identical cash and stock, receive the same scheduled shock and per-product/per-day noise draws, and apply the same procurement/payment rules. They calculate demand separately at their own prices, so realized sales are not copied from one policy to the other. Their inventories, cash and later purchases may diverge. The simulator uses the same assumed response curve for forecasts and realization with independent noise; it does not yet test structural demand-model misspecification.

The objective remains expected worker-retained surplus within supplied protections. The proposed sequential surplus-target/basket objective is not implemented. No matched passive-owner-return model is present; comparing optimized versus fixed public prices does not establish an advantage from eliminating owner extraction.

## Result fields

Success returns `schema_version`, `status: "ok"`, the unchanged `config`, `engine`, `objective`, `forecast_cost_basis: "current_replacement_cost"`, `realized_cost_basis: "FIFO"`, `optimized`, `fixed`, `comparison`, and human-readable `limitations`. A cost shock can make forecast contribution surplus positive while realized FIFO economic result is negative; these are different measures, not interchangeable predictions. Each path contains `{mode, summary, rows}`. Array rows are in day order and product entries retain configured product order.

`summary` contains cumulative `revenue`, `procurement`, `cost_of_goods_sold`, `waste_cost`, `waste_units`, `sales_units`, `unmet_demand`, `worker_wages_due`, `worker_wages_paid`, `operating_cost_due`, `operating_cost_paid`; plus final `economic_result` (cumulative), `closing_cash`, `reserve_balance`, `available_cash`, `wage_arrears`, `operating_arrears`, `closing_inventory_value`, `initial_inventory_value`, `successful_periods`, `failed_periods`, and `accounting_ok`.

Each row has:

- `day`, pricing `status` and `detail`, `shock_active`, `demand_factor_bps`, `cost_factor_bps`.
- `opening_cash`, `opening_inventory_value`, `procurement`, `revenue`, `cost_of_goods_sold`, `waste_cost`, `waste_units`, `sales_units`, `unmet_demand`.
- `worker_wages_due`, `worker_wages_paid`, `operating_cost_due`, `operating_cost_paid`, `wage_arrears`, `operating_arrears`, `economic_result`, `cumulative_economic_result`.
- `closing_cash`, `available_cash`, `reserve_balance`, `reserve_allocated`, `reserve_released`, `opening_reserve`, `closing_inventory_value`.
- `cash_reconciliation_error`, `equity_reconciliation_error`, `inventory_reconciliation_error`, `valuation_reconciliation_error`, all exactly zero in a successful response.
- `expected_worker_surplus` and `scenario_worker_surplus` (three values in scenario order), or `null` when no pricing recommendation exists. These forecast replacement-cost values are distinct from realized FIFO economic result.
- `products`, with `sku`, `label`, replacement `unit_cost`, `opening_stock`, `opening_inventory_value`, `requested_units`, `purchased_units`, `purchase_cost`, `realized_noise_bps`, `selected_price` (or `null`), `demand_basis`, `demand_reference_price`, `forecast_units` (or `null`), `actual_demand`, `sales_units`, `unmet_demand`, `revenue`, `cost_of_goods_sold`, `waste_units`, `waste_cost`, `closing_stock`, `closing_inventory_value`, and the two product inventory/valuation reconciliation errors.

`comparison.optimized_minus_fixed` reports differences in `economic_result`, `closing_cash`, `worker_wages_paid`, `unmet_demand`, and `waste_units`. Positive is not universally better: positive unmet demand or waste indicates a worse result on that measure. Reporting economic result separately from cash and arrears avoids hiding unpaid obligations behind a cash balance.
