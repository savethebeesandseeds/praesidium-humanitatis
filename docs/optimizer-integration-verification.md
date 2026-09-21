# Optimizer dashboard integration — 2026-09-20

Historical v1 evidence. The current `ph.price.v3` / `public-prices.v3` feedback
and liquidity contract supersedes the objective described below. All counts,
hashes and scope statements in this record describe that earlier snapshot;
see the [current exchange verification](exchange-simulation-verification.md)
and [current contract](../tools/price-optimization/docs/contract.md).

Goal: make the existing price optimizer callable by a synthetic store dashboard
through a versioned input/result contract, bounded execution with cancellation,
and independently verified financial explanations. Governance, broader risk
management, new optimization objectives, live publication and the dashboard
UI are outside this completed integration work.

## Delivered boundary

- [Contract](../tools/price-optimization/docs/contract.md): `ph.price.v1` separates
  policy, costs, catalog/candidates, demand forecasts and objective identity.
  Strict parsing rejects unknown fields, duplicate keys, incorrect numeric
  types and missing/extra/duplicate joins. Results retain the validated input,
  policy version, model version/SHA-256 and engine version.
- [Backend](../tools/price-optimization/docs/backend.md): Linux JSON Lines service
  with `solve`, `cancel` and `ping`, correlated events, one active worker,
  monotonic deadlines and bounded input/output. No network listener is added.
- [Process supervisor](../tools/price-optimization/src/process.cpp): independent
  worker deadline, cancellation, descendant cleanup/reaping, parent-death
  cleanup and bounded diagnostics. A stalled parent callback cannot leave the
  solver running past the supervisor's deadline.
- Explanations: independently recomputed prices, scenario/expected revenue,
  unit costs, wages, operations, reserve contribution and surplus; exact
  constraint slack/binding flags and local candidate-exclusion reasons.

The AMPL model and core optimization objective were not changed. The model
SHA-256 is `e7e95d508b0a9461c8843a8663e7c5f1f8a5068dc5c7998c036e77a67945c589`.
The new boundary verifies a worker's model digest and objective before
regenerating explanations; a mismatched worker is rejected.

## Verification

The existing approved Debian 12 container was reused:
`5738ae552c575e91e0d4308b8aa2c66aa170e8dd817282fd7abd5817907d3ff9`.
No image, port, mount, volume, lifecycle definition or Debian package changed.
The JSON parser was vendored from the official nlohmann/json 3.12.0 release,
verified against its published SHA-256 and retained with its MIT license.

The AMPL-enabled build passed **7/7 CTest entries** in 5.37 seconds:

| Check | Evidence |
| --- | --- |
| Existing core | 115 assertions passed. |
| Existing guardrail demo | Passed. |
| Strict contract and explanations | 79 assertions passed. |
| Process execution and cleanup | 113 assertions passed, including real child/grandchild cleanup, escaped sessions, parent death and a deliberately stalled callback. |
| Backend protocol | 79 checks passed using explicitly selected fixtures, including forged/mismatched worker replies, invalid UTF-8 diagnostics, busy/cancel states and invalid requests. |
| Backend through real AMPL | 108 checks passed, including success, infeasibility, deadline, cancellation and a subsequent successful solve. |
| Existing AMPL model integration | All 14 tiny fixtures matched exhaustive enumeration. |

The SDK-free build passed **6/6 CTest entries** in 2.15 seconds, including the
116-assertion core and a real AMPL-disabled worker returning `unavailable`
through the backend. It does not substitute fixture results for AMPL.

These times describe complete local test suites, not throughput or latency
benchmarks. The process tests separately observed worker termination while a
parent callback slept; OS scheduling and actual process reaping still add
latency. Uninterruptible kernel sleep can delay cleanup beyond a deadline.

Saved local evidence under `.build/verification/price-optimization/`:

- `integration-backend-final.log` and `integration-backend-ctest.log`;
- `integration-offline-final.log` and `integration-offline-ctest.log`;
- `backend-demo.request.jsonl` and `backend-demo.response.jsonl`.

After adding the explicit SDK-free registration, the final backend checks were
rebuilt and passed again (2/2); that log is
`integration-backend-final-recheck.log`. Documentation links and the unchanged
model digest were checked. Final Docker inspection showed only the container's
original interactive `bash` process, with no remaining worker, supervisor,
AMPL or HiGHS process. The approved container remains running.

The first integration run exposed a `timeout` versus `timed_out` vocabulary
mismatch. The corrected protocol uses `timed_out`; the original failure log
is retained as `integration-backend-first.log`.

## Concrete store example

The saved demonstration sends a fresh synthetic request through the actual
backend and AMPL worker, and receives one acceptance and one recommendation:

| Value | Euro cents |
| --- | ---: |
| Bread price | 200 |
| Beans price | 300 |
| Expected revenue | 6,000 |
| Expected unit costs | 3,000 |
| Protected wages | 800 |
| Operating costs | 100 |
| Reserve contribution | 100 |
| Expected worker surplus | 2,000 |

Scenario surplus is 2,400 and 1,400 cents. The response identifies supplied
forecast quantities separately from derived finances and excludes the
240-cent bread candidate for affordability and price-change violations.
No prices were published and no money was moved.

## Remaining scope

The dashboard can now consume this boundary. It still needs its UI and a server
adapter that owns the local process pipes. Runtime dependencies currently use
the vendor Demo evaluation mode; this work establishes neither operational
entitlement nor production readiness. Large-store performance, demand evidence,
policy authorization, full cash accounts, global infeasibility explanations
and additional objectives remain separate work.
