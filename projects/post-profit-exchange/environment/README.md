<!-- SPDX-License-Identifier: MIT -->
# Exchange simulation build environment

The standalone HTML runs locally without a server or installed compiler. Building
it uses the already approved [optimizer Debian container](../../../tools/price-optimization/environment/README.md).
This project creates no container, network listener, volume or additional mount.

The existing container is `praesidium-humanitatis-price-optimization`, immutable
ID `5738ae552c575e91e0d4308b8aa2c66aa170e8dd817282fd7abd5817907d3ff9`.
Its approved Debian 12 image, `/bin/bash` command, repository bind mount,
no ports/volumes/devices and restart policy `no` remain unchanged.

`setup-wasm.sh` installs only dependencies: Debian `python3=3.11.2-1+b1`
with `--no-install-recommends`, and the official Emscripten SDK 4.0.15 into
ignored `.build/deps/emsdk-4.0.15`. It does not change shell startup files.
Transitive Debian package versions are recorded under
`.build/post-profit-exchange/environment/debian-packages.txt`; the pins are
not a complete historical Debian snapshot.

SDK archive: [official 4.0.15 release](https://github.com/emscripten-core/emsdk/releases/tag/4.0.15).
Observed and checked archive SHA-256:
`35be7626493e3bd22860ee2177147f9bca3b6ff871edeab27c5b061a9ed9d23d`.
The pinned SDK manifest resolves to
`sdk-releases-b412b6307e541b93dd93f01b61181e15c17302ec-64bit`, with
Node 22.16.0 and Emscripten compiler commit
`09f52557f0d48b65b8c724853ed8f4e8bf80e669`. The compiler and its bundled
standard libraries keep their licenses; relevant runtime notices are embedded
in the HTML alongside the pricing engine license and downloadable source.

From repository-root PowerShell:

```powershell
.\tools\price-optimization\environment\container.ps1 status
.\tools\price-optimization\environment\container.ps1 -Action exec -Command @('bash','projects/post-profit-exchange/environment/setup-wasm.sh')
.\tools\price-optimization\environment\container.ps1 -Action exec -Command @('bash','projects/post-profit-exchange/build-wasm.sh')
```

The normal launcher checks the existing container identity/configuration before
execution. If absent, follow its documented approval procedure instead of
creating an alternative container. Setup downloads dependencies; runtime
simulation in the finished HTML performs no network requests.

The build uses Emscripten's documented
[single-file packaging](https://emscripten.org/docs/tools_reference/settings_reference.html#single-file)
and [modularized output](https://emscripten.org/docs/compiling/Modularized-Output.html).
`build-wasm.sh` compiles the MIT simulator and reusable MIT exponential-smoothing tool plus the restricted C++ pricing core
and enumeration backend. `package-html.py` embeds that runtime, plain UI, full
license notices and a ZIP of corresponding source into
`dist/post-profit-exchange.html`. AMPL is neither compiled nor redistributed.
