# TFT validation handoff — confirmation completed 2026-09-18

## Resolution — 2026-09-18

The amended long-range confirmation is complete: **2/2 CPU/CUDA CTest entries
passed**, with no skips. Each device passed all three original seeds (1729,
2718, 31415) at the fixed 3,200-update budget using the fresh held-out stream
(`seed XOR 0x2468ACE1`). CPU took 99.52 s, CUDA 295.77 s, and total CTest time
was 395.36 s. Evidence:
`.build/verification/uncertainty-validation/long-range-confirmation-tests.log`.

| Device | Seed | Held-out MAE | Zero baseline | Persistence | Shuffled signal | Attention removed |
| --- | --- | --- | --- | --- | --- | --- |
| CPU | 1729 | 0.00909897 | 0.505271 | 0.692119 | 0.674578 | 0.510468 |
| CPU | 2718 | 0.0143476 | 0.500853 | 0.686045 | 0.663264 | 0.516779 |
| CPU | 31415 | 0.0136344 | 0.491288 | 0.649271 | 0.672812 | 0.490719 |
| CUDA | 1729 | 0.0118515 | 0.478650 | 0.647283 | 0.662639 | 0.480872 |
| CUDA | 2718 | 0.00699976 | 0.496917 | 0.663983 | 0.672954 | 0.525296 |
| CUDA | 31415 | 0.0184682 | 0.505210 | 0.646148 | 0.680412 | 0.505678 |

The initial 16/17 full-suite result and its 800-update CPU failure are preserved
below. Combined with this focused confirmation, there is passing evidence for
all 17 registered suites; no new full 17-suite run was performed. Production
source was unchanged during confirmation, and only the previously amended
long-range test target was rebuilt. This validates the amended synthetic
protocol, not paper benchmark parity or universally stable optimization.

The handoff is resolved. The verification recorder completed and wrote
`.build/verification/uncertainty-validation/source-manifest.json`, checking source
and log hashes, the six preserved model/report pairs, additional CPU seed logs,
and the unchanged legacy checkpoint. See [verification](verification.md) and
[uncertainty validation](uncertainty-validation.md) for the current assessment.

## Historical handoff — 2026-09-17

The following note records the interrupted work and instructions as they stood
on September 17. Its pending status and resume steps are historical; the
completed confirmation above supersedes them.

Stopped at the user's request to conserve tokens. Work is **pending**, not fully
verified. The active confirmation build and its descendants were terminated;
the existing container, checkpoints, logs and source files were preserved.
No background validation job remains from this task.

### Completed and verified

- Added optional `Config::ordered_quantiles` and demo `--ordered-quantiles`:
  an unconstrained central output with outward softplus gaps. The original
  independent head remains the default. Prevents crossings; does not guarantee
  better accuracy or calibrated quantiles.
- Added format-2 checkpoints retaining the output mode, with format-1 loading
  preserving the original independent-head interpretation. Ordered save/resume,
  double precision and dropout continuation tests passed on CPU/CUDA.
- Added `calibration.h` / `calibration.cpp`: separate outward-only split CQR
  intervals, fitted independently per horizon. Nine test groups passed on each
  device, including finite-sample ranks, adjacent floating-point probabilities,
  cancellation/outward rounding, validation and overflow.
- Extended independent forward and gradient checks to the ordered head.
  All numerical/reference checks passed on CPU/CUDA. CPU finite differences
  cover 482 directions across both modes, with maximum error about 1.3e-9.
- Completed six predeclared CUDA uncertainty experiments: three seeds, both
  heads, 800 updates, 1,024 independent calibration windows and 4,096 test
  windows. Results and limitations are in `uncertainty-validation.md`.
  Three undercovered runs reached 79.66–80.11% aggregate coverage after CQR
  toward an 80% target. Overcoverage and individual quantile bias remain.
  The ordered head was less accurate in two of three final comparisons.
- Original 80-step format-1 checkpoint still produces matching scalar and
  per-horizon metrics within 1e-6 and retains SHA-256
  `4dcf540af2825069e5f3d785781d628bc51afe67956433be37eb6cb6a4f306b8`.
  A new ordered CLI checkpoint passed 5+2-step training, reload and evaluation.

### Long-range finding and unfinished confirmation

The original 64-step distant-signal test used three seeds and 800 updates.
Initial full CTest run: **16/17 suites passed**, no skips, 284.18 seconds.
Only CPU seed 1729 failed: held-out MAE 0.504862 versus zero baseline 0.504828;
shuffling its distant signal had no effect. Other two CPU seeds and all three
CUDA seeds passed. Preserve this failure; do not present the initial run as green.

A fixed diagnostic extended the identical CPU run to 3,200 updates, without
changing model, optimizer or data stream. It recovered: held-out MAE 0.009298,
shuffled-signal MAE 0.658684, attention-removed MAE 0.509986. Training started
using the signal between updates 800 and 1,000. This supports delayed
optimization rather than a demonstrated network wiring defect. Small gradients
through the two-variable selection LayerNorm are a hypothesis, not a proven cause.

Three additional CPU seeds, chosen before running, all passed at 3,200 updates:

| Seed | Held-out MAE | Zero baseline | Persistence | Shuffled signal | Attention removed |
| --- | --- | --- | --- | --- | --- |
| 161803 | 0.0211795 | 0.486291 | 0.657677 | 0.642107 | 0.485676 |
| 271828 | 0.0104622 | 0.504309 | 0.632845 | 0.679088 | 0.511732 |
| 141421 | 0.0167194 | 0.491822 | 0.683278 | 0.673957 | 0.506822 |

The current `tools/temporal-fusion-transformer/tests/tft_long_range_test.cpp` has an amended fixed budget of
3,200 updates and a **fresh held-out stream** (`seed XOR 0x2468ACE1`). It retains
all original seeds, thresholds and training settings. Its confirmation build
was interrupted at the user's request. **The amended CPU/CUDA CTest results
are not yet available. Do not claim all 17 suites pass.**

### Resume steps

1. Read this note and `uncertainty-validation.md`. Inspect current logs/source
   before changing anything. Do not rerun completed six-model experiments.
2. Rebuild only the long-range target and run its two CTest entries through the
   existing authoritative launcher:

   ```powershell
   .\tools\temporal-fusion-transformer\environment\container.ps1 -Action exec -Command @(
     'bash', '.temp/uncertainty-validation/confirm-long-range.sh'
   )
   ```

   This rebuilds `tft_long_range_test` and runs `ctest -R '^tft_long_range_'`.
   CPU timeout is 300 seconds, CUDA 600. The script logs separately from the
   initial failure. Build headers across the Windows mount can take minutes.
3. Record every confirmation outcome in `uncertainty-validation.md`, including
   the additional CPU seeds above. Update `verification.md` and README only
   with conclusions supported by completed checks. Keep the initial failure.
   Investigate any new failure without selecting seeds or weakening thresholds.
4. If both amended suites pass, run
   `python3 .temp/uncertainty-validation/record_verification.py` inside the same
   container. It asserts the expected logs, hashes sources/model-report pairs,
   checks the unchanged legacy checkpoint, and writes `source-manifest.json`.
   **This script has been prepared but not executed.** Its all-17 status is
   conditional on those assertions. Inspect it if results or paths change.
5. Review changed source/docs and whitespace, then provide the final scoped
   assessment. Production source already passed the initial run; only the
   long-range test changed afterward, so repeating unrelated suites is unnecessary.

### Artifacts and environment

- Detailed report: `docs/uncertainty-validation.md`; API example:
  `docs/experiments.md`; prior audit: `docs/core-validation.md`.
- Initial build/full test logs: `.build/verification/uncertainty-validation/`.
  `tests.log` is the original 16/17 result; `long-range-initial.cpp` preserves
  the original test. `long-range-diagnostic-3200.log` records recovery.
  `long-range-new-seed-{seed}.log` records the three extra CPU runs.
  `long-range-confirmation-build.log` contains the interrupted build.
- Six model/report pairs:
  `.build/runs/uncertainty-validation/{20260916,20262016,20264016}-{raw,ordered}/`.
  `model.pt` alone does not apply calibration: retain matching `report.json`.
- Diagnostic generator and executable are under
  `.temp/uncertainty-validation/`; original build/experiment/compatibility
  scripts are there too. Both `.temp` and `.build` are ignored local artifacts.
- Existing approved container: `praesidium-humanitatis-tft`, immutable ID
  `54c46d9b14dade26168d872e65113196fcc823d2e14d865162df002eaf9a132c`.
  Reuse it through `tools/temporal-fusion-transformer/environment/container.ps1`; do not create or delete
  Docker objects. Docker access requires sandbox escalation, previously approved.
- Source remains uncommitted/untracked as it was before this work. Do not
  initialize a repository or change ownership. For bounded Git inspection use
  `git -c safe.directory=C:/Work/praesidium-humanitatis ...` when necessary.

The research limits remain: synthetic tests do not establish paper benchmark
parity, performance on real temporal holdouts, universally stable optimization,
or calibrated individual quantiles. CQR coverage depends on exchangeable
calibration/test examples and is marginal per horizon, not simultaneous or
conditional coverage for arbitrary time series.
