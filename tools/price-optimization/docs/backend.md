<!-- SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0 -->
# Dashboard backend: supervised JSON Lines

`price_backend` is the Linux process boundary for a future dashboard server.
It accepts the [versioned contract](contract.md), runs the AMPL feedback model
in a separate worker, and emits structured results. It opens no network port,
publishes no prices, and has no governance or payment actions. The current
optimization objective is `operating_balance_tracking` version `1`: minimize
expected absolute funding balance under protected coverage and price-direction
constraints. Wages remain fixed inputs.

```text
dashboard server → JSON Lines → price_backend → isolated supervisor
                                               → price_ampl_worker → AMPL → HiGHS
                           ← verified JSON result ← process cleanup
```

The dashboard server owns the local pipe and translates these events to its
web interface. Browser code must not receive permission to choose executables,
runtime paths or solver commands. This is an integration boundary, not an HTTP
server or an authentication system.

## Build and execute

Inside the existing approved optimizer container, with the vendor dependencies
already staged:

```bash
bash tools/price-optimization/tasks.sh test-ampl
.build/price-optimization/price_backend --example |
  .build/price-optimization/price_backend \
    --ampl-directory /workspace/praesidium-humanitatis/.build/deps/ampl
```

`--example` emits one fresh synthetic solve command. The second process returns
an acceptance and then the actual AMPL result. The default worker is
`price_ampl_worker` beside the backend executable. Administrative options are
`--worker ABSOLUTE_PATH`, `--ampl-directory ABSOLUTE_PATH`, and `--solver highs`.
No request field can override them. The test worker is used only by explicitly
configured test commands; it is never a fallback for unavailable AMPL.

No Debian packages, container ports, mounts or lifecycle changes are required.
The JSON parser is vendored under its own MIT license; builds do not fetch it.
The supervisor requires Linux with `/proc` and `close_range` support (kernel
5.9 or newer). Other platforms can build the contract and existing core, but
these process executables are Linux-specific.

## Commands and events

Each command is one UTF-8 JSON object followed by a newline. There is no startup
greeting or solver text on stdout. Every object rejects unknown command fields.

| Command | Response |
| --- | --- |
| `{"op":"solve","timeout_ms":5000,"request":{...}}` | `accepted`, followed by exactly one terminal `result` while the connection remains usable. Invalid requests produce an `invalid_input` result without acceptance. |
| `{"op":"cancel","request_id":"..."}` | `cancellation_requested` for the active request, then its `cancelled` result after cleanup. An already completed or unknown request gets `rejected` with `not_found`. |
| `{"op":"ping"}` | `pong`, with the active request identifier or an empty string. |

`timeout_ms` is a required integer from 1 through 60000. The initial backend
allows one active solve per process. Another solve receives a `rejected` event
with error code `busy`; it does not replace the running job. Use fresh request
identifiers to correlate UI operations; this is not a persistent job store or
an idempotent execution cache. A late cancellation cannot reverse a result
already delivered.

Malformed commands get `rejected` with `invalid_command`; incomplete final
frames get `incomplete_frame`. Commands during an active solve are limited to
4096 bytes so cancellation polling cannot be occupied by parsing a large new
request. Oversized active commands get `active_command_too_large` before JSON
parsing. Input buffering is capped at 8 MiB; exceeding it closes the service
after requesting active-job cleanup. Clients should wait for acceptance or a
rejection and apply backpressure instead of flooding commands.

EOF finishes any active solve up to its deadline, then exits after queued
responses are delivered. SIGINT or SIGTERM cancels active work and exits.
Closing the parent process's control channel, including unexpected parent
death, causes the isolated supervisor to terminate and reap its descendants.
A disconnected or non-reading output consumer cannot retain unlimited output
memory: the output queue is capped and idle output delivery expires.

## Terminal results

Every `result` identifies `ph.price.v3`, engine `0.4.0`, objective
`operating_balance_tracking` version `1`, model `public-prices.v3` and the
compiled AMPL model's SHA-256. A parsed request also
includes its policy id/version and full input snapshot. These are reproducibility
records, not proof of policy approval or a complete vendor-runtime attestation.
The runtime/SDK provenance remains in `environment/`.

| Status | Meaning |
| --- | --- |
| `recommended` | Solver reported an accepted optimum and the result passed independent verification. |
| `invalid_input` | Schema, types, joins, numeric bounds or freshness were invalid. |
| `infeasible` | AMPL/HiGHS reported infeasibility under the protected coverage, inventory and price-policy/feedback constraints. |
| `unavailable` | The worker was built without AMPL support. |
| `solver_failed` | Startup, execution, runtime, solver or process supervision failed. |
| `rejected_solution` | A result was malformed, mismatched, stale or failed independent checks. |
| `timed_out` | The deadline expired; the worker tree was terminated and reaped. Error code `deadline_exceeded`. |
| `cancelled` | Cancellation terminated and reaped the worker tree. |

Failures contain a structured `{code,message}` error and no recommendation.
Acceptance means work has started, not that a price is valid. A nonzero backend
exit or pipe failure means the client must discard any incomplete response.

Successful recommendations include current and selected prices, candidate
indices, per-product and per-scenario financial calculations, expected revenue,
unit costs, protected wages, operations, reserve contribution and worker surplus.
The required request `feedback` object supplies signed `funding_balance`,
cash-backed earned `coverage_credit`, and disjoint additional operating
`liquidity_buffer`. Each scenario reports `funding_balance` equal to
input funding balance plus its worker surplus. The minimized score is
`recommendation.expected.absolute_funding_balance`; expected worker surplus is
a diagnostic. Coverage slack equals scenario worker surplus plus coverage credit
and liquidity buffer. Neither cash source is added to the signed funding balance
or objective. The result also echoes all three inputs and
the permitted price direction.
Constraint slack is recomputed exactly, with explicit binding flags. Supplied
forecasts are labeled as input evidence. Candidate exclusion reasons cover
local affordability, change-cap, inventory, feedback-direction and
contribution-reducing price-increase violations; they do not claim
to be a complete global infeasibility explanation or economic causal analysis.

Positive funding balance permits decreases or holding; negative balance permits
increases or holding, with no increase allowed to reduce that product's forecast
contribution in any scenario; zero balance requires holding. Every candidate grid
includes its previous price and corresponding forecasts. The backend does not
derive earned balances or certify liquidity from a ledger. The calling
application must establish disjoint spendable cash amounts for coverage credit
and additional liquidity, and schedule each new reserve funding
requirement once, separately from reserve cash earmarking. Releasing a reserve
does not restart that requirement. See the [contract](contract.md) for the exact
formulation and input bounds.

Operating liquidity can fund a forecast shortfall with neutral or negative
earned feedback. It does not create a discount signal or waive any price,
inventory or protected funding constraint. The application owns continuity
warnings and actual cash-exhaustion decisions; an optimizer infeasibility result
is not a command to close a store.

The parent checks request identity, snapshot, policy, objective and model digest
against its own build. It independently re-evaluates selected candidates and
the objective, then regenerates the financial explanation from the original
inputs. It does not trust financial totals supplied by a worker. Independent
checking establishes feasibility; optimality still relies on the solver.

`execution` records the configured solver, deadline, elapsed process time,
worker exit code and bounded diagnostics. Worker stdout is limited to 8 MiB;
exceeding this produces a failure. Diagnostics are capped at 64 KiB and marked
when truncated. Large but structurally valid inputs may exceed the output cap;
configured input bounds are not a capacity guarantee. Vendor stdout is captured as
diagnostics rather than corrupting the protocol. Diagnostics are untrusted
display text; invalid UTF-8 bytes are replaced. A dashboard should render them
as text, never HTML. Recommendation expiry still applies when the UI reads it.

## Deadline and cleanup guarantees

The parent uses a monotonic clock and nonblocking pipes. A separate supervisor
also enforces the worker deadline, so a slow parent callback cannot leave the
solver running indefinitely. Cancellation and timeout terminate the worker
process group. The supervisor adopts and reaps descendants, including ones
that escape into a new session. Normal worker exit also cleans up descendants.

The deadline covers worker startup, input transfer, computation and collection;
initial request validation and final response rendering are outside the worker
budget. This is bounded worker execution, not a hard real-time operating-system
guarantee. Scheduling and cleanup add latency. SIGKILL cannot immediately stop
uninterruptible kernel sleep; cleanup waits for actual reaping rather than
claiming that processes disappeared. The low-level `optimize()` C++ API remains
synchronous; dashboard integrations must use this supervised boundary to obtain
these execution controls.

## Verification boundary

Contract tests cover strict parsing, cross-joins, arithmetic and explanations.
Process tests exercise blocked children/grandchildren, escaped sessions,
timeouts, cancellation, parent death, output limits and cleanup. Backend tests
exercise protocol state, invalid input, busy/cancel behavior and forged worker
results. With the vendor runtime enabled, they also exercise actual AMPL
success, infeasibility, timeout, cancellation and subsequent recovery through
the same JSON boundary used by a dashboard server.

Synthetic evaluation does not establish live-store suitability, demand accuracy,
production-scale performance or operational AMPL entitlement. The exchange's
standalone browser simulator runs the shared C++ checks and an explicitly selected
bounded enumeration backend in WebAssembly. It does not run this process
supervisor or AMPL; these backend checks are not browser interaction tests.
