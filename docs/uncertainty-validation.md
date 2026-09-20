# Uncertainty and distant-history validation

This follow-up addresses crossing quantiles, interval coverage, and the gap
between copying the latest observation and using a distant historical signal.
It adds an optional output head, a separate calibration API, and reproducible
synthetic experiments. Real-data benchmark reproduction is still separate work.

## Ordered quantiles

Set `Config::ordered_quantiles = true`, or pass `--ordered-quantiles` when
starting a fresh `tft_demo` run. The default remains the original independent
linear head. The raw output nearest quantile level 0.5 is unconstrained. Moving
outward, lower outputs subtract softplus gaps and upper outputs add them.
This is differentiable and nondecreasing; finite-precision underflow can produce
ties. It does not sort predictions, and the same pinball loss trains the gaps.

This follows the general nonnegative-increment idea described in
[Park et al., section 3.1](https://proceedings.mlr.press/v151/park22a/park22a.pdf).
It is a small parameterization change, not their complete distribution model
or an exact reproduction of the original TFT head. Preventing crossings does
not establish quantile calibration.

Checkpoint formats 2 and 3 store the output mode. The loader also accepts format 1,
which always retains its original independent-head interpretation. Projection
parameter names and dimensions are unchanged. A resumed run cannot override
its head with a CLI flag. New checkpoints require the updated loader.

## Separate interval calibration

`calibration.h` provides `fit_cqr`, `apply_cqr`, and `evaluate_intervals`.
For every horizon separately, calibration scores are
`max(lower - target, target - upper)`. With N calibration windows and
miscoverage alpha, the correction is the one-based order statistic
`ceil((N + 1) * (1 - alpha))`. Insufficient N is rejected rather than silently
clipping the rank. Corrections are clamped below at zero, so this conservative
variant only expands intervals. Reversed endpoints are rejected.
The rank calculation preserves adjacent floating-point coverage levels using
an FMA residual check. Positive scores and adjusted endpoints round outward
to avoid losing coverage through large-offset cancellation; zero corrections
preserve the original endpoints. Arithmetic overflow is rejected.

This is based on [conformalized quantile regression](https://papers.nips.cc/paper/8613-conformalized-quantile-regression.pdf).
Under exchangeability of calibration/test examples, and a predictor fixed
independently of calibration labels, it gives a marginal coverage lower bound
for each fixed horizon. Clipping negative corrections can overcover; it does
not retain the paper's near-exact upper coverage bound. It does not guarantee
conditional or simultaneous coverage, or coverage under distribution shift.
Dependent overlapping windows from one time series require further methods
and validation. Horizons within one independent window may be dependent and
are not pooled as independent samples.

Calibrated bounds remain separate from the original quantiles and metrics.
They are not substituted into pinball loss or described as calibrated individual
quantiles. The correction must stay associated with the exact unchanged model,
feature preprocessing, horizon meanings, and data distribution.

## Fixed uncertainty experiment

`tft_uncertainty` uses the existing noisy sine generator: static amplitude in
`[0.5,1.5]`, independent random phase, Gaussian observation noise with standard
deviation 0.03, 16 historical steps and four forecast horizons. Both the signal
and its noise distribution are known, allowing a Gaussian oracle comparison.
This task assesses uncertainty behavior; it can be solved from static/future
inputs and does not establish temporal learning.

The comparison fixes these settings before running:

- Seeds 20260916, 20262016, and 20264016, with both raw and ordered heads.
- 800 updates, batches of 32, Adam learning rate 0.003, dropout 0.1, gradient
  clipping at 1.0, hidden width 16, four attention heads, quantiles 0.1/0.5/0.9.
- A predeclared report at update 80 and at the final update. Neither chooses
  the stopping point, seed, parameters, or output mode.
- 1,024 independent calibration windows and 4,096 independent test windows,
  from separate fixed RNG streams; calibration/test labels never train the model.
- Matched raw/ordered runs on one device use identical initial raw parameters,
  training batches, dropout streams, calibration data, and test data.
- CQR target coverage is 80% per horizon. Per-horizon coverage and widths are
  reported, along with raw quantile CDF frequencies, crossings, MAE, pinball
  loss, persistence, and the known Gaussian oracle.

Raw independent-head forecasts with reversed outer endpoints are reported
without CQR. They are not sorted to make them acceptable. Single-run empirical
coverage may differ from nominal coverage; there are 4,096 independent test
windows per horizon, not 16,384 independent errors across all horizons.
Outward-only CQR cannot correct overcoverage by shrinking intervals.

Every experiment reserves a new output directory, containing `model.pt` and
`report.json`; existing experiments are preserved. The report stores calibration
corrections and split seeds alongside the model mode and raw/calibrated metrics.
Reconstruct `CQRCalibration` using the correction vector, miscoverage, and
calibration size, on the prediction device/dtype, only for that saved model.

```powershell
.\tools\temporal-fusion-transformer\environment\container.ps1 -Action exec -Command @(
  '.build/temporal-fusion-transformer/tft_uncertainty', '--device', 'cuda', '--seed', '20260916',
  '--steps', '800', '--ordered-quantiles', '--output-dir', '.build/runs/uncertainty-example'
)
```

## Observed uncertainty results — 2026-09-17

All six predeclared experiments completed on CUDA. The following results use
the separate 4,096-window test set after 800 updates. "Original coverage" uses
the unchanged network outputs; "CQR coverage" uses the separate calibrated
intervals. Loss is pinball summed over the three quantiles, averaged over
examples and horizons.

| Seed | Head | Median MAE | Pinball loss | Original coverage | CQR coverage | CQR mean width |
| --- | --- | --- | --- | --- | --- | --- |
| 20260916 | Independent | 0.03126 | 0.02697 | 84.14% | 84.14% | 0.09034 |
| 20260916 | Ordered | 0.03424 | 0.03153 | 76.62% | 79.66% | 0.10789 |
| 20262016 | Independent | 0.02621 | 0.02535 | 87.00% | 87.00% | 0.10315 |
| 20262016 | Ordered | 0.02543 | 0.02426 | 85.07% | 85.07% | 0.09420 |
| 20264016 | Independent | 0.02692 | 0.02523 | 75.31% | 79.69% | 0.08511 |
| 20264016 | Ordered | 0.03493 | 0.03236 | 70.04% | 80.11% | 0.11018 |

The ordered head prevents crossings but does not consistently improve final
accuracy. Its MAE is 9.5% worse, 3.0% better, and 29.7% worse across the three
matched seeds; its pinball loss is also worse in two of three comparisons.
All six final models have zero observed crossings on these test sets, including
the independent heads. That empirical result does not give the independent
head a structural noncrossing guarantee.

At the predeclared 80-update checkpoint, ordered forecasts have lower MAE and
pinball loss in all three comparisons and zero crossings. Independent-head
crossing rates are 25.54%, 7.18%, and 7.18%. However, the ordered intervals
cover 95.84%, 95.10%, and 92.84% rather than the nominal 80%. More training
substantially improves error while revealing different calibration behavior;
neither crossing prevention nor a small training loss establishes coverage.

The known Gaussian oracle has mean interval width 0.0768931, median MAE
0.02383–0.02397, pinball loss approximately 0.0225, and observed coverage
80.15–80.32% across the three test sets. Final calibrated independent-head
intervals are 17.5%, 34.1%, and 10.7% wider than the oracle; ordered intervals
are 40.3%, 22.5%, and 43.3% wider. The ordered models' MAE remains 6.7–45.7%
above the oracle. A width advantage alone is not evidence of better uncertainty:
an interval can be narrow because it misses too many targets.

For the three runs that undercovered before calibration, CQR moves aggregate
test coverage to 79.66%, 79.69%, and 80.11%. Outward-only CQR leaves the three
overcovering runs unchanged, including the 87.00% result. Its per-horizon
results are:

| Seed | Head | Horizon 1 | Horizon 2 | Horizon 3 | Horizon 4 |
| --- | --- | --- | --- | --- | --- |
| 20260916 | Independent | 83.76% | 84.40% | 83.89% | 84.52% |
| 20260916 | Ordered | 81.91% | 78.86% | 77.76% | 80.13% |
| 20262016 | Independent | 86.16% | 87.74% | 87.23% | 86.87% |
| 20262016 | Ordered | 83.81% | 85.11% | 85.60% | 85.77% |
| 20264016 | Independent | 79.17% | 78.34% | 79.74% | 81.52% |
| 20264016 | Ordered | 80.83% | 78.86% | 79.13% | 81.64% |

Individual horizons still under- or overcover; the lowest observed calibrated
coverage is 77.76%. The finite-sample guarantee averages over calibration/test
draws and does not require every fixed calibration sample's empirical coverage
to equal or exceed 80%. These four horizon measurements also are dependent
within a test window. Three training seeds demonstrate behavior in these
experiments, not universal calibration or superiority of either head.

The empirical CDF at each original quantile is the fraction of targets at or
below that prediction. Its desired values are 10%, 50%, and 90%, respectively:

| Seed | Head | CDF at q0.1 | CDF at q0.5 | CDF at q0.9 |
| --- | --- | --- | --- | --- |
| 20260916 | Independent | 5.24% | 24.61% | 89.39% |
| 20260916 | Ordered | 2.33% | 26.16% | 78.94% |
| 20262016 | Independent | 3.45% | 53.64% | 90.45% |
| 20262016 | Ordered | 6.78% | 50.78% | 91.85% |
| 20264016 | Independent | 11.57% | 40.05% | 86.88% |
| 20264016 | Ordered | 10.86% | 43.51% | 80.90% |

The oracle CDFs are 9.91–10.07%, 49.51–50.46%, and 90.21–90.23%, which
supports the generator and metric calculations. The learned individual
quantiles remain biased. In particular, a reasonable interval coverage can
coexist with a strongly biased median: seed 20260916's independent model
has 84.14% coverage but only 24.61% of targets below its nominal median.
CQR changes interval bounds; it does not calibrate the median or the individual
quantile outputs. These remaining errors are visible rather than hidden by
sorting or by substituting calibrated bounds into raw quantile metrics.

The calibration executable passed **9/9 test groups on CPU and 9/9 on CUDA**.
These check hand-calculated per-horizon order statistics, insufficient sample
sizes, adjacent representable miscoverage levels, outward-only corrections,
large-offset cancellation in float32/float64, separate interval metrics,
an exhaustive exchangeable-rank example with dependent horizons, input
preservation/no autograd/noncontiguous tensors, malformed inputs, and overflow.
The detailed output is in `.build/verification/uncertainty-validation/tests.log`.
The six paired model/report directories are
`.build/runs/uncertainty-validation/{seed}-{raw,ordered}/` for the listed seeds.

## Distant historical signal

The separate `tft_long_range_test` uses the original head and a 64-step history.
The first observation is explicitly marked and determines all three targets;
63 independent distractors follow it. Static and future inputs are independent
noise. The three original seeds are 1729, 2718, and 31415. The initial protocol
used 800 fresh training batches and 512 independent held-out windows, with
Adam at 0.003, hidden width 16, four heads, zero dropout and clipping at 1.0.

Before running, acceptance was fixed at median MAE below 0.25 and half both the
zero and persistence baselines for every seed. Reassigning only the distant
signal among held-out examples must increase MAE by at least 50% and exceed
0.4. Post-training attention-output removal is reported as a diagnostic without
a pass threshold. It is not a separately trained ablation model or proof that
attention is necessary. The task does not establish variable-location retrieval,
generalization to unseen lengths, or realistic forecasting skill.

### Initial result and diagnosis

The initial protocol passed five of six seed/device combinations. It failed
the CPU run at seed 1729 by a large margin:

| Device | Seed | Held-out MAE | Zero baseline | Persistence | Shuffled distant signal | Attention output removed |
| --- | --- | --- | --- | --- | --- | --- |
| CPU | 1729 | 0.504862 | 0.504828 | 0.666596 | 0.504863 | 0.504864 |
| CPU | 2718 | 0.014817 | 0.506341 | 0.673733 | 0.668564 | 0.558048 |
| CPU | 31415 | 0.030479 | 0.481481 | 0.645915 | 0.617018 | 0.481810 |
| CUDA | 1729 | 0.022310 | 0.499016 | 0.674351 | 0.697986 | 0.499308 |
| CUDA | 2718 | 0.022272 | 0.489456 | 0.654445 | 0.655994 | 0.550873 |
| CUDA | 31415 | 0.012160 | 0.504954 | 0.659240 | 0.658660 | 0.504834 |

The failed run was effectively insensitive to the historical signal, despite
finite gradients and correct numerical tests. CPU/CUDA use different random
training data, so success on CUDA does not isolate a device-specific cause.
The original suite failure is retained in `tests.log`, and the original source
in `long-range-initial.cpp` has SHA-256
`426cd2d01df6ab9f4cc142be4991b0554d93ae4f787d5f143df4fb7c62a1c3cd`.

A diagnostic retained the same CPU initialization, optimizer and training
stream and extended the fixed budget to 3,200 updates, chosen before running
the extension. It recovered: MAE became 0.009298, with shuffled-signal MAE
0.658684 and attention-removed MAE 0.509986 on the original held-out set.
Training-batch MAE fell from 0.549 at step 800 to 0.079 at step 1,000; mean
attention mass on the first historical key rose from 0.030 to 0.439. These
minibatch observations diagnose delayed learning; the already-inspected
held-out result is development evidence, not a new confirmation set.

The two-variable selection LayerNorm has small input derivatives when its
normalized logits saturate. The diagnostic observed tiny selection gradients
during the plateau, followed by recovery without an architecture change. This
is a plausible contributing factor, not an isolated causal explanation.
The evidence supports insufficient training budget for that run rather than
a demonstrated forward/backward wiring defect.

### Amended confirmation protocol

After that diagnostic, the test budget was fixed at 3,200 updates before any
confirmation outcomes were inspected. All three original seeds and all
acceptance thresholds are retained. The held-out seed is now
`seed XOR 0x2468ACE1`, distinct from the initial `seed XOR 0x5DEECE66D` stream;
training data and optimizer settings are unchanged. There is no early stopping,
best-seed selection, restart or curriculum. Three additional CPU initialization
seeds (161803, 271828, 141421) were also fixed before their runs, each with the
same 3,200-update budget and acceptance criteria.

This is an amended training protocol motivated by a failure. Its success does
not retroactively make the original 800-update protocol reliable, or establish
robustness over arbitrary initializations and training budgets.

### Confirmation results — 2026-09-18

The interrupted build was resumed through the existing project launcher. Both
amended CTest entries passed: CPU in 99.52 seconds and CUDA in 295.77 seconds,
395.36 seconds total, with no skipped tests. All original seeds and acceptance
thresholds were retained, with 3,200 updates and the fresh held-out stream above.
No production-network changes were made for this confirmation.

| Device | Seed | Held-out MAE | Zero baseline | Persistence | Shuffled distant signal | Attention output removed |
| --- | --- | --- | --- | --- | --- | --- |
| CPU | 1729 | 0.009099 | 0.505271 | 0.692119 | 0.674578 | 0.510468 |
| CPU | 2718 | 0.014348 | 0.500853 | 0.686045 | 0.663264 | 0.516779 |
| CPU | 31415 | 0.013634 | 0.491288 | 0.649271 | 0.672812 | 0.490719 |
| CUDA | 1729 | 0.011852 | 0.478650 | 0.647283 | 0.662639 | 0.480872 |
| CUDA | 2718 | 0.007000 | 0.496917 | 0.663983 | 0.672954 | 0.525296 |
| CUDA | 31415 | 0.018468 | 0.505210 | 0.646148 | 0.680412 | 0.505678 |

The additional CPU initialization checks completed on 2026-09-17 at the same
3,200-update budget, before this confirmation completed. Their retained outcomes
are listed separately; they do not replace any original seed or failed result.

| CPU seed | Held-out MAE | Zero baseline | Persistence | Shuffled distant signal | Attention output removed |
| --- | --- | --- | --- | --- | --- |
| 161803 | 0.021180 | 0.486291 | 0.657677 | 0.642107 | 0.485676 |
| 271828 | 0.010462 | 0.504309 | 0.632845 | 0.679088 | 0.511732 |
| 141421 | 0.016719 | 0.491822 | 0.683278 | 0.673957 | 0.506822 |

The initial 16/17 run plus this focused 2/2 confirmation supplies passing evidence
for all 17 registered suites across separate runs. The original 800-update CPU
failure remains in `tests.log`; the amended build and outcomes are in
`long-range-confirmation-build.log` and `long-range-confirmation-tests.log` under
`.build/verification/uncertainty-validation/`. This demonstrates learning the
fixed-position distant signal under the amended budget. It does not establish
original TensorFlow parity or real-world forecasting skill; those are separate
[replication gates](tft-replication.md).

## Verification artifacts

The regular CPU/CUDA task commands include ordered-head numerical/reference
checks, checkpoint compatibility and continuation, CQR calculation/validation
tests, and the distant-history test. Local build and test logs are in
`.build/verification/uncertainty-validation/`; experiment reports are in
`.build/runs/uncertainty-validation/`.
The verification recorder completed on 2026-09-18 and wrote
`source-manifest.json`, checking the original failure, successful focused
confirmation, additional CPU seed logs, six model/report pairs, and unchanged
legacy checkpoint hash. Source and log hashes are retained with that record.
