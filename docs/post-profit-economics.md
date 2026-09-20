<!-- SPDX-License-Identifier: MIT -->
# Post-profit economics

## Institutional purpose

Praesidium Humanitatis is a research institution for the discovery and
formalization of post-profit-economic models of **production, distribution,
exchange and allocation**. We call this research program **post-profit
economics** (`post-profit-economics`).

The institution develops models, makes their assumptions explicit and tests
them through research demonstrations. Its purpose is to build knowledge about
economic arrangements that meet human needs, reduce suffering and sustain
productive activity under accountable governance.

## Working definition

Within this program, a post-profit-economic model organizes activity around
human benefit and the continued provision of useful goods and services. It
excludes passive-owner profit extraction and specifies who controls productive
resources, makes decisions and receives the benefits and burdens of operation.

Worker compensation, necessary costs, reserves, replacement of assets,
reinvestment and collectively governed surplus remain essential considerations.
Models must account for startup resources, financing obligations and outside
support. A positive accounting surplus can support the model's purposes;
maximizing that surplus is not a universal institutional objective. Each model
must state its own objectives, protected obligations and rules for surplus.

The program can investigate monetary and nonmonetary arrangements. The store's
public-price mechanism is one proposed form of exchange. Research should make
the conditions under which a mechanism works explicit and compare alternatives
without presuming that one mechanism fits every setting.

## Four connected research areas

| Area | Scope | Questions a model must address |
| --- | --- | --- |
| **1. Production** | Creating and maintaining goods, services and productive capacity. | What is produced, with which labor, knowledge, assets and inputs? Who controls work and productive decisions? How are capacity, maintenance and resource limits represented? |
| **2. Distribution** | Moving and providing goods and services from their source to the people who use them. | How do procurement, storage, logistics and service provision work? What determines availability, access, reliability, waste and unmet need? |
| **3. Exchange** | The terms and mechanisms by which participants transfer goods, services and resources. | How are prices, reciprocal commitments or other terms set? How are transfers recorded and settled, and how are obligations and disputes handled? |
| **4. Allocation** | Assigning scarce resources, capacity and surplus among competing needs and uses. | Which needs and uses receive priority? Who decides, under what constraints, and with what means of review? How are shortages, tradeoffs and competing claims resolved? |

These are research areas, not mandatory separate projects. Distribution studies
how provision reaches people; allocation studies which needs and uses receive
limited resources. Exchange specifies the terms of transfers. A store can
therefore investigate all three while depending on production models developed
elsewhere. Each demonstration identifies its primary area and its interfaces
with the others.

## Discovery, formalization and demonstration

**Discovery** means identifying needs, institutional arrangements and candidate
mechanisms with the people affected, and developing questions whose answers
could change the model. Record competing explanations and conditions under
which the proposal would fail.

**Formalization** means specifying enough of a model for others to inspect,
implement and challenge it. Mathematical formulations, governance protocols,
accounting rules and operational specifications are complementary research
outputs. Formalization must cover:

1. The research question, economic area, participants and boundary of the model.
2. Resources, state, units, time horizons, flows and accounting identities.
3. Decision rights, delegation, review, dispute handling and control of surplus.
4. Decisions, objectives, hard protections, constraints and failure behavior.
5. Assumptions about behavior, demand, information, financing and dependencies.
6. Predictions, comparison models, measures of benefit and harm, and evidence
   that would count against the proposal.

**Demonstration** means making a model inspectable and testable in a bounded
setting. Each project under `projects/` serves this purpose. It must state its
current stage: research definition, formal specification, simulation, shadow
evaluation or evaluated operation. A proposed demonstration is a research
commitment; it is not evidence that the model already works.

Evaluation proceeds from formal consistency and reproducible simulation to
empirical assessment when the necessary evidence and participant authority
exist. Compare models under declared starting resources, obligations and
external conditions. Report actual access, worker outcomes, resource use and
continuity alongside accounts. Publish limitations, failed cases and negative
results. Conclusions apply only to the conditions evaluated.

## Current demonstrations and research infrastructure

| Project | Economic model under study | Current evidence boundary |
| --- | --- | --- |
| [post-profit-exchange](../projects/post-profit-exchange/README.md) | Worker-governed store operation centered on public-price exchange, with distribution and allocation of inventory, budgets and surplus. | Standalone synthetic C++/WebAssembly simulation and reconciled accounts; proposed later objective stages and operational evaluation remain open. |
| [Suffering mitigation](../projects/suffering-mitigation/README.md) | Allocation of limited response resources according to human need and evaluated benefit. | Research definition. A concrete setting, allocation mechanism and intervention evaluation still need to be developed; forecasting validation is a separate prerequisite for forecast-based use. |

Production is part of the institution's mandate. Factory and laboratory models
are prospective demonstrations with no project folders or validated operating
models yet. Broader exchange and allocation models can be studied as the program
develops; the current portfolio does not exhaust the four areas.

Reusable forecasting and optimization capabilities belong in `tools/`. Their
technical evaluation remains independently useful. Forecast accuracy, solver
correctness, the viability of an economic model and evidence of human benefit
are distinct claims requiring distinct evidence.

The existing price engine maximizes expected worker-retained surplus subject
to supplied protections. The proposed store specification adds a surplus
target and affordability stages that are not yet implemented. Neither
objective defines all of post-profit economics.

Transition between existing arrangements and proposed models is also a research
question. The [adversarial-cooperation note](adversarial-cooperation.md) records
the intended work on incumbent interests, contributions and voluntary
cooperation. It remains a proposed protocol.

The [research plan](research-plan.md) sets out current work. The
[project index](../projects/README.md) maps demonstrations to the program;
[licensing](licensing.md) records the separate terms of the research tools.
