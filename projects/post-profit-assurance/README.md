<!-- SPDX-License-Identifier: MIT -->
# post-profit-assurance

This is a **future research component**, currently a specification and integration
placeholder. Its proposed service name is **Post-Profit Continuity Assurance**;
the exchange configuration identifies its contract as
`post_profit_continuity_assurance`.

The research question is how worker-governed productive units could support
continuity through transparent, collectively governed assistance when operating
resources become insufficient. Pool funding, eligibility, contributions, risk
sharing, decisions, recovery and participant control still need formalization
and evaluation. This folder does not implement an insurer, funded pool or payout
service.

The [exchange simulator](../post-profit-exchange/README.md) already records
unfunded requests when forecast coverage fails or operating cash falls below its
configured buffer. It also records simulated insolvency. Those events expose
needs and evidence for research; they do not contact this project, obtain
approval, create a receivable or add money to the exchange's accounts.

The [integration contract](CONTRACT.md) documents the existing
`exchange.assurance.v1` payload and `exchange.assurance-log.v1` run envelope.
Canonical event identity is `(run_id, policy, event.id)`. IDs are scoped to a run
and policy; the producer does not provide globally unique run IDs or an
idempotent payment service. Every current event has
`settlement_status: "unfunded_request"`.

Future work must define how requests are evaluated, how assistance is funded and
authorized, how duplicate/conflicting records are handled, and how confirmed
payments would be reconciled exactly once. No approval, premium, payment,
guaranteed coverage or economic benefit is claimed by the present prototype.
The application documents in this folder are MIT; dependencies retain their
own terms.
