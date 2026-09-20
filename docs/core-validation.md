# Core-network validation

This record separates three questions: whether the implementation computes its
specified equations, whether optimization can learn a simple temporal signal,
and whether forecasts are useful on real data. Passing the first two does not
establish the third. The production network was not changed for this initial
audit. A later [uncertainty and distant-history study](uncertainty-validation.md)
extends these checks to an optional ordered head and longer dependencies;
the results below preserve the original audit's scope and measurements.

## Results — 2026-09-17

The checks ran in the existing project container
`54c46d9b14dade26168d872e65113196fcc823d2e14d865162df002eaf9a132c`,
using GCC 12.2, LibTorch 2.6.0+cu124, and the NVIDIA RTX A2000 laptop GPU.
All **13 CTest entries passed** (seven CPU, six CUDA), with no skipped tests,
in 111.54 seconds. This includes the existing model, checkpoint, evaluation,
and CLI continuation suites. Compilation completed without source corrections.

| Check | Observed result |
| --- | --- |
| Independent forward calculation, five configurations | CPU max error `4.44089e-16`; CUDA max error `1.54972e-6` across all five output tensors |
| CPU finite differences | 241 tensor directions passed; max derivative error `1.25128e-9` |
| Gradient connectivity | All 22 top-level network blocks active in the multi-variable test |
| Matched CPU/CUDA forecasts | Max absolute difference `2.38419e-7` |
| Matched CPU/CUDA parameter gradients | Max absolute difference `3.72529e-8`; input-gradient checks also passed |
| Causal and batch derivatives | Forbidden future/sample dependencies exactly zero; permitted current inputs active |
| Evaluation/batch/prefix invariants | Passed on CPU and CUDA |

CPU history-learning results (MAE):

| Seed | Untrained | Trained, held out | Zero baseline | Shuffled history |
| --- | --- | --- | --- | --- |
| 1729 | 0.515369 | 0.0554511 | 0.490188 | 0.661972 |
| 2718 | 0.519129 | 0.0267211 | 0.514568 | 0.653997 |
| 31415 | 0.565003 | 0.0264933 | 0.505992 | 0.614234 |

CUDA history-learning results (MAE):

| Seed | Untrained | Trained, held out | Zero baseline | Shuffled history |
| --- | --- | --- | --- | --- |
| 1729 | 0.547191 | 0.0160063 | 0.522396 | 0.723211 |
| 2718 | 0.532757 | 0.0127823 | 0.519204 | 0.738212 |
| 31415 | 0.524876 | 0.0159236 | 0.485476 | 0.603826 |

All three predeclared seeds passed on both devices. The different CPU/CUDA
learning results use different random data streams; they are not a device
accuracy comparison. Device equivalence is tested separately above.

These observations support strong practical confidence in the tested core
computation and basic trainability. They are not a probability that the entire
implementation is bug-free, nor a forecasting benchmark or calibration result.

## What the additional tests check

### Independent evaluation calculation

`tools/temporal-fusion-transformer/tests/tft_reference_test.cpp` reads the public parameter dictionary and
calculates one sample and time step at a time. It uses primitive affine
operations, population-variance normalization, explicit LSTM gate equations,
and a separate attention calculation that enumerates only permitted keys.
It does not call the production GRN, variable-selection, LSTM, LayerNorm,
embedding, or attention modules to construct expected outputs.

Five fixed configurations cover mixed inputs, continuous-only inputs,
categorical-only inputs, different head counts and quantiles, and a minimal
one-variable/one-step case. Forecasts, all three selection weights, and
attention weights must agree. CPU uses float64 (`atol=rtol=2e-10`); CUDA uses
float32 (`atol=rtol=3e-5`). Configured dropout is nonzero in several cases but
disabled by evaluation mode. Every registered parameter must be consumed.

This is a separately implemented calculation of this library's equations. It
shares LibTorch tensor primitives and the parameter schema, and is not a
matched-weight reproduction of the authors' TensorFlow model.

### Derivatives, isolation, and device agreement

`tools/temporal-fusion-transformer/tests/tft_numerics_test.cpp` adds:

- CPU float64 central finite differences in one deterministic random direction
  for every parameter tensor and each continuous input tensor. Each direction
  is compared with autograd using a smooth scalar projection of the forecasts,
  step `1e-5`, and tolerance `2e-7 + 2e-4 * abs(analytical derivative)`.
- Nonzero aggregate gradient in every top-level network block, using three
  variables per group. Singleton selection networks can legitimately have
  zero gradients; the check intentionally avoids that degeneracy.
- Exact zero derivatives from later future inputs and other batch rows, with
  nonzero sensitivity to the current future input, at every tested horizon.
- Batch permutation and singleton-batch equivalence, horizon-prefix
  consistency, repeatable evaluation with configured dropout, and absence of
  recurrent state carried between independent forward calls.
- Identical-weight, identical-input CPU/CUDA comparison of all output tensors,
  all parameter gradients, and continuous-input gradients. Prediction and
  diagnostic tolerances are `atol=2e-5, rtol=2e-4`; gradient tolerances are
  `atol=3e-5, rtol=2e-3`.

Finite differences validate derivatives of the implemented computation; the
separate evaluation calculation supplies complementary forward evidence. One
direction per tensor is not an exhaustive elementwise Jacobian check. CUDA
tests permit numerical rounding differences and do not promise bitwise
reproducibility across environments.

### Learning that requires historical information

`tools/temporal-fusion-transformer/tests/tft_learning_test.cpp` trains with seeds 1729, 2718, and 31415.
Each run gets 300 fresh batches of 32 windows and an independent held-out set
of 256 windows. History length is six; forecast horizon is three. The target
repeats the last historical signal, sampled uniformly from `[-1,1]`. Static
and future inputs are independent noise and cannot disclose the target.

Before running, acceptance was fixed at median MAE below 0.25 and below half
the zero-forecast baseline for every seed. Reassigning whole histories among
validation examples, while preserving targets/static/future inputs, must
worsen MAE by at least 50%. There is no early stopping or selection of the best
seed. Both CPU and CUDA run all three seeds; their generated data streams differ.

This demonstrates learning on unseen samples and reliance on historical
information. Persistence solves this constructed task exactly. It does not
establish long-range memory, the necessity of attention, calibrated quantiles,
or superiority to persistence.

## Architecture audit and limits

Inspection against the [paper](https://arxiv.org/abs/1912.09363) and
[authors' implementation](https://github.com/google-research/google-research/blob/master/tft/libs/tft_model.py)
found no concrete core wiring defect. The GRNs, four static contexts,
encoder/decoder state transfer, shared-value averaged attention, inclusive
causal mask, and recurrent final residual follow the intended topology.
Known implementation differences, including embeddings and LayerNorm epsilon,
are recorded in [model.md](model.md).

The older sine demo's conditional mean is static amplitude multiplied by a
known future sine input. It can therefore be learned without historical
information. Its successful optimization alone was insufficient evidence for
the temporal path; the new history task closes that specific gap.

Unresolved questions include stochastic-dropout distribution correctness,
very long or extreme-valued sequences, long-range dependency learning,
attention ablations, original benchmark reproduction, and real temporal
holdouts with leakage-controlled preprocessing. The existing saved sine
experiment also has 67.19% coverage for its nominal 80% interval and a 25.98%
quantile-crossing rate. These are known uncertainty-quality limitations, not
evidence that the core forward/backward equations are broken.

## Reproduce

Use the existing provisioned container and authoritative launcher:

```powershell
.\tools\temporal-fusion-transformer\environment\container.ps1 -Action exec -Command @('bash', 'tools/temporal-fusion-transformer/tasks.sh', 'test')
.\tools\temporal-fusion-transformer\environment\container.ps1 -Action exec -Command @('bash', 'tools/temporal-fusion-transformer/tasks.sh', 'test-cuda')
```

These register and run the new checks along with the previous model,
checkpoint, evaluation, and CLI checks. CUDA returning skip code 77 is not
evidence of GPU correctness. Saved local logs for this audit are under
`.build/verification/core-validation/`.
`tests.log` contains every result above; `source-manifest.json` records SHA-256
fingerprints of the tested network, header, model tests, and CMake registration.
