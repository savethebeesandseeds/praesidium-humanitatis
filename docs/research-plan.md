# Research plan: post-profit economics

Praesidium Humanitatis researches the discovery and formalization of
post-profit-economic models of **production, distribution, exchange and
allocation**. The [research framework](post-profit-economics.md) defines these
areas, the working meaning of post-profit and the requirements for each
demonstration.

## Institutional research agenda

For each model, move from a concrete question and explicit assumptions to a
formal specification, reproducible demonstration and evaluation of outcomes.
State who governs the model, who benefits, who bears costs and what evidence
would challenge its claims. Technical correctness and economic or human benefit
require separate evidence.

| Area | Initial research direction | Next evidence or definition needed |
| --- | --- | --- |
| **Production** | Worker-governed factory and laboratory models. | Select a concrete productive process; define inputs, outputs, work, capacity, governance and continuity before creating a demonstration project. |
| **Distribution** | Provision, transport and availability of goods; a separate project placeholder. | Define a distribution model, its participants, resources and evidence requirements. |
| **Exchange** | The `post-profit-exchange` store model, followed by comparison with other mechanisms where appropriate. | Evaluate operating-balance feedback against empirical demand response; define settlement workflow and distinguish supplied scenarios from evidence of actual response to prices. |
| **Allocation** | Store budgets and surplus; a separate project placeholder. | Specify decision rights, priorities, shortage rules and outcome measures for each setting. |

The store now has an initial synthetic C++/WebAssembly simulator and reconciled
accounts and operating-balance feedback. Its next research work is empirical
evidence of stability, affordability and continuity.
Production and allocation models remain to be defined. The
[adversarial-cooperation track](adversarial-cooperation.md) examines transition
and incumbent interests; its protocol remains future work.

Reusable tools support these demonstrations and retain independent technical
evaluation. The MIT [simple exponential smoothing baseline](../tools/exponential-smoothing/README.md) now
supplies the exchange's EWMA core independently of the application. Empty history
and declared priors are the default; generated sales remain simulation evidence.
Within the separate Temporal Fusion Transformer tool, the current priority is to establish
TFT replication before adding query inspection. The meaning and contract of
query inspection still need definition; integration is not implemented.
The [replication gate](tft-replication.md) separates correctness of this code,
numerical agreement with the authors' reference, and comparable real-data results.
Synthetic test success alone does not satisfy that gate.

## Research infrastructure: evidence for the forecasting engine

### 1. Establish and confirm the forecasting foundation

Core tests cover the TFT's causal input contract, training gradients,
serialization, and CPU/CUDA execution. Versioned model/Adam checkpoints,
reproducible synthetic continuation, forecast diagnostics, optional ordered
quantiles, and separate interval calibration are implemented. Completed synthetic
experiments demonstrate learning and expose uncertainty and optimization limits;
they do not establish real-data usefulness or paper benchmark parity.

The amended long-range CPU/CUDA confirmation completed on 2026-09-18: both suites
passed all three original seeds at 3,200 updates on a fresh held-out stream.
The initial 16/17 run and its CPU failure at 800 updates remain part of the record.
The unchanged earlier checks plus the focused confirmation provide passing
evidence for all 17 registered suites, across separate runs. See the
[validation handoff](PENDING-TFT-VALIDATION.md) and
[full uncertainty study](uncertainty-validation.md). This closes the pending
internal confirmation; direct reference parity and real-data replication remain.

### 2. Test harder synthetic problems and real temporal benchmarks

Use controlled synthetic problems to examine history dependence, uncertainty,
and optimization across predeclared seeds and budgets. Then assess real datasets
with a specified target, unit, time resolution, and forecast horizon. These can
be general forecasting benchmarks. Record dataset permission, provenance,
collection practices, missingness, reporting delays, and permitted uses; retain
raw data outside version control.

Build a data pipeline that records both event time and availability time. Fit
scalers and category mappings only on training data. Calendar covariates can be
known in advance; revised statistics, future observations, and later annotations
cannot be smuggled into the forecast. Split by time before constructing training
windows, enforce forecast-label boundaries, and add entity holdouts when assessing
generalization to new places or groups.

Compare with persistence, seasonal naive forecasts, and a small statistical
model before attributing value to TFT. Predeclare metrics: quantile loss, median
absolute error, interval coverage and width, crossing rate, and error across
horizons and relevant dataset groups. Use rolling-origin validation, leave a final
test period untouched, and record seed, feature schema, software versions, data
version, training configuration, and compute cost. Include negative results.

Measure practical performance as well: training time, inference latency,
throughput, and peak memory at specified history lengths, forecast horizons, and
batch sizes. Record hardware, warm-up, and device synchronization so CPU/CUDA
timings describe comparable work. These measurements remain future benchmark
work; completed correctness tests alone do not establish speed or scalability.

Evaluate quantile ordering and calibration separately from forecast accuracy.
Completed uncertainty experiments do not establish calibrated individual
quantiles or coverage for dependent temporal windows and distribution shifts.
Keep calibration data separate and corrections tied to the unchanged model and
preprocessing. See [uncertainty validation](uncertainty-validation.md).

### 3. Define and evaluate query-inspection integration

Define what query inspection means, its inputs and outputs, and its relationship
to TFT before selecting an integration design. Establish evaluation criteria for
that contract and test the combined engine against appropriate alternatives.
Forecast quality and the value added by the integration require separate evidence.
Do not assume query inspection is an alert or intervention rule.

## Exchange demonstration: store operation, distribution and allocation

The [store project](../projects/post-profit-exchange/README.md) researches
whether a worker-governed store can maintain access to goods,
protected compensation and operational continuity without passive-owner profit
extraction. Its [specification](../projects/post-profit-exchange/STORE_SPECIFICATION.md)
defines the proposed operating model and simulator acceptance criteria.

The standalone simulator implements reconciled inventory, economic and cash
accounts with feedback from actual funding history. Earned excess calls for
lower prices; shortfalls call for useful increases. The optimizer minimizes
absolute projected funding imbalance within supplied protections. Extend the
operating evidence and investigate stability under empirical demand response;
synthetic validation supports only the implemented mechanism.
Track the store's distribution outcomes, terms of exchange and allocation of
budgets and surplus separately.

Compare policies under equivalent starting resources and external shocks,
including fixed approved prices and the current engine objective. A claim about
the effects of removing owner returns also requires an explicit comparison
model that accounts for financing, contributions and transition costs. Measure
unmet need, affordability, worker outcomes, waste, liquidity and dependence on
outside support. Report cases where the proposed model is infeasible or offers
no advantage. A successful price solve does not establish store viability.

## Engineering work deliberately left open

- Live exchange integration, authenticated publication and settlement workflows.
- Demonstration-specific exchange and allocation rules, governance protocols
  and comparisons of economic outcomes.
- Versioned dataset schema, availability-aware preprocessing, and temporal splits.
- Train/validate/infer workflows on real data using the implemented checkpoint API.
- Missing-data and variable-length semantics that are tested through the LSTMs.
- Benchmark replication, harder learning tasks, and comparisons with simpler models.
- Calibration assessment on real holdouts and prediction monitoring.
- A defined query-inspection contract, integration, and independent evaluation.
- Application-specific decision rules and evaluation of actual human benefit.

Progress across the program should keep claims within the evidence. Reusable
tools, formal economic models and beneficial operations are related research
outputs, each with its own evaluation.
