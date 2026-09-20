# CUDA development environment

This environment belongs to the MIT Temporal Fusion Transformer tool. The price
optimizer has a [separate CPU container definition](../tools/price-optimization/environment/README.md).

The environment was initially created on 2026-09-16. The user approved a
replacement on 2026-09-17 to remove the original project path from its immutable
mount configuration. The launcher reuses the replacement on subsequent runs.
See `verification.md` for dependency, build, and model-test results.

## Container configuration

| Setting | Value |
| --- | --- |
| Container name | `praesidium-humanitatis-tft` |
| Base image | `debian@sha256:6ebd97fa83deb272194a2cf015b3d26a4d538e9ad3a7a79d544c8af5b0a01443` (Debian 12) |
| Command | `/bin/bash`, interactive with a TTY |
| Bind mount | `C:\Work\praesidium-humanitatis` to `/workspace/praesidium-humanitatis`, read/write |
| Working directory | `/workspace/praesidium-humanitatis` |
| Named volumes | None |
| Published ports | None |
| Restart policy | `no` |
| GPU/device access | All NVIDIA GPUs; no additional devices |
| Shared memory | 1 GiB |
| Network | Docker's default bridge |
| Privileged mode | Disabled |

`container.ps1 plan` prints the actual host project path. The launcher derives it
from its own location and checks the full immutable container ID, image ID,
ownership/configuration labels, bind mount, command, GPU request, ports, and other
settings before operating on an existing container. A stopped matching container
is reused. An unmanaged or mismatched same-named container is preserved and
rejected. No launcher action deletes or rebuilds a container, image, or volume.

### Path migration and preserved backup

The active container is
`54c46d9b14dade26168d872e65113196fcc823d2e14d865162df002eaf9a132c`.
Its mount, working directory and ownership labels all use the new project name,
exactly as shown in the table. It does not require the old host junction or an
internal compatibility link. The launcher no longer accepts the legacy mount
configuration. Its shell profile is `/etc/profile.d/praesidium-humanitatis.sh`.

The original container remains stopped as
`praesidium-humanitatis-tft-before-path-migration`, ID
`fb76bcc496b41c383fd4363cb5c2af15a877076beb4254d2af957dab6eba4b24`.
Its installed environment is preserved. Do not start that archived container:
its immutable bind source still names the old path and could recreate it.
No Docker container, image or volume was deleted during the migration.

Use `C:\Work\praesidium-humanitatis` for all editor sessions and commands.
At migration verification time, the old host junction remained locked by Codex
helper processes (Windows sharing violation 32). It has **not been removed**;
no background cleanup is scheduled. Close Codex normally and run this one-off
command from a separate PowerShell window:

```powershell
powershell -NoProfile -File "C:\Work\praesidium-humanitatis\.temp\finish-rename\complete-cleanup.ps1"
```

The local helper checks the exact junction target, repository ownership, Git
identity and saved checkpoint before nonrecursive removal. It then restarts
only the verified replacement container and confirms the old path stays absent.
No process is terminated and no Docker object is deleted. Inspect
`.build/verification/path-migration/junction-cleanup.json` for the removal
outcome and `verified.json` for `hostJunctionRemoved` and
`postRemovalRestartVerified`; `completion.log` is written only after all final
checks pass. These scripts and records are local migration artifacts.

## Dependency provenance

The base image, CUDA installation procedure and package versions come from the
inspected local reference `C:\Work\cuwacunu\cuwacunu_embedding` at commit
`ded843a`. The reference includes `setup.sh`, `container.ps1`, `dependencies.lock`
and `README.md`. It uses a pinned Debian 12 base, CUDA toolkit 12.4.1, and system
cuDNN 9.26.0.51. This project retains that reference's package list, including
its development/profiling tools; it is a development environment, not a minimal
runtime image. CMake is added for this project's build:
[cmake 3.25.1-1](https://packages.debian.org/bookworm/cmake) and
[cmake-data 3.25.1-1](https://packages.debian.org/bookworm/cmake-data).

`setup.sh` only installs dependencies and configures/verifies the environment.
All APT installations use `--no-install-recommends`. Exact package versions are
requested and verified; missing versions fail instead of silently choosing newer
ones. Debian/NVIDIA repositories are live, so the lock is not an offline archive
or a guarantee that those packages remain downloadable. Base-image packages and
additional transitive dependencies are recorded in `.build/environment/debian-packages.txt`.
Package upgrades should be a reviewed lock update, followed by model validation.

## Stage LibTorch

The required Linux x86-64 bundle is LibTorch `2.6.0+cu124`, C++11 ABI. Stage an
independent copy of the existing bundle at `.build/deps/libtorch`; the source is
`C:\Work\cuwacunu\cuwacunu_embedding\.external\libtorch`. No host-wide library
installation is needed. Preserve the bundle's license files when copying or
redistributing it. Do not replace another existing staged bundle without review.

The installer checks `build-version`, the C++11 ABI flag and required library
files before installing packages. LibTorch's bundled CUDA/cuDNN runtime libraries
take priority over system libraries through `/etc/profile.d/praesidium-humanitatis.sh`.
CUDA stub libraries must never be placed on the runtime library path.

## Provision and verify

Run these commands from the project root in PowerShell, after staging LibTorch:

```powershell
.\tools\temporal-fusion-transformer\environment\container.ps1 plan
.\tools\temporal-fusion-transformer\environment\container.ps1 up
.\tools\temporal-fusion-transformer\environment\container.ps1 -Action exec -Command @('bash', 'tools/temporal-fusion-transformer/environment/setup.sh')
.\tools\temporal-fusion-transformer\environment\container.ps1 -Action exec -Command @('bash', 'tools/temporal-fusion-transformer/tasks.sh', 'test')
.\tools\temporal-fusion-transformer\environment\container.ps1 -Action exec -Command @('bash', 'tools/temporal-fusion-transformer/tasks.sh', 'test-cuda')
.\tools\temporal-fusion-transformer\environment\container.ps1 -Action exec -Command @('bash', 'tools/temporal-fusion-transformer/tasks.sh', 'demo', '--device', 'cuda', '--steps', '80')
```

Docker Desktop must be using Linux containers with NVIDIA GPU support. Setup
verifies the package pins, `ldd` resolution, `nvcc`, CMake, and visible GPUs;
model CPU/CUDA behavior is verified separately by the tests. On an installation
or verification failure, preserve the container and inspect the error.

The task runner uses `cmake -S tools/temporal-fusion-transformer -B .build/temporal-fusion-transformer` and
`-DCMAKE_PREFIX_PATH=/workspace/praesidium-humanitatis/.build/deps/libtorch`.
`test-cuda` enables `PRAESIDIUM_HUMANITATIS_CUDA_TESTS=ON` and runs all tests ending in `_cuda`;
`test` runs all tests ending in `_cpu`, including the CLI continuation workflow.
Build parallelism defaults to two jobs; override with the
positive integer environment variable `PRAESIDIUM_HUMANITATIS_BUILD_JOBS` when memory permits.
Build products stay under `.build/`; setup downloads stay under `.temp/`.
The first build can spend several minutes reading LibTorch's headers through the
Windows bind mount. A quiet compiler log during that stage does not necessarily
mean compilation has stalled; inspect compiler activity before interrupting it.

Other lifecycle commands are `status`, `shell`, and `stop`. Arbitrary `exec`
commands do not automatically source shell profiles; `tools/temporal-fusion-transformer/tasks.sh` does.
For direct CUDA tooling use `/usr/local/cuda-12.4/bin/nvcc` or an interactive shell.
`setup.sh` deliberately accepts no action arguments and performs no build,
training, testing, or container lifecycle operations.

## Isolated original TFT reference

The replication harness uses `setup-reference.sh` in the same approved container.
It installs Python 3.7.16 and the pinned TensorFlow 1.15.5 CPU reference under
`.build/reference-runtime`, without changing system Python, LibTorch or container
lifecycle. The Miniconda installer is checksum-pinned; dependency versions are in
`tools/temporal-fusion-transformer/environment/reference-requirements.txt`. No conda channel operations or shell
initialization are used. Download caches remain under `.temp/reference-runtime`.

```powershell
.\tools\temporal-fusion-transformer\environment\container.ps1 -Action exec -Command @('bash', 'tools/temporal-fusion-transformer/environment/setup-reference.sh')
```

Setup checks dependency consistency and executes a small TensorFlow session.
The resolved package list is `.build/verification/replication/reference-freeze.txt`.
Model comparison, data preparation, and training are separate operations described
in the [replication harness](../tools/temporal-fusion-transformer/replication/README.md). The original reference
runs on CPU; the C++ comparison additionally runs on CUDA with TF32 disabled.
