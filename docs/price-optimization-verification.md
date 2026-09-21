# Price optimization verification — 2026-09-20

Historical v1 evidence. The current `ph.price.v3` / `public-prices.v3` feedback
and liquidity contract supersedes the objective described below. All counts,
hashes and scope statements in this record describe that earlier snapshot;
see the [current exchange verification](exchange-simulation-verification.md)
and [current contract](../tools/price-optimization/docs/contract.md).

The later dashboard boundary has its own
[integration verification record](optimizer-integration-verification.md),
including JSON contracts, process deadlines/cancellation and real AMPL checks.
The original engine evidence below is preserved.

Initial offline and SDK compilation checks used the existing Debian 12 TFT
development container with GCC 12.2 and CMake 3.25.1. After the user approved
provisioning, the separate optimizer container was created on 2026-09-20 with
its documented pinned CPU toolchain. Its verified immutable ID is
`5738ae552c575e91e0d4308b8aa2c66aa170e8dd817282fd7abd5817907d3ff9`.
The launcher was run again and reused that same container. The recorded
configuration is `.build/verification/price-optimization/container.json`;
the installed package inventory is
`.build/price-optimization/environment/debian-packages.txt`.

## Offline core

The repository-root CMake build in `.build/core` uses the price engine without
Torch or AMPL. Both CTest entries passed: `price_core` (116 assertions) and
`price_guardrail_demo`. The saved local log is
`.build/core/Testing/Temporary/LastTest.log`.

The approved optimizer container then built the tool independently in
`.build/price-optimization` and also passed both offline CTest entries, including
all 116 core assertions. That log was preserved as
`.build/verification/price-optimization/offline-core.log` before enabling AMPL.

Coverage includes independent integer financial calculations, public-price
selection, affordability and change caps, inventory, scenario-by-scenario
coverage of worker pay/operations/reserves, malformed inputs and probabilities,
stale data, expiration during solve, clock rollback, fractional or forged solver
output, solver failures, unsupported drivers, and unavailable AMPL. Serialization
checks ensure request strings cannot become AMPL commands and numerical output
is independent of the system decimal locale.

The synthetic demo validates a supplied selection of 200 and 300 euro cents.
It computes scenario surplus of 2,400 and 1,400 cents after protected costs,
expected surplus of 2,000 cents, and rejects a candidate above its ceiling.
This demo does not perform optimization or publish prices.

## AMPL C++ SDK compilation

The public Linux amd64 SDK linked from the
[official API download page](https://dev.ampl.com/ampl/reference/apis.html)
was downloaded over HTTPS and staged locally in `.build/deps/amplapi`.
The SDK is API 3.2.0; its module metadata identifies version `20260717`.
The [local provenance record](../tools/price-optimization/environment/ampl-sdk.lock.json)
records its URL and observed SHA-256. This digest identifies the retrieved
archive; it is not an independently published vendor checksum.

The AMPL-enabled library, synthetic demo executable, and integration-test
executable compiled and linked successfully in `.build/price-optimization-ampl`.
The two registered offline checks also passed (115 core assertions; the
disabled-backend assertion is inapplicable to this build). This initial stage
did not start the AMPL interpreter. SDK binaries remain ignored local
dependencies under vendor terms; runtime evaluation is recorded below.

Review narrowed supported solver configuration to `highs`. Other AMPL drivers
can have different status codes, so accepting arbitrary driver names while
assuming HiGHS success codes would be misleading. The adapter accepts the
reviewed normal `0` / `solved` status and AMPL's documented presolve solution
`99` / `solved`, with independent feasibility checks for both.

## Public runtime evaluation

The interpreter and HiGHS driver were obtained from the module links in
[AMPL's official container documentation](https://dev.ampl.com/ampl/python/docker.html#other-docker-containers).
The [runtime provenance record](../tools/price-optimization/environment/ampl-runtime.lock.json)
preserves the observed archive hashes, metadata versions, and reported binary
versions: AMPL 20260809 and HiGHS 1.15.1, driver 20260813, MP 20260806.
Vendor notices and the supplied `ampl.lic` remain unmodified under ignored
`.build/deps/ampl`. No account or license activation was created.

AMPL reports its bundled Demo license and a maintenance date of 2027-01-31;
this is not recorded as an inferred license expiry. The [vendor EULA](https://ampl.com/terms-eula/)
permits Demo evaluation with published size limits. These tiny synthetic
fixtures have at most seven binary variables and 39 declared linear constraints.
Demo evaluation does not establish an entitlement for operational store use.

The initial real integration run solved the first fixture at objective 2000
and caught a status-handling defect in the second: AMPL's presolver proved
infeasibility through an exception, which the adapter initially classified as
a generic failure. The failed run is retained at
`.build/verification/price-optimization/test-ampl.log`.

The runtime raised a generic `AMPLException`, rather than a dedicated
infeasibility exception. The adapter now checks both authoritative status
fields after that exception: only `299` / `infeasible` establishes presolve
infeasibility. A later fixture exposed AMPL's presolve solution status
`99` / `solved`; this now enters the same independent C++ validation as a
HiGHS optimal result. Both presolve codes are documented in the
[AMPL changelog](https://dev.ampl.com/releases/ampl.html), entry 2021-05-31.
Other errors and nonoptimal outcomes still return no recommendation.

The final AMPL-enabled run passed **all three CTest entries** in 2.93 seconds:
115 core assertions, the guardrail demo, and the real AMPL integration suite.
All 14 synthetic fixtures agreed with exhaustive enumeration. Explicit cases
covered a feasible model solved by AMPL presolve, presolve-detected
infeasibility, and a binary model that HiGHS proved infeasible. A deliberately
missing runtime directory remained a solver failure with no recommendation.
The final build/test log is
`.build/verification/price-optimization/test-ampl-final.log`; detailed fixture
output is `.build/verification/price-optimization/integration-ctest.log`.

The standalone AMPL demo also passed: bread at 200 euro cents, beans at 300
euro cents, expected worker surplus of 2,000 cents after protected costs.
Its output is `.build/verification/price-optimization/demo-ampl.log`.
No prices were published. `dpkg --audit` reported no package problems.

## Evidence still required

Operational use still requires an entitlement suitable for that deployment.

The current checks establish neither legal license compliance nor real-world
affordability, forecast accuracy, self-sufficiency, latency guarantees, or worker
control. The model has no live store connection and does not publish prices.
