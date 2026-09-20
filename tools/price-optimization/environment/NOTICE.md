<!-- SPDX-License-Identifier: MIT -->

# Inherited MIT material and optimizer modifications

Copyright (c) 2026 Waajacu

The following optimizer files were adapted on 2026-09-20 from previously MIT-licensed material in this repository. The original material retains the MIT License. Its complete, unchanged copyright and permission notice is reproduced in [LICENSE-MIT-TFT-LAUNCHER](LICENSE-MIT-TFT-LAUNCHER), copied from `tools/temporal-fusion-transformer/LICENSE`.

| Optimizer file | Original MIT source | Inherited material | New optimizer modifications |
| --- | --- | --- | --- |
| `environment/container.ps1` | `tools/temporal-fusion-transformer/environment/container.ps1` (formerly `code/environment/container.ps1`) | PowerShell action dispatch, Docker invocation, ownership labels and configuration inspection, immutable ID verification, preserving mismatches and reusing stopped containers | Separate optimizer name/tool label, relocated root, CPU configuration, additional configuration checks, image architecture and Debian environment validation |
| `environment/setup.sh` | `tools/temporal-fusion-transformer/environment/setup.sh` (formerly `code/environment/setup.sh`) | Bash setup structure, Debian/architecture/root checks, dependency lock validation, pinned APT installation, installed version checks, profile configuration and package inventory | Relocated root, CPU dependency selection, optimizer profile and inventory paths, AMPL staging defaults and dependency notices |
| `tasks.sh` | `tools/temporal-fusion-transformer/tasks.sh` (formerly `code/tasks.sh`) | Bash task dispatch, setup prerequisite, build concurrency validation, CMake build/test invocation | Optimizer source/build paths, AMPL SDK/runtime configuration and explicit integration/demo tasks |
| `environment/dependencies.lock` | `tools/temporal-fusion-transformer/environment/dependencies.lock` (formerly `code/environment/dependencies.lock`) | Selected package/version entries copied without version changes | Selection of CPU toolchain packages; no new restriction is asserted over package/version facts |

The first three files use `MIT AND LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0`: the inherited portions remain MIT, while new copyrightable optimizer modifications are offered under `tools/price-optimization/LICENSE`. Recipients retain the existing MIT rights to the original material; the Worker Protection License does not retroactively restrict it. Distributing an adapted script requires preserving the applicable notices for both portions. The dependency lock and this notice are MIT.

New optimizer documentation in `environment/README.md` is covered by the Worker Protection License. AMPL, its C++ API, solver binaries and Debian packages retain their separate applicable licenses; they are not relicensed by these notices.
