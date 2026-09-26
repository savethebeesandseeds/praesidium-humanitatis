# Praesidium Humanitatis

Praesidium Humanitatis is a research institution dedicated to the discovery
and formalization of **post-profit-economic models of production,
distribution, exchange and allocation**. Its research program is
**post-profit economics** (`post-profit-economics`).

We investigate how economic activity can meet human needs, reduce suffering
and sustain the people and resources on which it depends. In this program,
post-profit models exclude passive-owner profit extraction. Worker
compensation, necessary costs, reserves, reinvestment and collectively governed
surplus remain part of economic life.

## Four research areas

| Area | Central question |
| --- | --- |
| **Production** | How are goods and services created, and how are productive work, assets and decisions governed? |
| **Distribution** | How do goods and services reach people reliably and accessibly through procurement, storage, logistics and provision? |
| **Exchange** | On what terms do people and organizations transfer goods, services and resources, including prices, reciprocity and settlement? |
| **Allocation** | How are scarce resources, capacity and surplus assigned among competing needs and uses, and who decides? |

These areas interact. Each project is a research demonstration of one or more
post-profit-economic models: it states a hypothesis, formalizes an operating
model and produces evidence about its behavior and limits. A demonstration
may be a specification, simulation or evaluated operation; its stage must be
explicit. An implemented tool alone does not establish economic viability or
human benefit.

The [research framework](docs/post-profit-economics.md) defines the program and
what a demonstration must establish. The [research plan](docs/research-plan.md)
connects that purpose to current work.

Shared repository material and application code are MIT licensed. Each tool has
an explicit license. AMPL and other dependencies retain their own terms.

We acknowledge the potentially disruptive implications of operating stores
and factories without passive-owner profit extraction. Any resulting advantage
in affordability, worker benefit or resilience remains a research hypothesis
to test. Existing factory and store owners, including those who exercise power
within current arrangements, are directed to our
[Adversarial Cooperation work](https://github.com/savethebeesandseeds/adversarial-cooperation).
We aim to construct a protocol addressing their interests in relation to this
potential advantage. The [research note](docs/adversarial-cooperation.md)
records that intention and the distinction between foregoing owner profit and
funding worker pay, necessary costs and worker-controlled surplus.

## Tools

| Tool | Purpose | License | Status |
| --- | --- | --- | --- |
| [Exponential smoothing (EWMA)](tools/exponential-smoothing/README.md) | Dependency-free C++17 simple exponential smoothing, uncertainty and past-only diagnostics | [MIT](tools/exponential-smoothing/LICENSE) | Nonseasonal single-level baseline used by exchange; empirical accuracy remains unvalidated |
| [Temporal Fusion Transformer](tools/temporal-fusion-transformer/README.md) | C++17 / LibTorch / CUDA forecasting with model diagnostics | [MIT](tools/temporal-fusion-transformer/LICENSE) | Implemented; real-data replication remains a gate |
| [Price optimization](tools/price-optimization/README.md) | C++17 pricing constraints and AMPL mixed-integer optimization | [Worker Protection License 1.0](tools/price-optimization/LICENSE) | Initial synthetic prototype; operational AMPL rights and store integration required |

The optimizer is source-available with use restrictions, not OSI open source.
Its license prohibits exploitation and coercion and requires democratic worker
control and collective benefit in productive deployments. It is a legally
unreviewed draft; enforceability depends on applicable law. Neither licensing nor
technical checks can guarantee prevention of misuse. See the precise
[license boundaries and review status](docs/licensing.md).

## Research demonstrations

[post-profit-exchange](projects/post-profit-exchange/README.md) is a store
research demonstration centered on **exchange** through public pricing, with
**distribution** through stock and procurement and **allocation** through
inventory, protected budgets and worker-governed surplus. Its scope includes
stock, procurement, sales, accounts and worker benefit. A
[standalone C++/WebAssembly simulator](projects/post-profit-exchange/dist/post-profit-exchange.html)
provides plain controls, charts and reconciled accounts. Live integration and
evaluation with observed operating data remain future work.

Future factory and laboratory demonstrations can investigate **production**
and its connections to the other areas, with their own operating models and
specifications. Those projects have not yet been defined. Demonstrations
belong in `projects/`; reusable research capabilities belong in `tools/`.

The separate pricing tool used by the store project selects one public price
per product, enforces affordability and price-change bounds, and requires
projected worker pay,
operating costs, and reserves to be covered in each supplied demand scenario.
It returns a recommendation for review. A live autonomous store, demand-response
estimator, payment system, and hard real-time service are not implemented.

A [supervised JSON backend](tools/price-optimization/docs/backend.md) now exposes
the existing optimizer for the synthetic dashboard example, with versioned
inputs, process deadlines/cancellation and independently checked explanations.

## Tycoon simulation engine

[`simulation/`](simulation/README.md) is the home for our own tycoon engine:
a visual world connecting workplaces, stores, households and supply routes
to the post-profit-economic models. It will own the shared world, rendering,
time controls and inspection, while projects define the economic rules.
The initial design starts with replaying the existing store simulation on a
small map; a runnable graphical engine and interactive world-building are
future work.

## Build and environments

The tools have independent builds. Simple exponential smoothing is a dependency-free C++17
library (`ph::exponential_smoothing`); the optimizer and TFT have separate documented
Debian environments. The smoothing baseline and optimizer do not require
LibTorch or CUDA. Container dependency installation is separate from lifecycle
and build tasks.

In an existing Linux environment with CMake and a C++17 compiler, build and test
the exponential-smoothing baseline and optimizer's dependency-free core:

```bash
cmake -S . -B .build/core -DCMAKE_BUILD_TYPE=Release
cmake --build .build/core --parallel 2
ctest --test-dir .build/core --output-on-failure --no-tests=error
```

This does not run AMPL. Follow the
[optimizer guide](tools/price-optimization/README.md) and its
[container procedure](tools/price-optimization/environment/README.md) for the
optional C++ API, licensed runtime, solver, and integration checks.

For the existing TFT environment, from PowerShell:

```powershell
.\tools\temporal-fusion-transformer\environment\container.ps1 up
.\tools\temporal-fusion-transformer\environment\container.ps1 -Action exec -Command @('bash', 'tools/temporal-fusion-transformer/tasks.sh', 'test')
```

See the [TFT environment](docs/environment.md) for exact dependencies. Use
`tools/temporal-fusion-transformer/tasks.sh` and
`tools/temporal-fusion-transformer/environment/` for TFT tasks and setup.
Existing build artifacts are preserved; canonical TFT builds use
`.build/temporal-fusion-transformer`.

## Layout

```text
LICENSE                              MIT default, with explicit tool exceptions
tools/
  exponential-smoothing/             MIT simple exponential smoothing and tests; no model-training data
  temporal-fusion-transformer/        MIT forecasting library, tests, environment
  price-optimization/                 restricted engine, AMPL model, tests, environment
projects/
  post-profit-exchange/               store simulation, specification, standalone HTML (MIT application)
  post-profit-distribution/           preserved research placeholder
  post-profit-production/             research placeholder
  post-profit-allocation/             research placeholder
  post-profit-assurance/              continuity-support contract placeholder; no funded payouts
simulation/                          shared tycoon engine; initial design and integration plan
docs/                                institutional framework, research, validation, licensing
.build/                              ignored dependencies, results, build outputs
.temp/                               ignored downloads and local working files
```

The TFT's existing [verification record](docs/verification.md),
[uncertainty study](docs/uncertainty-validation.md),
[replication plan](docs/tft-replication.md), and
[pending validation](docs/PENDING-TFT-VALIDATION.md) remain part of the project.
The institutional framing does not claim new forecasting or economic evidence.

[Licensing](docs/licensing.md) · [Third-party notices](docs/third-party-notices.md)
· [Reorganization](docs/reorganization.md)
