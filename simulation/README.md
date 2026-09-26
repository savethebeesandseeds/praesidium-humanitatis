<!-- SPDX-License-Identifier: MIT -->
# Post-profit economy tycoon engine

The home for our own tycoon engine: a world where we can build, run and inspect
post-profit-economic models of production, distribution, exchange and allocation.
People, workplaces, stores, transport and shared resources should make the
economy visible, from the movement of goods to the decisions about work and
surplus.

**Stage:** initial design. This directory establishes the engine's purpose and
boundaries; it does not yet contain a runnable engine. The existing
[store simulator](../projects/post-profit-exchange/README.md) remains runnable
in its project.

## What the player should be able to do

- Build a settlement of homes, productive workplaces, stores and supply routes.
- Watch resources become goods, move through storage and transport, and meet
  people's needs. Inspect shortages, waste and capacity limits where they occur.
- Explore worker-governed operating policies, protected compensation, reserves,
  reinvestment and collective decisions about surplus.
- Pause, advance time, inspect an entity and trace its resources and accounts.
- Replay a scenario and compare policies with equivalent starting resources
  and external shocks.

The measures of success come from the scenario: needs met, access to goods,
worker outcomes, continuity and resource use. Accounts explain how activity is
supported. Post-profit operation excludes passive-owner profit extraction;
worker pay, costs, reserves and collectively governed surplus remain visible.

## Repository boundaries

| Location | Responsibility |
| --- | --- |
| `simulation/` | Shared world engine, scene rendering, camera and input, time controls, inspection, scenario composition and adapters to economic models. |
| [`projects/`](../projects/README.md) | Economic operating models, governance rules, domain state, accounts, research questions and evaluation. |
| [`tools/`](../README.md#tools) | Reusable forecasting, optimization and other technical capabilities. |
| [`docs/`](../docs/post-profit-economics.md) | Institutional framework, research plans and evidence. |

The engine composes project models through explicit interfaces. Projects and
tools must remain usable without the graphical engine. The store's existing
`projects/post-profit-exchange/simulation/` contains its domain simulator and
stays with that project; its pricing and accounting rules should have one
implementation.

Simulation state advances independently of rendering. Drawing another frame,
moving the camera or changing playback speed must not change an economic result.
Scenario inputs, model versions, seeds and player decisions should be recorded
so a run can be reproduced. Visual interpolation must not create economic
transactions or imply detail that the underlying model does not supply.

## First milestone: one store on a small map

Start with a store, a supplier connection and a neighborhood representing its
customers. Use the existing synthetic exchange example as the economic source.
The first scene should make daily procurement, stock, sales, wages, reserves,
waste and unmet demand inspectable, with pause, day stepping and policy selection.

The current [`exchange.sim.v3` contract](../projects/post-profit-exchange/simulation/CONTRACT.md)
returns complete runs with daily rows, events and separate balancing-feedback
and fixed-price paths. The first adapter can replay those results. It must
preserve each path's actual horizon, show terminal events and retain units and
accounting meanings. Requested assurance is unfunded unless a model explicitly
records a funded settlement.

Customer and delivery animation initially illustrates aggregate daily flows;
the existing contract does not provide individual routes or transaction times.
Changing an economic input requires a new run. Interactive changes within a
running economy need an explicit state-and-command interface in the relevant
project before the graphical engine can apply them.

Completion means the scene can replay a run, explain the store's state on any
available day, reconcile displayed quantities and money with the source records,
and reproduce the same results regardless of frame rate or playback speed.

## Growing into a playable economy

After store playback, add world editing and incremental model interfaces, then
production, transport and allocation through their project contracts. Production
recipes, labor capacity, travel time, storage and resource limits must affect
the simulation through explicit rules. Their models still need to be developed.

Choose the rendering technology and visual perspective when building the first
scene. Keep the economic state accessible to headless runs and automated
validation. Synthetic scenarios should expose their assumptions and let us
observe failures as well as successes; their outcomes are simulation evidence.

Independent engine code and documentation use the repository's [MIT license](../LICENSE).
Dependencies retain their own terms, including the price optimizer's separate
license when it is used. See the existing [license boundaries](../docs/licensing.md).
