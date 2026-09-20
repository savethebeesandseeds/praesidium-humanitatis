<!-- SPDX-License-Identifier: MIT -->
# Research note: disruptive potential and adversarial cooperation

Status: research direction and proposed future protocol, 2026-09-20. The
optimizer and [post-profit-exchange](../projects/post-profit-exchange/README.md)
application research are experimental infrastructure.
The transition protocol described here has not been constructed or validated.

This work addresses transition and cooperation within Praesidium Humanitatis's
[post-profit economics program](post-profit-economics.md): the discovery and
formalization of models of production, distribution, exchange and allocation.
It examines how participants in existing arrangements could engage with the
proposed models and how their interests and contributions would be represented.

Our related work is
[Adversarial Cooperation](https://github.com/savethebeesandseeds/adversarial-cooperation).
We direct existing factory and store owners, and others who exercise decision-making
power within the current ownership model, to that work and to the research
questions below. This note states the intended application to productive units;
it does not claim that the linked work already supplies a completed protocol
for this setting.

## Research framing and disruptive potential

We are researching whether productive units can sustain themselves and serve
workers and customers without a passive owner's claim on their surplus. If
successful, this could disrupt existing arrangements for pricing, ownership
income and control in stores and factories. The effects could extend to
workers, customers, suppliers, creditors and communities, as well as owners.
Those consequences are part of the research problem, not an incidental outcome
to disregard.

Removing the requirement to generate profit for a passive owner may reduce the
revenue a unit needs to sustain its operation. That could create room for more
accessible prices, better worker compensation or greater resilience. This is
a potential structural advantage to investigate, not an established finding
of the current software. Procurement, financing, productive capacity, service
quality, transition costs and contributions previously made by an owner still
need to be accounted for in a fair comparison.

Here, "post-profit" refers to the absence of private profit extraction by
passive owners. It does not mean ignoring costs, withholding worker pay,
eliminating reserves or prohibiting worker-controlled surplus. The implemented
optimizer currently maximizes expected worker-retained surplus within supplied
constraints. It has no matched owner-return baseline and its synthetic tests
do not demonstrate economic superiority or real-world self-sufficiency.

## Note to incumbent owners

Factory and store owners have interests that may conflict with this operating
model, including invested resources, existing commitments, livelihoods and
expectations about income and control. We intend to examine those interests
explicitly, alongside the interests of workers and customers. Owners who also
work in their productive unit contribute labor and expertise that should be
distinguished from a passive claim on its surplus.

We invite these incumbent participants to engage with our
[Adversarial Cooperation research](https://github.com/savethebeesandseeds/adversarial-cooperation).
The proposed application begins by acknowledging conflicting incentives and
looking for terms on which participants can cooperate. It does not presume
that everyone has the same interests or that technical success alone resolves
the conflict.

## Protocol we aim to construct

We aim to construct a protocol for addressing incumbent owners' interests in
relation to the potential operating advantage of foregoing passive-owner
profit extraction. The research should make that tension explicit and examine
possible transitions without sacrificing worker control or making the new
operation dependent on permanent owner-profit obligations.

The intended protocol should provide a way to:

1. State participants' interests, contributions, commitments and points of
   conflict, separating compensation for actual work and resources from
   expectations of continuing control or passive returns.
2. Compare operating models under declared, comparable assumptions and expose
   who bears transition costs, who benefits and which claimed advantages remain
   uncertain. Include cases where the proposed model has no advantage.
3. Develop and evaluate voluntary proposals for participation, transfer or use
   of productive assets, continued productive work and other forms of cooperation.
   Proposed terms must identify their effects on workers and customers as well
   as owners; no particular compensation arrangement is promised here.
4. Record disagreements, allow proposals to be challenged or declined, and
   define how commitments would be reviewed, revised or ended. Preserve the
   productive unit's independent decisions and the project's worker protections.
5. Evaluate whether agreements can sustain cooperation despite conflicting
   interests, including when assumptions change, rather than treating an initial
   agreement as proof of a successful transition.

This is a statement of research intent, not a negotiation offer or an
implemented governance or risk-management system. The immediate technical
work remains the optimization engine and the `post-profit-exchange` application,
beginning with a synthetic store demonstration.
The protocol is a later research track. Calling the project research does not
change its licenses or establish permission for a live operational deployment.
