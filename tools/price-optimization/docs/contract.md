# Price optimization integration contract

<!-- SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0 -->

The dashboard integration uses `ph.price.v3`, engine `0.5.0`, model `public-prices.v4`, and objective `operating_balance_tracking` version `2`. Prices respond to a supplied, reconciled funding balance: positive permits decreases or holding, negative permits contribution-preserving increases or holding, and zero requires holding. Separately supplied operating liquidity can fund forecast shortfalls without becoming earned income or changing that direction. The engine does not maintain the store ledger, authenticate policy, estimate demand or publish prices. A policy identifier is a caller-supplied audit label, not authenticated worker consent.

## Request

All fields below are required. Unknown fields are rejected at every object level. Currency amounts are integer minor units in one three-letter uppercase currency, quantities are whole units, and timestamps are Unix seconds. No conversion from decimal money, booleans, strings, or out-of-range integers is performed.

| Field | Meaning |
| --- | --- |
| `schema_version` | Exactly `ph.price.v3`. |
| `request_id`, `currency` | Caller request identifier and the single currency. |
| `as_of`, `valid_until`, `max_input_age_seconds` | Input timestamp, exclusive validity boundary, and exclusive maximum age. The engine checks freshness again after solving. |
| `policy.id`, `policy.version` | Nonempty policy label and positive integer version. The internal reference is `id@version`, at most 256 bytes without control characters. |
| `policy.worker_wage_floor`, `policy.reserve_contribution` | Protected fixed amounts for this decision horizon. |
| `policy.products[]` | Exactly one `{sku, affordability_ceiling, max_change_basis_points}` per catalog SKU. A basis point is one hundredth of a percent. |
| `costs.operating_cost` | Fixed operating cost, excluding wages and per-unit cost. |
| `costs.unit_costs[]` | Exactly one `{sku, amount}` per catalog SKU. |
| `catalog[]` | `{sku, previous_price, inventory, candidate_prices}`; candidate prices are distinct positive integers and must include `previous_price` as the hold candidate. |
| `demand.source` | Explicit provenance label for externally supplied forecasts. |
| `demand.scenarios[]` | `{id, probability}`; IDs are distinct, probabilities positive, and their sum is within `1e-9` of one. |
| `demand.forecasts[]` | Exactly one `{sku, price, units}` for every catalog candidate. `units` follows the order of `demand.scenarios`. |
| `feedback.funding_balance` | Signed reconciled operating funding balance, or an explicitly amortized control adjustment derived from it. Positive means ahead of required funding; negative means a shortfall. |
| `feedback.coverage_credit` | Nonnegative, independently established cash-backed earned funding available to cover this horizon. Must be zero when `funding_balance <= 0`. It supports coverage and does not replace the signed balance in the objective. |
| `feedback.liquidity_buffer` | Nonnegative additional spendable operating cash, disjoint from `coverage_credit`. Available at any funding-balance sign; it may originate in initial assets. It funds continuity without becoming earned balance, discount entitlement, or objective income. |
| `objective` | Exactly `{ "id": "operating_balance_tracking", "version": 2 }`. |

Policy, unit costs, and forecasts join by SKU and candidate price, independently of their input array order. Missing, extra, and duplicate matches are errors. Catalog order determines result product order; candidate indices are zero-based within that catalog product's `candidate_prices` array. Forecast scenario order is significant and preserved.

JSON text is limited to 8 MiB and 32 nested containers. Duplicate object keys, including escaped equivalents, malformed JSON, nonfinite/overflowing numbers, and trailing JSON values are rejected. Engine bounds also apply: at most 256 products, 64 candidates per product and 32 scenarios; individual prices, costs and fixed obligations at most 1,000,000,000 minor units; quantities at most 1,000,000 units; and bounded aggregate arithmetic. `funding_balance` is bounded by ±2^50; each of `coverage_credit` and `liquidity_buffer` is bounded by 0 through 2^50. The caller must establish their accounting basis and disjoint spendable cash amounts. Do not count the same cash in both fields, count inventory as cash, or include cash already committed elsewhere. Type and range validation cannot prove that funds exist. Reference labels are not AMPL instructions.

Schema `ph.price.v1` and `ph.price.v2` requests are not silently migrated. The JSON shape remains `ph.price.v3`, but objective version `1` is rejected: callers must explicitly request objective version `2`, supply all three feedback fields, and provide forecasts for every hold candidate. The engine has no discretionary surplus target or static basket-price objective.

`synthetic_request_json(now)` produces the bread-and-beans evaluation fixture with current timestamps. Its forecast source explicitly identifies synthetic data. It supplies no empirical demand, operational authorization, or approved store policy.

The complete synthetic request below illustrates every required field. The fixed timestamps are illustrative; generate fresh timestamps with `price_backend --example` before executing. That command wraps this request in the supervisor's `solve` command, described in [the backend protocol](backend.md).

```json
{
  "schema_version": "ph.price.v3",
  "request_id": "synthetic-cooperative-001",
  "currency": "EUR",
  "as_of": 1800000000,
  "valid_until": 1800000300,
  "max_input_age_seconds": 120,
  "feedback": {"funding_balance": 0, "coverage_credit": 0, "liquidity_buffer": 0},
  "policy": {
    "id": "synthetic worker assembly resolution 001",
    "version": 1,
    "worker_wage_floor": 800,
    "reserve_contribution": 100,
    "products": [
      {"sku": "bread", "affordability_ceiling": 220, "max_change_basis_points": 1000},
      {"sku": "beans", "affordability_ceiling": 330, "max_change_basis_points": 1000}
    ]
  },
  "costs": {
    "operating_cost": 100,
    "unit_costs": [
      {"sku": "bread", "amount": 100},
      {"sku": "beans", "amount": 150}
    ]
  },
  "catalog": [
    {"sku": "bread", "previous_price": 200, "inventory": 20, "candidate_prices": [180, 200, 220, 240]},
    {"sku": "beans", "previous_price": 300, "inventory": 15, "candidate_prices": [270, 300, 330]}
  ],
  "demand": {
    "source": "synthetic fixture; no empirical elasticity claim",
    "scenarios": [
      {"id": "usual", "probability": 0.6},
      {"id": "low-demand", "probability": 0.4}
    ],
    "forecasts": [
      {"sku": "bread", "price": 180, "units": [20, 14]},
      {"sku": "bread", "price": 200, "units": [16, 12]},
      {"sku": "bread", "price": 220, "units": [12, 8]},
      {"sku": "bread", "price": 240, "units": [20, 18]},
      {"sku": "beans", "price": 270, "units": [13, 9]},
      {"sku": "beans", "price": 300, "units": [12, 8]},
      {"sku": "beans", "price": 330, "units": [8, 5]}
    ]
  },
  "objective": {"id": "operating_balance_tracking", "version": 2}
}
```

## Result

Every terminal result has `event: "result"`, `schema_version`, `request_id`, `status`, `engine_version`, `model_version`, `model_sha256`, and `objective`. The model hash identifies the embedded AMPL model used by the configured build; manually compiling without the build definition yields `unavailable`. `policy: {id, version}` and `input_snapshot` preserve the validated request when available. Failures before successful parsing use `null` for these two fields. A supervisor may retain the already-validated policy and snapshot when it reports interruption.

Statuses are `recommended`, `invalid_input`, `infeasible`, `unavailable`, `solver_failed`, `rejected_solution`, `timed_out`, and `cancelled`. A deadline failure has status `timed_out` and error code `deadline_exceeded`. Only `recommended` contains `recommendation`. Other statuses contain `error: {code, message}` and never a fallback or partial price recommendation. Callers should branch on the status/code and display the message as text. Messages are diagnostic, not a stable machine grammar. AMPL infeasibility is not presented as a proven explanation of which business policy caused it.

For success, C++ independently checks the selected indices against the retained, freshly validated snapshot and rejects inconsistent prices, objective, identifiers, timestamps, or scenario amounts. Results contain:

- `recommendation.currency`, `monetary_unit`, `checked_at`, and exclusive `valid_until`. Validity ends at the earlier of the caller's validity boundary and maximum input age. The optimizer never publishes a price.
- `recommendation.products[]`: `sku`, `candidate_index`, `previous_price`, `selected_price`, `unit_cost`; per-scenario supplied `forecast_units` explicitly marked `forecast_origin: "supplied_input"`; and derived integer `revenue`, `unit_cost_total`, and `contribution`.
- `recommendation.scenarios[]`: scenario ID and probability, total revenue, unit cost total, contribution, worker wages, operating cost, reserve contribution, remaining `worker_surplus`, and `funding_balance` equal to input funding balance plus that surplus.
- `recommendation.expected`: probability-weighted revenue, costs, contribution and worker surplus, plus the minimized `absolute_funding_balance`. Expected values use floating point because scenario probabilities are floating point; all hard constraints and individual scenario monetary amounts use exact integers. Weights are used as supplied, without silent normalization. Expected worker surplus remains a financial diagnostic, not the objective.
- `recommendation.feedback`: supplied `funding_balance`, `coverage_credit`, `liquidity_buffer`, and `direction` (`down_or_hold`, `up_or_hold`, or `hold`).
- `local_candidate_exclusions`: candidate index, price, SKU and reason codes for candidates that violate `affordability_ceiling`, `price_change_cap`, `inventory` (with scenario ID), `feedback_direction`, `increase_reduces_contribution` (with scenario ID), or `affordable_alternative` (with `alternative_candidate_index` and `alternative_price`). The last reason identifies a locally legal cheaper candidate satisfying the comparison below. Candidates omitted from this list may be feasible but suboptimal, or may fail portfolio coverage in combination. The explicit scope is `local_candidate_constraints_only` and `global_infeasibility_explanation` is `false`. This is not an exhaustive explanation of optimization choices.

For supplied funding balance `B`, earned coverage credit `C`, disjoint additional operating liquidity `L`, and scenario surplus `m_s` after unit costs, protected wages, operations and this horizon's reserve requirement, the formulation is:

```text
minimize sum_s probability_s * abs(B + m_s)
subject to m_s + C + L >= 0 in every scenario
and the local price, inventory, direction and affordable-alternative rules
```

A candidate is excluded if another candidate for the **same SKU** is strictly cheaper, otherwise locally legal, and has both at least as many forecast units and at least as much contribution `(price - unit_cost) * units` in **every supplied scenario**. The alternative must pass affordability, change-cap, inventory, feedback-direction and safe-increase checks. Replacing the higher price therefore cannot worsen modeled portfolio coverage. The rule applies at every balance sign without relaxing price direction. It does not require selecting the cheapest feasible basket or resolve tradeoffs across products or scenarios.

The balance objective applies among the remaining candidates. Its score may be larger than with objective version 1: a more affordable offer can generate more surplus and must no longer be rejected for that reason alone. For a supplied one-scenario example with unit cost 100 cents, fixed requirements 400 and `B = 1000`, price 200 forecasting 10 purchases gives surplus 600 and score 1600; price 160 forecasting 30 gives surplus 1400 and score 2400. When both pass the other rules, version 2 excludes price 200 and selects 160. This is a deterministic forecast fixture, not observed consumer behavior.

The reported score is the expected absolute balance, not the absolute value of expected balance: positive and negative scenario deviations cannot cancel. Positive `B` restricts every price to at most its previous price; negative `B` restricts every price to at least its previous price; zero `B` requires the previous prices. When `B < 0`, an increase is also forbidden if `(candidate_price - unit_cost) * forecast_units` is lower than the hold candidate's contribution in any scenario. A price rise therefore must preserve forecast contribution in every supplied scenario, but the model does not guarantee recovery of a realized shortfall.

Liquidity expands only explicit financial coverage. For example, `B = 0`, `C = 0`, `L = 1000` can fund a held-price horizon with scenario surplus `-1000`; the projected funding balance remains `-1000`, not zero. With `L = 999` that same horizon is infeasible. Existing cash does not waive affordability, inventory, price direction, safe-increase, wage, or reserve requirements. The application decides whether to continue under an assurance warning, revise an authorized reserve schedule, or end after actual cash exhaustion; the optimizer does not silently relax constraints or decide store closure.

Worker pay is a fixed protected input, never a variable the optimizer reduces. Inventory constrains supplied forecast units, without truncating demand. Reserve contribution is a newly scheduled funding requirement for this horizon, not a reserve account balance or an additional per-unit expense. The application must reconcile it once. In the exchange simulator, the reserve target increase above endowed reserve is scheduled only once; releasing or re-earmarking cash does not create a second requirement or earned funding. Its recovery horizon amortizes raw realized history into `B`, while `C` separately limits support to earned liquid funding. These application calculations are described in the [simulator contract](../../../projects/post-profit-exchange/simulation/CONTRACT.md).

## Constraint explanations

The following reported constraints include integer `slack` and `binding: (slack == 0)`. Feedback direction and contribution-preserving increases are independently checked and exposed through the feedback direction and candidate exclusions; they do not have additional selected-product slack fields.

| Location | Exact slack definition |
| --- | --- |
| `products[i].constraints.affordability` | `affordability_ceiling - selected_price`, in minor currency units. |
| `products[i].constraints.price_change` | `previous_price * max_change_basis_points - abs(selected_price - previous_price) * 10000`; unit is `minor_currency_units_times_basis_points`. This avoids rounding a percentage before checking it. |
| `products[i].scenarios[s].inventory` | `inventory - forecast_units`, in whole units. |
| `scenarios[s].coverage` | `worker_surplus + feedback.coverage_credit + feedback.liquidity_buffer`, in minor currency units. A negative scenario surplus can be covered by the two caller-established disjoint cash sources. |

A binding flag means a selected solution lies on that constraint's boundary. It is not a dual price, sensitivity estimate, or proof that the constraint determined the optimum. A cheaper alternative with equal contribution and no fewer purchases excludes the higher price regardless of candidate order. For remaining incomparable choices, enumeration retains original candidate-index order on equal scores; AMPL supplies no general deterministic tie-breaking guarantee. The snapshot and version/hash metadata support replay and audit; they do not imply that another solver build will choose identical prices when multiple optima exist.

For the synthetic selection `[1, 1]`, bread is 200 cents and beans 300 cents. Zero input funding balance requires these held prices. Usual-scenario revenue is 6,800, unit costs 3,400, protected wages 800, operating cost 100, reserve 100, leaving 2,400. Low-demand surplus is 1,400. At probabilities 0.6 and 0.4, expected surplus and expected absolute funding balance both equal 2,000 in this example. They are different measures in general. Bread's 240-cent candidate violates affordability, price-change and zero-balance hold rules. None of these supplied forecasts is an observed store outcome.

## Scope of the boundary

This contract is an optimizer integration boundary for a synthetic store dashboard. The deadline/cancellation supervisor owns process execution and operational events; those controls do not become AMPL policy variables. Policy editing authority, customer-facing publication, worker governance, and broader risk management remain outside the optimizer. Live prices require a separate application and appropriate AMPL entitlement.
