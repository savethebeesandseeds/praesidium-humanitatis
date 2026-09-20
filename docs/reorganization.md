# Research institution, demonstrations and tools

Praesidium Humanitatis's organizing purpose is the discovery and formalization
of post-profit-economic models of production, distribution, exchange and
allocation. The [research framework](post-profit-economics.md) defines this
institutional scope; the [research plan](research-plan.md) connects it to work
in the repository.

`projects/` contains research demonstrations of those models. `tools/` contains
reusable technical capabilities with independent validation and explicit
licenses. `docs/` contains the institutional framework, research plans and
evidence records. A tool's technical success and a demonstration's economic
or human outcomes remain separate claims.

The store is an exchange demonstration with distribution and allocation
mechanisms. Suffering mitigation is a proposed allocation demonstration.
Production models for factories and laboratories remain future work. The
institutional framing preserves the existing implementation, licenses and
validation records.

## Tool and application separation

On 2026-09-20 the existing TFT source was moved from `code/` to
`tools/temporal-fusion-transformer/`. All 44 existing files were hash-verified
during the move before path updates. Model code, public headers, target names,
dependency pins, checkpoints, and historical verification artifacts were
preserved. Only paths in scripts and documentation changed.

The TFT has an explicit unchanged MIT license copy. Root documentation, shared
build entry points, and `projects/` remain MIT. The new price optimization tool
has a separate restrictive license. Its conditions apply to copied or derived
engine material and use of the engine, even through an independent MIT client.
See [licensing](licensing.md) for the exact boundaries and legal draft status.

Canonical TFT scripts now build into `.build/temporal-fusion-transformer`.
Historical `.build/tft` caches and outputs are preserved. Compatibility wrappers
retain the old `code/tasks.sh`, environment launch/setup commands, and CMake
entry; callers of source-level paths should migrate to the canonical directory.
Existing verification logs still record the original paths and are not rewritten.
The TFT container's name, image, bind mount, and device access stay as documented
in [its environment record](environment.md).

The optimizer has its own CPU-only Debian container definition and independent
build. It does not link TFT or require its CUDA environment. AMPL, the C++ SDK,
solver, and suitable runtime license are external prerequisites for solving;
offline C++ validation tests do not demonstrate AMPL integration or real-store
performance.

The two demonstration directories define their purpose and integration contracts.
They do not claim finished humanitarian interventions or a deployed autonomous
store. [post-profit-exchange](../projects/post-profit-exchange/README.md) is the
store application. Its [store specification](../projects/post-profit-exchange/STORE_SPECIFICATION.md)
covers the intended operation: inventory, procurement, sales, accounts and
worker benefit. Real-time price selection is one optimization decision within
that operation. The standalone browser simulator compiles the shared pricing
core and an explicit bounded enumeration backend to WebAssembly. The Linux AMPL
backend remains available through `ph.price.v1`; it does not run in the HTML.
Synthetic accounts and plain charts are implemented. Operational integration,
governance and broader risk management follow later.

Factory and laboratory applications belong in their own future project folders,
with distinct operating models and appropriate shared tools. No such application
folders are created by the store reorganization. Pricing-specific source names,
build targets, API versions and the optimizer container still identify the
existing pricing tool; the store project has its own name and scope.

## Migration verification

The canonical TFT build completed in the existing verified Debian container.
All 10 CPU CTest suites passed on 2026-09-20 (228.13 seconds), including
long-history learning, compatibility, checkpointing, and the CLI continuation
workflow. The log is retained locally at
`.build/temporal-fusion-transformer/Testing/Temporary/LastTest.log`. The CUDA
suites were not rerun for this path-only change; prior CUDA evidence remains
in its original verification records.

The old PowerShell launcher correctly forwards to the verified TFT container.
The old CMake entry configured successfully with the documented CUDA profile;
canonical scripts load that profile themselves. Nine Bash scripts, six Python
files, and three PowerShell launchers passed syntax checks. Relative Markdown
links resolved. Git status worked from the workspace owner's account without
a global ownership exception; `.git` ownership was unchanged.

The optimizer's separate [verification record](price-optimization-verification.md)
records the subsequent approved container provisioning, offline tests, SDK
compilation, and the status of real AMPL/HiGHS evaluation.
