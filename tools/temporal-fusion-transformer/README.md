# Temporal Fusion Transformer

A reusable forecasting tool within Praesidium Humanitatis's
[post-profit economics research program](../../docs/post-profit-economics.md).
The implemented component is a **C++17 Temporal Fusion Transformer (TFT)
library using LibTorch and CUDA**. Connecting forecasting with **query
inspection** is a technical research goal; its query format, semantics and
connection to the model remain to be defined.

The immediate question is how well the engine works: what it learns, how reliably
it forecasts, how useful its inspection capabilities become, and where it fails.
Technical development and evaluation can proceed independently of a particular
application. The tool can support multiple economic demonstrations and other
forecasting uses.

## Tool research and demonstration use

- **Standalone engine:** develop forecasting and query inspection with explicit
  interfaces, reproducible experiments, and comparisons against simpler methods.
  Keep domain-specific targets and response rules outside the reusable core.
- **Allocation demonstration:** the
  [suffering-mitigation project](../../projects/suffering-mitigation/README.md)
  investigates how evidence of human need can inform limited response
  resources. Refine this with people who know the setting and those affected.
  Forecast performance, allocation quality and evidence that an intervention
  helps people are separate outcomes.

The demonstration can inform the engine without defining its entire scope or
being a prerequisite for technical progress. See the
[research plan](../../docs/research-plan.md) for its place in the wider program.

The current priority is [TFT replication and validation](../../docs/tft-replication.md).
Query-inspection integration follows that gate: internal correctness tests,
comparison with the authors' implementation, and a reproduced real-data benchmark
are different requirements. The existing synthetic results do not establish
equivalent forecasting performance.

## What is here

- A trainable single-target, multi-horizon TFT library with continuous and
  categorical inputs, variable selection, static context, encoder/decoder LSTMs,
  gated residual networks, causal interpretable attention, and quantile forecasts.
- Separate static, historical, and known-future input groups. Future targets are
  supplied only to the loss, never to the model's forward method.
- Variable-selection and attention diagnostics returned with predictions.
- Resumable training checkpoints containing configuration, Adam state, and progress.
- Forecast metrics, a persistence baseline, and JSON experiment reports.
- Optional ordered quantiles and a separate per-horizon interval calibration API.
- A synthetic training example, correctness tests, and CMake build.
- A pinned Debian 12 / CUDA 12.4 / LibTorch 2.6.0 container definition, adapted
  from the supplied local reference. Dependency setup and lifecycle are separate.

Verified on 2026-09-16–17: the model, checkpoint, and evaluation suites passed
on CPU and CUDA; the CPU command-line continuation workflow also passed.
The saved synthetic experiment beats persistence on error, while exposing
undercoverage and crossing quantiles. See the
[verification record](../../docs/verification.md) for exact results and their limits.

A deeper [core-network audit](../../docs/core-validation.md) passed all 13 expanded
CPU/CUDA test suites, including independent forward equations, numerical
gradient checks, device agreement, and held-out learning that requires history.
This strengthens confidence in the implementation. The subsequent
[uncertainty and distant-history study](../../docs/uncertainty-validation.md) adds
crossing prevention, independent calibration/test splits, and a harder temporal
learning test. Its initial expanded run passed 16/17 suites, with one CPU
distant-history seed failing at 800 updates. A longer diagnostic recovered. On
2026-09-18, both amended CPU/CUDA suites passed at 3,200 updates, including every
original seed on a fresh holdout. The original failure and remaining quantile
bias are retained in the study. Real-data forecasting remains unproven.

This is an initial research implementation. It is not yet a reproduction of the
paper's benchmark results, a general data ingestion system, a query-inspection
engine, or an evaluated service for decisions about people. The model uses
fixed-length complete windows; missing values, irregular sampling, scaling,
splitting, and feature provenance must be handled by a future data pipeline.

## Build and run

The Windows project folder is `C:\Work\praesidium-humanitatis`; the repository is
[praesidium-humanitatis](https://github.com/savethebeesandseeds/praesidium-humanitatis).
See [the exact container definition and prerequisites](../../docs/environment.md).
The local Linux LibTorch bundle belongs in `.build/deps/libtorch`. The project
container configuration has been approved and created. Run from this root in PowerShell:

```powershell
.\tools\temporal-fusion-transformer\environment\container.ps1 up
.\tools\temporal-fusion-transformer\environment\container.ps1 -Action exec -Command @('bash', 'tools/temporal-fusion-transformer/environment/setup.sh')
.\tools\temporal-fusion-transformer\environment\container.ps1 -Action exec -Command @('bash', 'tools/temporal-fusion-transformer/tasks.sh', 'test')
.\tools\temporal-fusion-transformer\environment\container.ps1 -Action exec -Command @('bash', 'tools/temporal-fusion-transformer/tasks.sh', 'test-cuda')
.\tools\temporal-fusion-transformer\environment\container.ps1 -Action exec -Command @('bash', 'tools/temporal-fusion-transformer/tasks.sh', 'demo', '--device', 'cuda', '--steps', '80')
```

Inside the provisioned project container, use `bash tools/temporal-fusion-transformer/tasks.sh test`.
In another Linux environment with the compiler, CMake, LibTorch, and its runtime
dependencies already available, build directly:

```bash
cmake -S tools/temporal-fusion-transformer -B .build/temporal-fusion-transformer -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/.build/deps/libtorch" -DPRAESIDIUM_HUMANITATIS_CUDA_TESTS=ON
cmake --build .build/temporal-fusion-transformer --parallel 2
ctest --test-dir .build/temporal-fusion-transformer --output-on-failure
.build/temporal-fusion-transformer/tft_demo --device cuda --steps 80
```

CUDA runs use LibTorch's CUDA kernels; no custom CUDA kernel or Python model is
required. CPU tests also work with a suitable CPU LibTorch distribution and a
separately provisioned compiler, though the managed setup checks its pinned CUDA
bundle. A skipped CUDA test does not establish that GPU execution works.

## Use the library

Include `praesidium-humanitatis/tft.h` and link the CMake target
`praesidium_humanitatis::tft`. Construct `praesidium_humanitatis::TFT(config)`, move
it with `model->to(device)`, and pass a `praesidium_humanitatis::Batch` to
`model->forward(batch)`. `praesidium_humanitatis::quantile_loss` accepts the returned
predictions and held-separate targets. See [tensor contracts and architecture](../../docs/model.md) and
[the runnable example](examples/train_synthetic.cpp).

Include `praesidium-humanitatis/checkpoint.h` to save and restore complete training
checkpoints, or `praesidium-humanitatis/evaluation.h` for forecast diagnostics.
The example supports
`--checkpoint`, `--resume`, `--evaluate-only`, and `--report`.
See [experiments and checkpoint contracts](../../docs/experiments.md).
Set `Config::ordered_quantiles` or start the demo with `--ordered-quantiles`
to prevent crossing forecasts. Include `praesidium-humanitatis/calibration.h`
for `fit_cqr` and `apply_cqr`; calibration uses a separate labeled split and
must remain associated with the fixed model. The `tft_uncertainty` executable
compares both heads against the known synthetic noise distribution.
Preprocessing and real dataset ingestion remain future work.

## Layout

Paths below are relative to this tool directory. Its complete source remains
[MIT licensed](LICENSE). Shared research documents and ignored build artifacts
live at the repository root.

```text
README.md                          TFT purpose and usage
LICENSE                            TFT MIT license
CMakeLists.txt                     library, examples, tests
include/praesidium-humanitatis/     model, checkpoint, evaluation, calibration APIs
src/                               model, objective, serialization, metrics, calibration
examples/                          synthetic training and uncertainty experiments
tests/                             correctness and training checks
replication/                       pinned reference, parity and Electricity harness
environment/                       dependency installer, pins, container launcher
tasks.sh                           build/test/demo operations
```

## Next evidence to earn

- Align the documented implementation differences and compare identical inputs
  and mapped weights with a pinned authors' reference, including gradients.
- Reproduce the Electricity forecasting protocol and normalized P50/P90 metrics,
  comparing repeated reference and C++ runs. See the
  [replication gate](../../docs/tft-replication.md) for what counts as similar results.
- After that gate, define query inspection through concrete input/output examples
  before choosing an interface or implementation. Forecast diagnostics already
  exist; they do not by themselves implement query inspection.
- After validating the forecasting foundation, develop forecast-based use in
  the suffering-mitigation allocation demonstration, with its own target, data,
  resource constraints, decision rules, feasible response and evaluation of
  benefit. Its domain-specific decisions belong in the application layer.

[Experiments](../../docs/experiments.md) · [Research plan](../../docs/research-plan.md)
· [Verification record](../../docs/verification.md)
· [Dependency notices](../../docs/third-party-notices.md)

The architecture is based on [Lim et al.'s TFT paper](https://arxiv.org/abs/1912.09363)
and checked against the [authors' reference implementation](https://github.com/google-research/google-research/blob/master/tft/libs/tft_model.py).
This independent C++ implementation is not an official release by the authors.
