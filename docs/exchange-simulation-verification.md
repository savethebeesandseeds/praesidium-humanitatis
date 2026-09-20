<!-- SPDX-License-Identifier: MIT -->
# Standalone exchange simulation verification

Verified on 2026-09-20 in the existing approved optimizer Debian 12 container,
ID `5738ae552c575e91e0d4308b8aa2c66aa170e8dd817282fd7abd5817907d3ff9`.
No container, mount, volume, device or published port was added or replaced.

The store application is now
[`projects/post-profit-exchange`](../projects/post-profit-exchange/README.md).
The former empty exchange placeholder, including its zero-byte README, was
preserved as `projects/post-profit-distribution`. The production and allocation
placeholders were preserved. Research framing identifies the store primarily
with exchange, while retaining its distribution and allocation connections.

## Implementation checked

- MIT C++ daily simulator with strict configuration, paired seeded demand,
  procurement, FIFO inventory, sales, spoilage, wages and operating arrears,
  reserve allocation and exact account reconciliation.
- Explicit, separately licensed bounded enumeration backend using the existing
  pricing formulation and shared C++ validation. No automatic AMPL fallback.
- Same source compiled with GCC 12.2 and Emscripten 4.0.15; authoritative defaults
  come from C++, not a duplicate JavaScript calculation.
- Standalone [HTML artifact](../projects/post-profit-exchange/dist/post-profit-exchange.html)
  with embedded WebAssembly, plain controls, SVG charts, ledgers, exports,
  component licenses and downloadable corresponding source. No remote assets.
- Source/build instructions and reproducibility limits in the
  [environment guide](../projects/post-profit-exchange/environment/README.md).

## Results

| Check | Result |
| --- | --- |
| Bounded enumeration and AMPL integration CTest entries | 2/2 passed in 2.38 seconds. Enumeration includes 120 seeded oracle comparisons; all 14 existing AMPL fixtures agree in status/objective with validated enumeration. |
| Final native exchange simulator | 1,216 checks passed; CTest entry passed in 0.07 seconds (0.21 seconds total). |
| Native versus WebAssembly | 14 fixtures agree; deterministic repeated results and exact cash, stock, valuation and equity identities pass. Includes default and 365-day runs, three products, shocks, shortages, no starting resources and invalid configuration. |
| Delivered HTML | Embedded module validates; actual UI worker message handler produces native-equivalent default ledgers and structured failures when executed in an isolated Node worker. Static IDs, script syntax, source-download presence and remote-asset checks pass. |
| Browser visual review | Not performed: Codex Browser Use rejected the local `file://` URL under its browser URL security policy. No alternative browser surface or local-server workaround was used. |

Native CTest output is retained in
`.build/post-profit-exchange/native/Testing/Temporary/LastTest.log`.
Build/runtime artifacts and installed Debian package inventory are under
`.build/post-profit-exchange/`. The compiler SDK is ignored local build data.

Repeat the WebAssembly and artifact checks after building, inside the documented
environment from the repository root:

```sh
node_bin=.build/deps/emsdk-4.0.15/node/22.16.0_64bit/bin/node
"$node_bin" projects/post-profit-exchange/simulation/wasm_test.cjs \
  .build/post-profit-exchange/wasm/exchange-runtime.js \
  .build/post-profit-exchange/native/exchange_simulation
"$node_bin" projects/post-profit-exchange/simulation/standalone_test.cjs \
  projects/post-profit-exchange/dist/post-profit-exchange.html \
  .build/post-profit-exchange/native/exchange_simulation
```

## Evidence boundary

These are implementation and synthetic consistency checks. They do not establish
store viability or an economic advantage from removing owner returns. There is
no empirical demand calibration, matched owner-return baseline, authenticated
worker governance, tax model, live publication, payment integration or hard
real-time guarantee. Each simulation period represents a day. The later
surplus-target/basket objective remains a specification, not implemented behavior.

Forecast surplus uses current replacement costs; realized economic result uses
FIFO inventory costs. Those can have opposite signs after a cost shock, even
without demand error; this distinction is labeled and regression-tested.
The [simulation contract](../projects/post-profit-exchange/simulation/CONTRACT.md)
records these assumptions, supported knobs and exact accounting rules.
