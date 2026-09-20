# Original TFT replication harness

This directory implements the [replication plan](../../../docs/tft-replication.md).
It compares the C++ model against the authors' pinned TensorFlow implementation;
it does not substitute the project's independent-equation tests for that reference.
Implementation and execution are in progress. A script's existence is not evidence
that its checks passed; consult the verification record and retained run logs.

Run from the repository root inside the existing project container. From Windows,
use `tools/temporal-fusion-transformer/environment/container.ps1 -Action exec -Command @(...)`. No step below
creates or replaces a container.

## Runtime and numerical checks

```bash
bash tools/temporal-fusion-transformer/environment/setup-reference.sh
.build/reference-runtime/bin/python tools/temporal-fusion-transformer/replication/fetch_reference.py
bash tools/temporal-fusion-transformer/replication/verify.sh .build/verification/replication/check-001
```

The isolated Python 3.7 / TensorFlow 1.15 runtime leaves LibTorch dependencies
unchanged. The source downloader checks immutable revision hashes. Verification
uses an exclusive output directory: choose a new path after a failed attempt.
Logs and source/runtime fingerprints remain available for every attempt.

Mixed fixtures cover shared continuous/categorical embeddings, repeated IDs,
nontrivial normalization parameters and changing categories across three Adam
updates. An Electricity fixture covers the published model dimensions. Float32
outputs/gradients use `atol=2e-5, rtol=2e-4`; updates use `atol=2e-6, rtol=2e-4`.
Both C++ CPU and CUDA are compared with the original CPU TensorFlow reference.
Dropout is disabled for numerical comparison. Optimizer probes distinguish native
sparse clipping and momentum behavior from a dense-gradient approximation.

`reference_optimizer.h` is specific to this study. It implements per-variable
clipping, TensorFlow's Adam epsilon placement, and occurrence-gradient norms for
embedding tables. The default library/synthetic trainer continues using LibTorch
Adam. Reference-compatible LSTMs freeze the redundant Torch recurrent bias.
`reference_loss.h` additionally matches the original positive/negative-part
pinball expression, including float32 coefficients and the exact-zero
subgradient. These conventions are tested directly against TensorFlow.

## Data and benchmark

```bash
.build/reference-runtime/bin/python tools/temporal-fusion-transformer/replication/prepare_electricity.py \
  --reference-root .build/reference/tft --data-dir .build/datasets/electricity \
  --output .build/replication/electricity --download
.build/reference-runtime/bin/python tools/temporal-fusion-transformer/replication/prepare_electricity.py \
  --output .build/replication/electricity --verify-only
```

Raw UCI data, preprocessing provenance, tensor hashes and sampled windows are
retained under ignored build directories. `electricity_protocol.json` freezes
splits, samples, seeds, architecture, metrics and acceptance before results.
Raw rows are stored once; compact window indices let both trainers consume the
same examples without materializing all overlapping windows in memory.

For each benchmark seed, export an Electricity fixture using
`export_reference.py --preset electricity --initialization keras --seed SEED
--static-cardinality 369` for the verified export, whose original preprocessing
retains 369 entities. Derive this value from the prepared category vocabulary;
the standalone architecture fixture keeps its nominal 370-category default.
Its `original_tf_weights.npz` initializes the original model, and its mapped
`parameter` / `frozen_parameter` tensors initialize C++. Fixture updates never
replace these initial weights. Shared epoch orders come from
`train_reference.py --prepare-orders-only`.

`train_reference.py` streams the shared windows through the unchanged original
model and compiled optimizer. Explicit smoke/profile modes validate operation and
estimate runtime before full training. Development scoring uses validation data;
test scoring belongs to the completed full protocol. Each run has a new output
directory, progress records, the best validation checkpoint and a final report.
Full comparison requires all five paired seeds and both P50/P90 normalized risks.
Report both pooled and mean-per-horizon aggregation because they differ.
