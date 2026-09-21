<!-- SPDX-License-Identifier: MIT -->
# Proposed continuity-assurance integration

Status: research handoff, 2026-09-21. The exchange producer and local JSON log
exist. An assurance receiver, decision process, pooled fund and settlement
implementation do not. This document distinguishes current producer behavior
from requirements for a future consumer.

## Existing producer and event envelope

The source is `projects/post-profit-exchange`, simulator schema
`exchange.sim.v3`. Its native file runner writes `assurance-events.json` using:

```text
{
  "schema_version": "exchange.assurance-log.v1",
  "run_id": "caller-chosen-run",
  "events": [
    {"policy": "optimized", "event": <exchange.assurance.v1 object>}
  ]
}
```

`optimized` is the simulator's balancing-feedback policy; the other value is
`fixed`. The native run manifest links the configuration, history, complete
simulation result, daily records and event log. The direct C++/WASM simulator
returns per-policy events without a run envelope. A browser export or another
caller must retain or assign the run context before aggregating events.

Canonical identity is the **tuple `(run_id, policy, event.id)`**. Treat its parts
as structured values; do not create an ambiguous concatenated key. The producer's
local `event.id` is currently `policy-day-type`, so separate runs can have the
same event ID. Policy in the envelope and payload must agree. `run_id` is a
caller-supplied 1–64 character label using letters, digits, dot, underscore or
dash. The file runner checks syntax and refuses an existing output path, but
does not enforce unique run IDs across directories. An aggregator must establish
distinct run identities in its namespace and retain source-manifest provenance.
Neither this tuple nor the log provides authentication or tamper evidence.

## Existing payload

Every current event contains all these fields:

| Field | Producer meaning |
| --- | --- |
| `schema_version` | Exactly `exchange.assurance.v1`. |
| `id` | Local policy/day/type identifier. |
| `policy` | `optimized` or `fixed`. |
| `type` | `assurance_requested` or `insolvency_declared`. |
| `provider` | Exactly `post_profit_continuity_assurance`. This identifies a proposed contract, not a contacted provider. |
| `day` | Simulated day, including zero for exhausted opening cash. |
| `reason` | Producer explanation; technical failures include diagnostic text. It is not a stable decision-code taxonomy. |
| `currency` | The simulation's three-letter uppercase currency; amounts are integer minor units. |
| `required_support` | Nonnegative requested buffer support at that event, not an approved claim or payout. |
| `cash`, `reserve`, `inventory_value` | Current state; reserve is a subset of cash, inventory is valued at FIFO costs. |
| `funding_balance` | Signed earned funding history after scheduled reserve requirements; endowed cash is not earned funding. |
| `wage_arrears`, `operating_arrears` | Outstanding fixed obligations. |
| `forecast` | Forecast/reserve evidence when available; an empty object at opening-cash failure. |
| `settlement_status` | Exactly `unfunded_request` for every current event, including insolvency records. |

Keep integer amounts and signed history intact. Do not add reserve to cash when
measuring resources, or treat inventory value as immediately spendable money.
Forecast sigma stress is conditional model evidence, not a calibrated loss
probability, premium, maximum claim or guarantee.

## Event triggers and consequences

The exchange can request assurance after an uncertified continuity-pricing day
or when cash is below `fixed_daily_cost * trigger_buffer_days + arrears`.
The usual requested support is that nonnegative difference. It can be zero when
the event instead identifies a coverage failure, so a zero amount does not mean
there was no warning. Opening cash exhaustion requests one day's fixed costs.
A technical model-error request has zero support and separate diagnostics; it
does not establish an economic loss claim.

`insolvency_declared` records the simulator's terminal condition: zero opening
cash, zero cash after paying fixed obligations, or remaining wage/operating
arrears. It ends that policy's simulated path. The other comparator can continue,
so its horizon and totals may differ. This is a model event, not a legal
determination or a demand for automatic payment.

All current events remain unfunded. They do not create an exchange receivable,
advance cash, forgive arrears, reverse a terminal outcome or transfer resources.
Writing or exporting the file sends no external message. The simulator does not
resume a stopped path on the assumption that assistance will arrive.

## Future receiver and settlement work

Before implementing a receiver, define version support, authenticated producer
authority, run identity, record validation and the governance of decisions.
Repeated identical canonical identities should be recognizable as replay;
conflicting payloads for an identity require explicit resolution. These are
proposed receiver requirements, **not an implemented idempotency guarantee**.
No event log currently prevents duplicate payout because no payout exists.

An eventual funding model must separately define sources of funds, eligibility,
contributions, limits, collective approval, appeals, insolvency handling and the
conditions for support. A requested amount must not become an approval merely
because a model produced it. Any implemented transfer would need an authorized
settlement record and reconciled receipts, distinguishable from this request
schema, before exchange cash or liabilities could change.

Preserve the [exchange file records](../post-profit-exchange/docs/FILES.md),
[simulator contract](../post-profit-exchange/simulation/CONTRACT.md) and
[forecast assumptions](../post-profit-exchange/docs/MODELS.md) with research
comparisons. The current interface establishes a traceable research handoff;
it does not establish funded protection or real-world effectiveness.
