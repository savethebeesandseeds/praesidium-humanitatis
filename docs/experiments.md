# Reproducible experiments and evaluation

The reusable checkpoint and evaluation APIs support the next stage of research.
The executable still uses synthetic data. It does not ingest a real dataset or
fit preprocessing for one.

## Save, resume, and inspect a run

Inside the provisioned container, from the project root:

```bash
# Train, save every 20 updates and at completion, and write a metrics report.
.build/temporal-fusion-transformer/tft_demo --device cuda --steps 80 \
  --checkpoint .build/runs/sine.pt --report .build/runs/sine.json

# Restore model, Adam, seed and step count; perform 40 additional updates.
.build/temporal-fusion-transformer/tft_demo --device cuda --resume .build/runs/sine.pt --steps 40 \
  --checkpoint .build/runs/sine.pt --report .build/runs/continued.json

# Evaluate without changing weights or optimizer state.
.build/temporal-fusion-transformer/tft_demo --device cuda --resume .build/runs/sine.pt \
  --evaluate-only --report .build/runs/evaluation.json
```

From PowerShell, prefix executable arguments with:

```powershell
.\tools\temporal-fusion-transformer\environment\container.ps1 -Action exec -Command @(
  'bash', 'tools/temporal-fusion-transformer/tasks.sh', 'demo', '--device', 'cuda', '--steps', '80',
  '--checkpoint', '.build/runs/sine.pt', '--report', '.build/runs/sine.json'
)
```

The CLI creates output parent directories. `--steps` means additional updates.
`--seed` is only valid for a fresh run; resume restores the stored seed.
`--save-every N` sets the periodic checkpoint interval; zero saves only at the
end. Periodic saves require `--checkpoint`; a resume input is not implicitly
overwritten. Use the same input and output checkpoint path to replace it
deliberately. `--evaluate-only` requires `--resume` and rejects `--steps`.
Reports must have a different path from the checkpoint.

Each JSON report stores the seed, initial/completed step counts, generator ID,
LibTorch version, device, dtype, model dimensions, quantiles, clipping threshold,
Adam settings, validation dimensions, and initial/final/baseline metrics.
The report and checkpoint are separate files; they are not published as one
transaction.

## Reproducibility contract

New runs identify the synthetic generator as
`praesidium-humanitatis.synthetic.sine.v1`. Checkpoints created before the project
rename with `adht.synthetic.sine.v1` remain compatible: the generator is unchanged,
and resumed checkpoints and reports retain their original identifier. Every
training update seeds Torch with `seed + completed_steps` before generating the
batch and applying dropout. Validation is regenerated from a separate fixed stream
(`seed XOR 0x5DEECE66D`) and is independent of the number of preceding updates.
The same saved run therefore gets the same validation set on the same device.

The CPU continuation test compares uninterrupted training with save/load/resume,
including Adam moments and dropout. The CLI workflow additionally compares the
full JSON metrics for uninterrupted training, resumed training, and evaluation
only. The guarantee is scoped to the same software build, generator, dtype,
device, and CPU thread setting. CUDA kernels, device-specific RNG streams, or
different software versions can change numerical results. Moving a checkpoint
between CPU and CUDA is supported, but does not promise an identical trajectory.

General library callers own the equivalent data-order and seeding protocol.
Checkpoint loading constructs a model and consumes RNG state; reseed before the
next update. Opaque RNG state, data loader state, and preprocessing are not saved.

## Checkpoint API

Include `praesidium-humanitatis/checkpoint.h` and link `praesidium_humanitatis::tft`.

```cpp
praesidium_humanitatis::TrainingProgress progress;
progress.completed_steps = completed_updates;
progress.seed = seed;
progress.gradient_clip_norm = 1.0;
progress.data_id = "your-dataset-and-feature-schema-version";
praesidium_humanitatis::save_checkpoint("model.pt", model, optimizer, progress);

auto restored = praesidium_humanitatis::load_checkpoint("model.pt", torch::Device(torch::kCUDA, 0));
// restored.model, restored.optimizer, restored.progress
```

Library callers must create the parent directory. Format version 3 stores the
complete model `Config`, float32/float64 dtype, model weights/buffers, top-level
train/eval mode, Adam parameter groups/options/moments, and progress metadata.
It reconstructs the topology automatically and transfers model and optimizer
tensors to the requested CPU/CUDA device. Unsupported versions, malformed
metadata, inconsistent parameter groups, and invalid numeric state are rejected.
The loader accepts versions 1 and 2 with legacy independent embeddings and
LayerNorm epsilon `1e-5`. Version 1 retains the independent quantile head;
version 2 stores `ordered_quantiles`. Version 3 additionally stores the epsilon
and embedding-sharing maps. New files need the updated loader.
Use `--ordered-quantiles` on fresh demo runs to train the
optional noncrossing head. Resume restores the saved mode and rejects overrides.

The optimizer must reference every model parameter exactly once across nonempty
groups. Save between completed updates without concurrent mutation. The
clipping norm is finite and strictly positive. Mixed per-submodule modes,
frozen-parameter flags, other optimizer types, learning-rate schedulers,
preprocessing, and dataset contents are outside this format.

Save uses a private sibling temporary directory, flushes and closes the archive,
then renames it over the destination. On Linux this replaces the previous file
atomically; a failed save preserves it. This is not an `fsync`-based power-loss
durability guarantee. Other platforms can reject replacement of an existing
file. Load only trusted LibTorch archives.

## What the metrics mean

Include `praesidium-humanitatis/evaluation.h` and call
`evaluate_forecasts(predictions, targets, quantiles)`. Inputs follow the
model/loss tensor contract. Evaluation requires an explicit 0.5 quantile and
at least one quantile on either side. Per-horizon tensors keep the prediction
device and dtype and have no autograd history.

| Metric | Definition |
| --- | --- |
| Quantile loss | Pinball loss summed over quantiles, averaged over batch/horizon |
| Median MAE | Mean absolute error of the explicit 0.5 forecast |
| Interval coverage | Fraction of targets inclusively between the first and last quantile predictions |
| Interval width | Mean signed upper-minus-lower width |
| Crossing rate | Fraction of sample/horizon positions with any adjacent quantile inversion |

Loss, MAE, and coverage are also reported by horizon. Predictions are never
sorted to hide crossings. A reversed outer interval has negative width and
cannot cover a target; inspect crossing rate alongside the mean width. For
quantiles 0.1 and 0.9, nominal coverage is 0.8, but empirical coverage on one
synthetic run does not establish calibration.

The optional ordered head prevents crossings by construction. Separate
per-horizon interval calibration and the reproducible uncertainty experiment
are documented in [uncertainty-validation.md](uncertainty-validation.md).

Include `praesidium-humanitatis/calibration.h` for the separate CQR API. With a
fixed saved model, fit on calibration windows and evaluate on distinct test
windows. The caller supplies these batches, their separate targets, and the
same preprocessing used by the model; tensors must match its device and dtype.

```cpp
#include "praesidium-humanitatis/checkpoint.h"
#include "praesidium-humanitatis/calibration.h"

namespace ph = praesidium_humanitatis;
auto session = ph::load_checkpoint("run/model.pt", torch::Device(torch::kCUDA, 0));
session.model->eval();
torch::NoGradGuard no_grad;
const auto endpoints = [](const torch::Tensor& predictions) {
  return ph::PredictionInterval{predictions.select(-1, 0),
                                predictions.select(-1, predictions.size(-1) - 1)};
};
const auto calibration_predictions = session.model->forward(calibration_batch).predictions;
const auto calibration = ph::fit_cqr(endpoints(calibration_predictions),
                                      calibration_targets, 0.2); // 80% per horizon
const auto test_predictions = session.model->forward(test_batch).predictions;
const auto interval = ph::apply_cqr(endpoints(test_predictions), calibration);
const auto metrics = ph::evaluate_intervals(interval, test_targets);
```

Keep `calibration.correction`, `miscoverage`, and `sample_count` paired with
that exact model and its feature/horizon definitions. The uncertainty executable
already writes these as `calibrated_interval.correction`, `miscoverage`, and
`calibration_size` in the sibling `report.json`. To reuse its saved correction,
read those fields and construct `CQRCalibration` on the loaded model's device
and dtype; do not refit on the test labels. The library has no calibration JSON
serialization API, and `model.pt` does not contain the calibration correction.
Preserve sufficient numeric precision when storing corrections. Retraining or
changing preprocessing requires fresh calibration. Reversed outer endpoints
are rejected; keep inspecting the original quantile metrics separately.

`persistence_forecast(last_observation, horizon, quantile_count)` repeats the
last observed target across all future steps and quantiles. This is a simple
point-forecast baseline with zero-width intervals, not an uncertainty model.
Compare median error and quantile loss while keeping that limitation explicit.

Before real-world claims, add temporal holdouts, data availability checks,
seasonal/statistical baselines, and assessment of relevant subgroups using the
plan in `research-plan.md`.
