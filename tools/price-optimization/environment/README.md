<!-- SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0 -->

# Price optimization environment

The user approved this definition on 2026-09-20. The container was created and
provisioned with the pinned CPU toolchain; its full verified ID is
`5738ae552c575e91e0d4308b8aa2c66aa170e8dd817282fd7abd5817907d3ff9`.

The launcher, setup and task scripts adapt existing MIT code from the TFT tool. Their inherited portions retain MIT permission, with the complete original notice preserved in [LICENSE-MIT-TFT-LAUNCHER](LICENSE-MIT-TFT-LAUNCHER). New copyrightable optimizer modifications use the [Worker Protection License](../LICENSE). The scripts' composite SPDX expression records both sets of obligations; it does not withdraw MIT rights in inherited code. [NOTICE.md](NOTICE.md) identifies the source files and modifications. The copied dependency pins remain under MIT.

| Setting | Approved value |
| --- | --- |
| Container name | `praesidium-humanitatis-price-optimization` |
| Base image | `debian@sha256:6ebd97fa83deb272194a2cf015b3d26a4d538e9ad3a7a79d544c8af5b0a01443` (Debian 12, Linux amd64) |
| Command | `/bin/bash`, interactive and TTY |
| User | Container root |
| Bind mount | Repository root (currently `C:\Work\praesidium-humanitatis`) to `/workspace/praesidium-humanitatis`, read/write |
| Working directory | `/workspace/praesidium-humanitatis` |
| Named volumes / published ports | None / none |
| Restart policy | `no` |
| GPU / other devices | None / none |
| Network / privileged | Default bridge / false |
| Shared memory | 64 MiB |
| Ownership labels | `org.praesidium-humanitatis.managed-by=praesidium-humanitatis`, `project-root=<absolute repository root>` |
| Configuration labels | `org.praesidium-humanitatis.configuration=1`, `org.praesidium-humanitatis.tool=price-optimization` |

The launcher inspects the exact name before mutation. It reuses an existing matching container, including a stopped container, and operates by verified immutable ID. A container with missing ownership labels or different configuration is preserved and reported. There is no deletion, replacement, rebuild or prune action. Creation verifies the image, container ID, command, mount, ports, devices and basic Debian environment. A failed verification leaves state available for inspection.

From the repository root, `plan` requires no Docker access:

```powershell
.\tools\price-optimization\environment\container.ps1 plan
.\tools\price-optimization\environment\container.ps1 status
```

Lifecycle and dependency installation are separate. The launcher reuses this
approved container on subsequent runs:

```powershell
.\tools\price-optimization\environment\container.ps1 up
.\tools\price-optimization\environment\container.ps1 -Action exec -Command @('bash', 'tools/price-optimization/environment/setup.sh')
.\tools\price-optimization\environment\container.ps1 -Action exec -Command @('bash', 'tools/price-optimization/tasks.sh', 'test')
.\tools\price-optimization\environment\container.ps1 -Action exec -Command @('bash', 'tools/price-optimization/tasks.sh', 'demo')
```

`setup.sh` accepts no arguments and only installs dependencies and configures the environment. It uses `apt-get --no-install-recommends` with explicit CPU build dependency versions copied from the existing TFT lock. These pins cover the selected toolchain, not a complete snapshot of every transitive Debian package. The immutable image and recorded installed package inventory help audit the resulting environment; full bit-for-bit reproducibility needs an approved Debian snapshot/package archive. An unavailable pin fails installation instead of silently choosing a new version. This CPU container has no CUDA or LibTorch dependency. The dependency inventory is written to `.build/price-optimization/environment/debian-packages.txt`.

## AMPL and solver staging

The official API 3.2.0 SDK, AMPL interpreter 20260809, and HiGHS 1.15.1 driver
20260813 are now staged locally. Provenance and observed archive hashes are in
[`ampl-sdk.lock.json`](ampl-sdk.lock.json) and
[`ampl-runtime.lock.json`](ampl-runtime.lock.json). The interpreter reports the
vendor-bundled Demo license, with maintenance through 2027-01-31. No account or
license activation was needed for these small synthetic evaluation cases.
Vendor binaries and notices remain in ignored `.build/deps/`.

The Demo license has model-size and use restrictions. Its presence does not
establish permission for operational store pricing; obtain suitable rights
before that deployment. See [licensing](../../../docs/licensing.md).

AMPL, its C++ API and solver binaries are external dependencies under their own terms. `setup.sh` does not download or activate them. Obtain an authorized Linux amd64 runtime, C++ API and suitable solver, verify the vendor-provided integrity information, and stage them as follows:

```text
.build/deps/ampl/         # AMPL executable, authorized solver and runtime files
.build/deps/amplapi/      # C++ API include/ and lib/ directories
```

Configure the applicable license using AMPL's own instructions; do not commit license files, activation credentials or vendor binaries. No license or binary redistribution rights are supplied by this repository. The default build and tests validate the C++ pricing constraints without AMPL. Running an actual optimization requires the staged API, runtime, solver and a license permitting that use.

Inside the configured container:

```bash
bash tools/price-optimization/tasks.sh test
bash tools/price-optimization/tasks.sh demo
# With authorized dependencies staged:
bash tools/price-optimization/tasks.sh demo-ampl
bash tools/price-optimization/tasks.sh test-ampl
```

`PH_PRICE_WITH_AMPL=ON` enables the API for `configure`, `build` and `test`. `test-ampl` explicitly registers and runs the licensed integration test; regular `test` clears that registration. Override the SDK directory with `AMPLAPI_ROOT` and runtime directory with `PH_AMPL_BINARY_DIR`. `PH_AMPL_SOLVER` must be `highs`: only that driver's status handling is supported in this version. This is configuration, not a bundled solver or a promise that the installed license supports it. Build concurrency defaults to two jobs and can be set with `PRAESIDIUM_HUMANITATIS_BUILD_JOBS`.

The [dashboard backend](../docs/backend.md) runs as a local JSON Lines process
in this same container. It adds no exposed port, mount, service or installed
package. Its vendored JSON header retains MIT terms. `test-ampl` also tests this
backend through actual AMPL success, infeasibility, timeout and cancellation.

Use `container.ps1 shell` for an interactive shell and `container.ps1 stop` to stop this exact managed container. These actions preserve its writable layer and mounted files.
