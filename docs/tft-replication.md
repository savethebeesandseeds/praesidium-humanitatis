# TFT replication gate

Establish the forecasting foundation before extending it into query inspection or
application integration. The current evidence supports an independently tested
TFT implementation; it does **not** establish numerical parity with the authors'
implementation or replication of their forecasting benchmarks.

Three claims require different evidence: internal correctness means our equations,
gradients and execution behave as tested; reference parity means equivalent inputs
and weights produce equivalent computations; benchmark replication means the
complete training and evaluation procedure produces comparable held-out results.
Passing one does not establish the others.

## 1. Complete the internal confirmation

**Status: completed 2026-09-18.** Both amended distant-history CPU/CUDA suites
passed all three original seeds on each device at 3,200 updates, using the fresh
holdout. Total test time was 395.36 seconds, with no skips. The
[validation handoff](PENDING-TFT-VALIDATION.md) preserves the original 800-update
failure, the longer diagnostic recovery, and the amended results. Existing
[core validation](core-validation.md) and [uncertainty validation](uncertainty-validation.md)
provide substantial internal evidence, but synthetic success is not benchmark
replication.

The amended checks used their predeclared seeds, thresholds and training budget.
Logs are retained; the original failure is not replaced by the successful amended
protocol. This closes this internal check, not the remaining replication gates.

## 2. Compare directly with the authors' implementation

**Status: in progress; not established.** Use upstream revision
`5b09c22d73a9d35eb6c5d2a99b95677a45053466` for the
[authors' TensorFlow model](https://github.com/google-research/google-research/blob/5b09c22d73a9d35eb6c5d2a99b95677a45053466/tft/libs/tft_model.py).
The inspected model file's SHA-256 is
`53f0046e12b9b79ffea096516b2278774e65d760df684f97600341b9abef9a2c`.
Its [launcher](https://github.com/google-research/google-research/blob/5b09c22d73a9d35eb6c5d2a99b95677a45053466/tft/run.sh)
requires TensorFlow 1.15; provision an isolated compatible reference runtime while
preserving the project's LibTorch environment. Provisioning is isolated under
`.build/reference-runtime`; source is under `.build/reference/tft` with a checksum
manifest. Reference runtime health and parity results must be recorded separately.
Resolve the differences recorded in [the model contract](model.md), and record
any additional differences found during the comparison.

The isolated Python 3.7.16 / TensorFlow 1.15.5 runtime passed dependency consistency
and CPU session checks; its exact packages are recorded in
`.build/verification/replication/reference-freeze.txt`. The first install attempt's
cloudpickle pin conflict was corrected to TensorFlow Probability's required
`1.1.1`, with the failed attempt retained.

The shared-embedding, epsilon, diagnostic and checkpoint changes are implemented.
The focused compatibility/checkpoint suites passed 4/4 CPU/CUDA entries in 24.47
seconds. These results establish compatibility behavior; direct original-model
comparisons remain pending. The executable procedure and evidence locations are
documented in the [replication harness](../tools/temporal-fusion-transformer/replication/README.md).

Build fixtures with identical inputs and mapped weights. With dropout disabled,
compare intermediate activations, forecasts, variable-selection weights, attention,
loss and parameter/input gradients. Explicitly handle feature ordering, tensor
layout and LSTM parameter conventions. Predeclare numeric tolerances by dtype and
device. Verify training defaults separately, including initialization, Adam and
gradient-clipping behavior; a passing forward fixture does not establish a matched
optimization procedure. Include a controlled optimizer-step comparison after
resolving shared weights and LSTM bias parameterization.

Acceptance: every fixture passes, and remaining differences are documented and
either removed from the reference configuration or evaluated separately. An
independent implementation of our own equations is insufficient for this gate.

## 3. Match the Electricity data and evaluation protocol

**Status: Python protocol fixtures, full data preparation, and sampled C++
loader/metric comparison passed.** The seven Python fixtures execute pinned original preprocessing,
formatter, window and metric functions. An initial category-order mismatch was
corrected by retaining the reference formatter's input order; both attempt logs
are preserved. The passing run took 15.739 seconds. Start with the public
[UCI Electricity dataset](https://archive.ics.uci.edu/dataset/321/electricityloaddiagrams20112014),
keeping raw data outside version control and recording attribution, source and
checksums. The
[paper](https://arxiv.org/pdf/1912.09363) uses **168 historical hours to forecast 24
hours**. Its published Electricity scores are **P50 q-risk 0.055 and P90 q-risk
0.027**; these cannot be compared to our current raw summed quantile loss.

Reproduce the [official preprocessing](https://github.com/google-research/google-research/blob/5b09c22d73a9d35eb6c5d2a99b95677a45053466/tft/script_download_data.py)
and [Electricity formatter](https://github.com/google-research/google-research/blob/5b09c22d73a9d35eb6c5d2a99b95677a45053466/tft/data_formatters/electricity.py):
hourly averaging, active-series handling, training-only per-entity scaling,
category mapping, temporal splits and inverse transformation. Preserve the
seven-day history overlap without allowing forecast labels across split boundaries.
The reference validation labels start August 8, 2014; test labels start September
1. Test every complete sliding window, matching the reference's hourly origins.

Metric trap: paper q-risk pools pinball errors and absolute targets across all
horizons. The [reference metric helper](https://github.com/google-research/google-research/blob/5b09c22d73a9d35eb6c5d2a99b95677a45053466/tft/libs/utils.py),
called with DataFrames, calculates horizon-wise ratios that its caller averages.
Fixture-check this behavior in the pinned runtime and report both aggregations.
Score inverse-transformed predictions in original units.

The prepared original data contains 369 entities with contiguous categories
`0..368`; raw client `MT_223` has no activity in the reference's 2014 date range.
The original formatter's training vocabulary also has 369 entries. Derive
embedding cardinality from that vocabulary, rather than the raw dataset's nominal
370 clients. Candidate counts are 1,853,057 train, 204,057 validation and 53,505
test windows; all five seed sample manifests are retained. Dataset metadata SHA-256:
`57766180e172d70821e31c6b9b2c20423ae6e2f13e4fbfa9eac836cd960e64fc`.

The actual C++ loader's first two sampled training windows matched Python exactly
across ten exported tensors. Model-fixture and naive-baseline normalized risks
matched within `3.33e-16` for all three quantiles and both aggregations, against a
predeclared `1e-12` tolerance. This verifies data/metric plumbing, not forecasting
quality. Receipt: `.build/verification/replication/electricity-data-parity-attempt1.json`.

Acceptance: reference and C++ pipelines agree on sampled window identifiers,
features, scaling, targets and metric fixtures. Save manifests so both models use
the same data. Add persistence and seasonal-naive comparisons.

## 4. Reproduce training and compare held-out results

**Status: not run.** Use the published Electricity settings: hidden size 160,
four heads, dropout 0.1, batch 64, learning rate 0.001 and clipping threshold 0.01.
The formatter specifies 450,000 training windows, 50,000 validation windows,
up to 100 epochs and early-stopping patience five. Match validation checkpoint
selection and stopping behavior. Use independent outputs for quantiles
`{0.1, 0.5, 0.9}`; exclude ordered quantiles and CQR from this baseline.

The paired execution uses the original TensorFlow CPU model (four threads) and
the C++ CUDA model (one host thread, TF32 disabled). Numerical fixtures separately
check C++ CPU and CUDA. Hardware/runtime differences will be reported alongside
accuracy; timing does not represent a comparison on identical compute backends.

Both trainers consume the same persisted sample indices and epoch permutations.
For zero-based epoch `e`, use NumPy `RandomState((seed XOR 0x51F7) + e)` to shuffle
ascending sample indices. This is an explicit training-wrapper adaptation to
control batch order across frameworks. TensorFlow retains the original model and
compiled optimizer. Save the checkpoint on every strict validation improvement;
apply early stopping separately with `min_delta=1e-4` and patience five. Start each
pair from the same original Keras-initialized weights, mapped into C++. Dropout
uses each framework's native random generator, so full stochastic trajectories
are compared by held-out outcomes rather than claimed to be identical.

The wrapper explicitly weights validation batch losses by sample count. A native
TensorFlow 1.15 fixture confirmed array evaluation `3.0`, uncorrected generator
evaluation `4.5`, and corrected streaming evaluation `3.0` for a partial final
batch. This avoids changing checkpoint selection when replacing dense arrays
with indexed streaming. Log:
`.build/verification/replication/reference-batch-weighting-attempt1.log`.

Smoke-test trap: [the fixed-parameter script](https://github.com/google-research/google-research/blob/5b09c22d73a9d35eb6c5d2a99b95677a45053466/tft/script_train_fixed_params.py)
currently invokes `use_testing_mode=True`, reducing the model, data and training.
Explicitly disable it. Its default is one repeat, not a multi-seed study.

Engineering acceptance rule frozen on 2026-09-18 before benchmark results:
five runs of each implementation with seeds
`20260918, 20261918, 20262918, 20263918, 20264918`, with C++ mean P50 and P90 no more
than 5% worse than the rerun reference, for both pooled and mean-per-horizon
aggregation. Report paired differences and uncertainty,
every seed including failures, published-score differences, runtime and memory.
This is our similarity criterion, not a statistical proof or an authors' guarantee.
For paired mean differences, report the 95% percentile bootstrap interval from
all `5^5` resamples of the five seed pairs, using linear percentile interpolation.
With only five seeds this interval has limited precision; it is descriptive and
does not change the acceptance rule.
If the reference misses the published scores materially, investigate that gap
before claiming paper replication. Keep the test set out of tuning.

Reusing published settings establishes a narrower result than repeating the paper's
60-trial hyperparameter search. Passing Electricity supports that benchmark only;
it does not reproduce all four datasets or the whole paper. Record the completed
gates and remaining limits before extending the engine.

## Execution order and numerical acceptance

1. Provision and verify the isolated original reference runtime and pinned source.
2. Add opt-in shared embeddings and LayerNorm epsilon while preserving legacy
   defaults and checkpoint loading. Verify legacy behavior and continuation.
3. Compare the original TensorFlow computation with C++ using mapped weights.
   Mixed continuous/categorical fixtures use seeds `1729, 2718, 31415`; include
   repeated category IDs, shared historical/future variables, and a fixture with
   the Electricity model dimensions. Float32 outputs and gradients use
   `atol=2e-5, rtol=2e-4`; controlled optimizer updates use
   `atol=2e-6, rtol=2e-4`. Check native sparse embedding clipping and updates,
   not only a dense-gradient substitute. Retain every failed comparison.
4. Validate preprocessing/window/metric fixtures, then prepare the fixed dataset
   and manifests. Use the same window samples and initial mapped weights for
   paired reference/C++ runs. Test labels are excluded from model selection.
5. Run a development smoke test to validate the trainer and estimate memory/time,
   clearly separate from the full protocol. Execute full runs only after the
   preceding gates pass; record every seed and the validation-selected checkpoint.
6. Assess the frozen criterion, compare with published scores, and publish the
   evidence and remaining limits in this repository before query inspection.

Compatibility code, data preparation, and the reference fixture exporter can be
developed in parallel. Numerical parity and data validation precede benchmark
claims. Any amended protocol must retain the original result and explain the
change; test outcomes cannot be used to silently relax acceptance thresholds.
