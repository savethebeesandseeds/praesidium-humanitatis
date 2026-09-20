# Price optimization integration contract

<!-- SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0 -->

The dashboard integration uses `ph.price.v1`, model `public-prices.v1`, and objective `expected_worker_surplus` version `1`. It retains the tested pricing model. It does not add store governance, authorization, demand estimation, price publication, or a different objective. A policy identifier is a caller-supplied audit label, not authenticated worker consent.

## Request

All fields below are required. Unknown fields are rejected at every object level. Currency amounts are integer minor units in one three-letter uppercase currency, quantities are whole units, and timestamps are Unix seconds. No conversion from decimal money, booleans, strings, or out-of-range integers is performed.

| Field | Meaning |
| --- | --- |
| `schema_version` | Exactly `ph.price.v1`. |
| `request_id`, `currency` | Caller request identifier and the single currency. |
| `as_of`, `valid_until`, `max_input_age_seconds` | Input timestamp, exclusive validity boundary, and exclusive maximum age. The engine checks freshness again after solving. |
| `policy.id`, `policy.version` | Nonempty policy label and positive integer version. The internal reference is `id@version`, at most 256 bytes without control characters. |
| `policy.worker_wage_floor`, `policy.reserve_contribution` | Protected fixed amounts for this decision horizon. |
| `policy.products[]` | Exactly one `{sku, affordability_ceiling, max_change_basis_points}` per catalog SKU. A basis point is one hundredth of a percent. |
| `costs.operating_cost` | Fixed operating cost, excluding wages and per-unit cost. |
| `costs.unit_costs[]` | Exactly one `{sku, amount}` per catalog SKU. |
| `catalog[]` | `{sku, previous_price, inventory, candidate_prices}`; candidate prices are distinct positive integers. |
| `demand.source` | Explicit provenance label for externally supplied forecasts. |
| `demand.scenarios[]` | `{id, probability}`; IDs are distinct, probabilities positive, and their sum is within `1e-9` of one. |
| `demand.forecasts[]` | Exactly one `{sku, price, units}` for every catalog candidate. `units` follows the order of `demand.scenarios`. |
| `objective` | Exactly `{ "id": "expected_worker_surplus", "version": 1 }`. |

Policy, unit costs, and forecasts join by SKU and candidate price, independently of their input array order. Missing, extra, and duplicate matches are errors. Catalog order determines result product order; candidate indices are zero-based within that catalog product's `candidate_prices` array. Forecast scenario order is significant and preserved.

JSON text is limited to 8 MiB and 32 nested containers. Duplicate object keys, including escaped equivalents, malformed JSON, nonfinite/overflowing numbers, and trailing JSON values are rejected. Engine bounds also apply: at most 256 products, 64 candidates per product and 32 scenarios; individual money values at most 1,000,000,000 minor units; quantities at most 1,000,000 units; and bounded aggregate arithmetic. Reference labels are not AMPL instructions.

`synthetic_request_json(now)` produces the bread-and-beans evaluation fixture with current timestamps. Its forecast source explicitly identifies synthetic data. It supplies no empirical demand, operational authorization, or approved store policy.

The complete synthetic request below illustrates every required field. The fixed timestamps are illustrative; generate fresh timestamps with `price_backend --example` before executing. That command wraps this request in the supervisor's `solve` command, described in [the backend protocol](backend.md).

```json
{
  "schema_version": "ph.price.v1",
  "request_id": "synthetic-cooperative-001",
  "currency": "EUR",
  "as_of": 1800000000,
  "valid_until": 1800000300,
  "max_input_age_seconds": 120,
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
  "objective": {"id": "expected_worker_surplus", "version": 1}
}
```

## Result

Every terminal result has `event: "result"`, `schema_version`, `request_id`, `status`, `engine_version`, `model_version`, `model_sha256`, and `objective`. The model hash identifies the embedded AMPL model used by the configured build; manually compiling without the build definition yields `unavailable`. `policy: {id, version}` and `input_snapshot` preserve the validated request when available. Failures before successful parsing use `null` for these two fields. A supervisor may retain the already-validated policy and snapshot when it reports interruption.

Statuses are `recommended`, `invalid_input`, `infeasible`, `unavailable`, `solver_failed`, `rejected_solution`, `timed_out`, and `cancelled`. A deadline failure has status `timed_out` and error code `deadline_exceeded`. Only `recommended` contains `recommendation`. Other statuses contain `error: {code, message}` and never a fallback or partial price recommendation. Callers should branch on the status/code and display the message as text. Messages are diagnostic, not a stable machine grammar. AMPL infeasibility is not presented as a proven explanation of which business policy caused it.

For success, C++ independently checks the selected indices against the retained, freshly validated snapshot and rejects inconsistent prices, objective, identifiers, timestamps, or scenario amounts. Results contain:

- `recommendation.currency`, `monetary_unit`, `checked_at`, and exclusive `valid_until`. Validity ends at the earlier of the caller's validity boundary and maximum input age. The optimizer never publishes a price.
- `recommendation.products[]`: `sku`, `candidate_index`, `previous_price`, `selected_price`, `unit_cost`; per-scenario supplied `forecast_units` explicitly marked `forecast_origin: "supplied_input"`; and derived integer `revenue`, `unit_cost_total`, and `contribution`.
- `recommendation.scenarios[]`: scenario ID and probability, total revenue, unit cost total, contribution, worker wages, operating cost, reserve contribution, and remaining worker surplus.
- `recommendation.expected`: probability-weighted revenue, costs, contribution, and worker surplus. Expected values use floating point because scenario probabilities are floating point; all hard constraints and individual scenario monetary amounts use exact integers. Weights are used as supplied, without silent normalization.
- `local_candidate_exclusions`: candidate index, price, SKU and reason codes for candidates that independently violate `affordability_ceiling`, `price_change_cap`, or `inventory` (with scenario ID). Candidates omitted from this list may be feasible but suboptimal, or may fail portfolio coverage in combination. The explicit scope is `local_candidate_constraints_only` and `global_infeasibility_explanation` is `false`. This is not an exhaustive explanation of optimization choices.

The objective equals the expected worker surplus after unit costs, protected wages, operating costs, and reserve contribution. Worker pay is an input floor, never a variable the optimizer reduces. Inventory constrains supplied forecast units, without truncating demand. Reserve contribution is the model's protected cash allocation, not an additional per-unit expense.

## Constraint explanations

Every selected-product constraint includes integer `slack` and `binding: (slack == 0)`:

| Location | Exact slack definition |
| --- | --- |
| `products[i].constraints.affordability` | `affordability_ceiling - selected_price`, in minor currency units. |
| `products[i].constraints.price_change` | `previous_price * max_change_basis_points - abs(selected_price - previous_price) * 10000`; unit is `minor_currency_units_times_basis_points`. This avoids rounding a percentage before checking it. |
| `products[i].scenarios[s].inventory` | `inventory - forecast_units`, in whole units. |
| `scenarios[s].coverage` | Revenue minus unit costs, worker wages, operating cost and reserve contribution, in minor currency units. This equals scenario worker surplus. |

A binding flag means a selected solution lies on that constraint's boundary. It is not a dual price, sensitivity estimate, or proof that the constraint determined the optimum. The model supplies no deterministic tie-breaking guarantee. The snapshot and version/hash metadata support replay and audit; they do not imply that another solver build will choose identical prices when multiple optima exist.

For the synthetic selection `[1, 1]`, bread is 200 cents and beans 300 cents. Usual-scenario revenue is 6,800, unit costs 3,400, protected wages 800, operating cost 100, reserve 100, leaving 2,400. Low-demand surplus is 1,400. At probabilities 0.6 and 0.4, expected surplus is 2,000. Bread's 240-cent candidate is independently excluded by both affordability and price-change limits. None of these supplied forecasts is an observed store outcome.

## Scope of the boundary

This contract is an optimizer integration boundary for a synthetic store dashboard. The deadline/cancellation supervisor owns process execution and operational events; those controls do not become AMPL policy variables. Policy editing authority, customer-facing publication, worker governance, and broader risk management remain outside the optimizer. Live prices require a separate application and appropriate AMPL entitlement.
