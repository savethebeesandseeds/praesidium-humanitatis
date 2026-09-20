# Verification record

Updated: 2026-09-18. The original C++ build, model/checkpoint/evaluation suites on
CPU and CUDA, CPU command-line continuation workflow, and saved CUDA experiment
passed. The later uncertainty and distant-history validation is recorded in
[its separate report](uncertainty-validation.md), including an initial
long-range learning failure and the completed amended confirmation. No runtime
tests were skipped.

The subsequent [original-reference replication work](tft-replication.md) adds
opt-in shared embeddings, configurable LayerNorm epsilon, diagnostic tensors,
and format-3 checkpoints. Its focused compatibility/checkpoint run passed **4/4
CPU/CUDA CTest entries** in 24.47 seconds with no skips. This covers shared
gradients and single optimizer updates, legacy defaults, format-1/2 migration,
format-3 continuation, and CPU/CUDA checkpoint portability. Log:
`.build/verification/replication/compatibility-tests.log`. Full regression and
original TensorFlow comparisons for this changed source are still in progress;
the historical results below do not substitute for those checks.
The preserved 80-update format-1 experiment also reproduced every scalar and
per-horizon CUDA evaluation metric within `1e-6` with the new loader, retaining
SHA-256 `4dcf540af2825069e5f3d785781d628bc51afe67956433be37eb6cb6a4f306b8`.
Current evaluation: `.build/verification/replication/legacy-evaluation.json`.

The subsequent [core-network audit](core-validation.md) adds an independent
forward calculation, finite-difference derivatives, causal/batch Jacobians,
matched CPU/CUDA outputs and gradients, and three-seed held-out learning that
requires history. All **13 expanded CTest entries passed** on 2026-09-17,
with no skips (111.54 s). No production-network changes were needed.

The next study adds an optional noncrossing head, backward-compatible format-2
checkpoints, and per-horizon outward-only conformal interval calibration. Its
initial expanded run passed **16/17 CTest entries** (284.18 s): all numerical,
reference, original learning, model, checkpoint, evaluation, calibration, and
CLI checks passed, but one of three CPU distant-history seeds failed at the
predeclared 800-update budget. All three CUDA distant-history seeds passed.
The first-run log is preserved as
`.build/verification/uncertainty-validation/tests.log`; subsequent diagnosis
and the amended protocol are reported separately rather than replacing it.

On **2026-09-18**, the amended long-range confirmation passed **2/2 CPU/CUDA
CTest entries**, with all three original seeds passing on each device at the
fixed 3,200-update budget and with a fresh held-out stream. CPU took 99.52 s,
CUDA 295.77 s, and total CTest time was 395.36 s. Held-out MAE ranged from
0.009099 to 0.014348 on CPU and 0.007000 to 0.018468 on CUDA; shuffling the
distant signal or removing attention degraded every run. The log is
`.build/verification/uncertainty-validation/long-range-confirmation-tests.log`.
The earlier 800-update failure remains part of the evidence.

The initial full run plus this focused confirmation provide passing evidence
for **all 17 registered suites**. This is not a new full 17-suite run. Production
source was unchanged during confirmation; only the previously amended
long-range test target was rebuilt. The [validation handoff](PENDING-TFT-VALIDATION.md)
is resolved. The verification recorder completed, checking all six preserved
model/report pairs, the three additional CPU seed logs, and the unchanged legacy
checkpoint hash. Source and log hashes are saved in
`.build/verification/uncertainty-validation/source-manifest.json`.

All six fixed uncertainty experiments completed. Three initially undercovered
runs reached 79.66–80.11% aggregate test coverage after calibration toward an
80% target. Outward-only calibration deliberately retains overcoverage, and
individual quantile bias remains. The ordered head was less accurate at 800
updates in two of three paired seeds and therefore remains optional.
The original format-1 checkpoint retained its SHA-256 and reproduced all
scalar/per-horizon metrics within 1e-6. An ordered CLI checkpoint also survived
5+2-step training, resume, and evaluation, with exactly matching saved metrics.

Original verification used container `ad-humanitatem-tft`, renamed on 2026-09-17
to `praesidium-humanitatis-tft` with the same immutable ID:
`fb76bcc496b41c383fd4363cb5c2af15a877076beb4254d2af957dab6eba4b24`.
The launcher verified the approved image, command, ownership labels, mount,
GPU request, ports, and restart policy after creation. No named volumes are used.
The inspected configuration is recorded locally in
`.build/verification/container.json`. A second `up` reused the same immutable ID.
That container was left running after the initial verification. The later path
migration preserves it as a stopped backup; see `environment.md` for the current
container identity and configuration.

The original names below record the pre-rename builds and experiment provenance.
Current source, public headers, CMake targets and paths use
`praesidium-humanitatis` (or `praesidium_humanitatis` for C++ identifiers).

## Verified environment

| Component | Version / value |
| --- | --- |
| Base | Debian 12 amd64, pinned image digest in `environment.md` |
| Compiler | GCC 12.2.0, C++17, Release build |
| CMake | 3.25.1 |
| LibTorch | 2.6.0+cu124, Linux C++11 ABI |
| CUDA toolkit | 12.4.1, nvcc 12.4.131 |
| System cuDNN | 9.26.0.51; LibTorch's bundled runtime has loader precedence |
| GPU | NVIDIA RTX A2000 8GB Laptop GPU, compute capability 8.6 |
| Host NVIDIA driver | 596.52 |

Setup installed all exact package pins, passed `dpkg --audit`, and verified that
LibTorch's core shared libraries resolve without missing dependencies. The full
installed package inventory is `.build/environment/debian-packages.txt`.
Staged build metadata and core CPU/CUDA library SHA-256 hashes match the supplied
reference; those hashes are in `.build/verification/libtorch-checksums.json`.

## Completed checks

- Inspected the original repository: existing `main` branch at `57e71bf`, with
  only the license before this work. Workspace and `.git` both belong to
  `waajacu\santi`; no repository recreation or global Git trust change occurred.
- Located and inspected the provided environment reference at its current path,
  `C:\Work\cuwacunu\cuwacunu_embedding`, including setup, launcher, lock, README,
  container mounts, image identity, and Git history.
- Confirmed a locally available pinned Debian 12 base image and staged an
  independent Linux LibTorch `2.6.0+cu124` bundle under `.build/deps/libtorch`.
- PowerShell launcher parses and its read-only `plan` prints the expected
  configuration. Mock Docker responses exercise creation, reuse of running and
  stopped containers, immutable-ID targeting, and preservation on configuration
  conflicts. Real creation and both running/stopped-container reuse passed with
  the same immutable ID. Conflict handling was checked with mocks.
- Bash syntax checks pass for setup and task runner. Shell scripts use LF endings.
- Dependency lock preserves all 321 reference pins and adds CMake and cmake-data
  `3.25.1-1`. All new source files pass whitespace and conflict-marker checks;
  `git diff --check` also passes for tracked changes.

## Initial foundation results — 2026-09-16

The library `adht_tft`, test executable `tft_test`, and example `tft_demo` compiled
and linked successfully. No model-source corrections were required by this run.

| Check | Result |
| --- | --- |
| CPU suite | 11/11 groups passed; CTest 7.54 s |
| CUDA suite | 11/11 groups passed on `cuda:0`; CTest 9.29 s |
| CPU fixed-batch optimization | Quantile loss 0.610882 to 0.0153901 |
| CUDA fixed-batch optimization | Quantile loss 1.11556 to 0.0314337 |
| CPU synthetic demo, 80 updates | Validation loss 1.15612 to 0.116203 |
| CUDA synthetic demo, 80 updates | Validation loss 1.09804 to 0.0708954 |

The C++ test executable contains 11 groups for shapes and normalized diagnostics,
causality, shared-value attention/head permutation/uniform causal attention,
continuous/categorical-only input variants, invalid configuration and
input rejection, hand-computed pinball loss/gradients, registered-parameter
backward propagation, weight serialization, float64 execution, and fixed-batch
training. CUDA was available and all test groups actually executed. CPU and CUDA
random-number streams differ, so these runs are separate correctness/training
checks, not a numerical device-equivalence comparison or a speed benchmark.

The demo uses freshly generated training batches and a separate fixed synthetic
validation set. Its loss sums the three quantiles and averages samples/horizons.
Its improvement establishes learning on that constructed task, not accuracy on
human outcomes.

## Checkpoint and evaluation improvements — 2026-09-16–17

The follow-up build adds resumable format-v1 checkpoints, forecast diagnostics,
a persistence baseline, JSON reports, and a CLI for save/resume/evaluation.
The interrupted build completed; the stopped container was reused on September
17 to finish verification. Its identity and dependencies were preserved.

| Check | CPU | CUDA |
| --- | --- | --- |
| Original model suite | 11/11 groups; 4.59 s | 11/11 groups; 13.97 s |
| Checkpoint suite | 6/6 groups; 3.52 s | 7/7 groups; 13.59 s |
| Evaluation suite | 7/7 groups; 2.21 s | 7/7 groups; 4.02 s |
| CLI workflow | Passed; 25.92 s | Saved experiment/reload checked separately |

All four CPU CTest entries and all three CUDA CTest entries passed. Checkpoint
checks cover complete configuration/dtype, model outputs, Adam groups/options/
moments, malformed archives, overwrite preservation, and dropout continuation.
The extra CUDA group moves checkpoints in both directions between CPU and CUDA,
compares predictions within tolerance, and performs further optimizer updates.

The CPU CLI workflow verifies that 12 uninterrupted updates produce exactly the
same JSON metrics as 5 updates followed by 7 resumed updates, and as loading that
checkpoint for evaluation only. It exercises periodic saving, rejects conflicting
arguments, checks that invalid commands preserve the checkpoint, and tests
case-insensitive report/checkpoint path collisions on the Windows-backed mount.

## Saved synthetic experiment — 2026-09-17

Generator: `adht.synthetic.sine.v1`; seed 20260916; CUDA float32; 80 updates;
32 samples per training batch; 128 fixed validation windows; history 16;
horizon 4; quantiles 0.1/0.5/0.9. The new per-update seed protocol differs from
the initial demo, so its results should not be compared as matched-data runs.

| Validation metric | TFT after training | Persistence |
| --- | --- | --- |
| Summed quantile loss | 0.0738911 | 0.554652 |
| Median MAE | 0.0674646 | 0.369768 |
| Outer-interval coverage | 67.1875% | 0% (point forecast) |
| Mean signed interval width | 0.265169 | 0 |
| Any-adjacent-quantile crossing rate | 25.9766% | 0% |

Initial TFT loss was 1.09582. Error improved on this constructed task, but the
nominal 80% interval covers only about 67% of validation targets, and about 26%
of forecasts contain a crossing. These are unresolved uncertainty-quality
findings, not hidden by sorting predictions. The experiment does not establish
calibration, paper benchmark parity, or real-world benefit.

Artifacts are `.build/runs/2026-09-17-sine/model.pt`, `training.json`, and
`evaluation.json`. Reloading the CUDA checkpoint for evaluation reproduced all
scalar metrics within 1e-6, retained 80 completed steps, and left the checkpoint's
SHA-256 unchanged. Reports also include per-horizon errors and coverage.

## Project rename verification (2026-09-17)

The real project folder is now `C:\Work\praesidium-humanitatis`, and `origin`
points to `https://github.com/savethebeesandseeds/praesidium-humanitatis`.
The remote HEAD remained `57e71bfc479230aa95791872bb9ee8cc8df31fd1`.
The folder and complete `.git` tree retain their original Windows owner;
ordinary Git status succeeds under that account without a global ownership
exception. This initial rename left the old host path as a compatibility
junction. The subsequent approved path migration removes that dependency.

The container was renamed in place to `praesidium-humanitatis-tft`. Comparing
the before/after inspection records verified the same immutable ID, image,
mounts, labels, command, ports, restart policy and GPU requests. The runtime
profile uses the new name. No Docker objects or training artifacts were deleted.
Prior build outputs remain under `.build/tft-before-rename`.

A fresh build in `.build/tft` compiled the renamed public headers, namespace,
library and every executable. All **four CPU and three CUDA CTest entries
passed**, with no skips (47.02 s CPU; 43.35 s CUDA). Launcher mocks also passed
canonical creation/reuse and exact-ID legacy reuse, plus refusal of 20
configuration mismatches, four invalid host junctions and nine container-link
failures, including case-sensitive Linux path checks.

The original 80-update CUDA checkpoint evaluated successfully with the renamed
binary. Scalar and per-horizon metrics matched the previous report within 1e-6.
Adam training resumed to step 81 in a separate checkpoint; the original file
retained SHA-256
`4dcf540af2825069e5f3d785781d628bc51afe67956433be37eb6cb6a4f306b8`.
Legacy reports retain `adht.synthetic.sine.v1`, while fresh and resumed runs
from the new CPU workflow use `praesidium-humanitatis.synthetic.sine.v1`.
This is compatibility verification, not a new forecasting benchmark.

Rename records are under `.build/verification/rename/`: `build-cuda.log`,
`cpu-tests.log`, `launcher-checks.log`, `compatibility-checks.log`, before/after
container inspections, ownership records, and legacy evaluation/resume reports.

## Canonical container migration (2026-09-17)

After the user approved the cleanup plan, the original container was stopped
and preserved as `praesidium-humanitatis-tft-before-path-migration`. Its ID is
`fb76bcc496b41c383fd4363cb5c2af15a877076beb4254d2af957dab6eba4b24`.
The replacement `praesidium-humanitatis-tft` has ID
`54c46d9b14dade26168d872e65113196fcc823d2e14d865162df002eaf9a132c`.
It uses the authoritative launcher configuration: the canonical host bind and
container workdir, the same pinned Debian image and dependencies, and NVIDIA
GPU access. The old-path exception was removed from the launcher and task
runner. No Docker object was deleted.

The existing setup script completed successfully, and comparing complete
installed package inventories found **zero differences**. A fresh rebuild in
the replacement passed **all four CPU tests (48.70 s) and all three CUDA tests
(42.84 s)** without skips. The canonical launcher passed 45 mock checks.
The saved 80-update checkpoint retained its SHA-256 above; scalar and
per-horizon evaluation metrics matched the saved report within 1e-6.

Stopping and starting through the launcher reused the replacement's immutable
ID. The canonical bind, shell profile and GPU remained healthy; the archived
container stayed stopped. The root and all 213 inspected `.git` entries retained
the original owner's SID. Ordinary Git status worked under that Windows user,
with unchanged HEAD and renamed origin. Source files remain uncommitted.

The final host-path cleanup is **pending**, not completed: the attempted guarded
removal returned Windows error 32 because Codex helpers still hold the old
junction. No background helper is running. Follow the external PowerShell step
in `environment.md` after closing Codex normally. That step removes only the
verified junction, then checks the repository and checkpoint again and verifies
that restarting the replacement does not recreate the old path.

Evidence is in `.build/verification/path-migration/`: setup, CPU/CUDA and
runtime logs; before/after container inspections and package inventories;
legacy evaluation; launcher/restart checks; `verified.json`; and
`junction-cleanup.json`. The manifest's `verified` flag covers the completed
runtime migration; final cleanup is recorded separately by `hostJunctionRemoved`
and `postRemovalRestartVerified`.

## Local logs and rerunning

- `.build/verification/setup.log`: package installation and environment checks.
- `.build/verification/build-cpu.log`: configuration, full build, CPU CTest.
- `.build/verification/cpu-tests.log`: all CPU group results.
- `.build/verification/cuda-tests.log` and `cuda-test-details.log`: CUDA CTest
  invocation and all GPU group results.
- `.build/verification/demo-cpu.log` and `demo-cuda.log`: training/validation losses.
- `.build/verification/improvements/build-cuda.log`, `cuda-details.log`:
  follow-up build and GPU suite results.
- `.build/verification/improvements/build-cpu.log`, `cpu-tests.log`, and
  `cpu-details.log`: completed build and the resumed CPU verification.
- `.build/verification/improvements/demo-cuda.log` and
  `demo-cuda-evaluation.log`: saved GPU experiment and reload verification.

Use the commands in `environment.md` to repeat these checks. Logs and binaries
are ignored generated artifacts; this document preserves the results in source.
Upstream LibTorch CMake reports a failed NVRTC short-hash calculation, an NVTX3
fallback, and disabled optional direct integrations including `USE_CUDNN=0`.
They did not prevent configuration, linking, or CUDA execution. These tests
verify LibTorch CUDA behavior, not which underlying GPU kernel library it uses.
The first build spent several minutes reading headers across the Windows mount.

These checks do not establish benchmark parity, calibrated uncertainty, or
real-world benefit; those remain separate research milestones.
