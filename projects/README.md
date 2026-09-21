# Research demonstrations

Each project is a demonstration within Praesidium Humanitatis's
[post-profit economics research program](../docs/post-profit-economics.md).
Projects discover, formalize and evaluate economic models of production,
distribution, exchange and allocation in concrete settings. Each states its
research question and evidence stage.

Application code and documents in this directory are MIT licensed, as specified
by the repository's root `LICENSE`. Dependencies retain their own terms.

| Project | Economic areas and purpose | Reusable tool | Current state |
| --- | --- | --- | --- |
| [post-profit-exchange](post-profit-exchange/README.md) | **Exchange**, with **distribution** and **allocation**: investigate a self-sufficient store under worker control, without passive-owner profit extraction | MIT [simple exponential smoothing baseline](../tools/exponential-smoothing/README.md); separately licensed price optimizer with AMPL and bounded offline backends | Standalone C++/WebAssembly simulation, plain charts and reconciled synthetic accounts; live operation remains future work |
| [Suffering mitigation](suffering-mitigation/README.md) | **Allocation**: investigate how evidence of human need can inform limited response resources and evaluate actions that reduce suffering | MIT Temporal Fusion Transformer | Research definition; allocation rules and intervention evaluation remain open; forecasting validation is a separate gate |
| [post-profit-assurance](post-profit-assurance/README.md) | **Allocation** and continuity: research collectively governed support for productive units facing resource shortfalls | Proposed `exchange.assurance.v1` event boundary | Specification placeholder and unfunded exchange events only; no receiver, funded pool or payouts |

Every demonstration must identify its hypothesis, participants, governance,
resources and flows, objectives and constraints, comparison models, and
measures of benefit and harm. It must report what is specified, implemented
and supported by evidence. See the
[formalization requirements](../docs/post-profit-economics.md#discovery-formalization-and-demonstration).

Each application folder describes a complete domain of operation. For
`post-profit-exchange`, this includes stock, procurement, sales, accounts and
worker benefit. Its [store specification](post-profit-exchange/STORE_SPECIFICATION.md)
defines policy, objectives, costs and evidence; the optimization tool returns
price decisions for the application to evaluate and apply.

Applications consume versioned tool interfaces. Store workflows and dashboard
code belong in the store project; reusable solving, validation and model code
belong in the tool. Tools must not depend on application code.

Factories and laboratories are prospective **production** demonstrations with
their own specifications, reusing tools where appropriate. The production,
distribution and allocation folders are placeholders; `post-profit-exchange`
contains the store demonstration. The portfolio can grow across all four areas.

Using the optimizer through an MIT application does not remove the optimizer's
conditions. See [license boundaries](../docs/licensing.md).
