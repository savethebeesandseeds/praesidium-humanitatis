<!-- SPDX-License-Identifier: MIT -->
# Configuration and JSON run records

The native simulator uses a central .cfg file and ordinary JSON records. There is no database, network storage service, or hidden configuration merge. The .cfg extension identifies a run configuration; its syntax is **strict JSON**, not INI. Comments, duplicate keys and trailing commas are not accepted.

The [example configuration](../configs/exchange.cfg) declares research assumptions and defaults to [empty history](../records/empty-history.json). No observed sales are supplied. The separate [sample history](../records/sample-history.json) has 14 explicitly synthetic rows, seven per SKU, and is an opt-in import example/test fixture. Set `records.history_file` to that file only when intentionally exercising synthetic history. Imported historical observations initialize the forecasting model; they are not added to the cash ledger, opening inventory or earned funding balance.

## Run configuration

Every field below is required:

    {
      "schema_version": "exchange.run.v1",
      "run_id": "example",
      "simulation": {
        "...": "the complete simulator configuration"
      },
      "records": {
        "history_file": "../records/empty-history.json",
        "output_directory": "../runs/example"
      }
    }

The abbreviated simulation object above illustrates the wrapper only. Use the complete checked-in example or generate current defaults. The simulator validates all its fields, including products, forecasts, consumers, funding feedback and continuity-assurance settings. The file layer does not supply missing defaults or override them.

The run_id is 1–64 characters drawn from letters, digits, dot, underscore and dash. It is copied into the manifest and assurance log. It does not choose the output directory: that location is records.output_directory.

Record paths resolve relative to the configuration file's directory, irrespective of the shell's working directory. Local absolute paths are also supported. URLs and explicit network-share paths are rejected; the runner has no database or network-fetch operation. Configuration and history files are limited to 8 MiB and 32 nested containers.

From the repository root in the native build environment:

    .build/post-profit-exchange/native/exchange_simulation --config projects/post-profit-exchange/configs/exchange.cfg

Generate a full configuration from the compiled simulator's current defaults:

    .build/post-profit-exchange/native/exchange_simulation --example-config

This prints the whole .cfg document without writing it. Save it into a chosen configuration file, then set its history and output paths. C++ defaults embed the central example configuration's simulation section at build time; the CLI does not maintain another independent set of product/settings values. A supplied .cfg remains authoritative for that run.

For another run, choose a new output directory and appropriate run ID in the .cfg. **An existing output path is always refused**, including an empty directory, file or symbolic link. The runner never deletes, reuses or overwrites previous run records.

With no CLI options, the existing one-command JSON stdin/stdout interface remains available. The --help option prints usage. Successful file runs print a JSON summary containing the run ID, output directory, manifest path and final policy summaries, and exit with code 0. File-run errors print a JSON error and exit with code 2. Legacy stdin mode retains its previous exit behavior; callers inspect the simulator's status.

## Historical observations

The default history is exactly an empty wrapper:

    { "schema_version": "exchange.history.v1", "observations": [] }

The following values are fictional examples illustrating the observation shape,
not supplied evidence of customer behavior:

    {
      "schema_version": "exchange.history.v1",
      "observations": [
        {"day": -2, "sku": "bread", "price": 200, "sales_units": 18, "stockout": false},
        {"day": -1, "sku": "bread", "price": 200, "sales_units": 20, "stockout": false}
      ]
    }

Days identify pre-run observations and must be negative integers from −1,000,000 through −1. Day −1 is the most recent possible pre-run day. Each SKU's days must strictly increase in file order; different SKUs may be interleaved. Duplicate day/SKU pairs and unknown SKUs are rejected. Empty observations are valid; the model then starts from each configured `base_demand` and the configured error floor. These are declared priors, not fabricated observations. `min_history` only controls the warmup flag; it is not a data-sufficiency test.

Each observation requires a known SKU, integer public price from 1 to 1,000,000 cents, integer sales count from 0 to 1,000,000, and a boolean stockout flag. There may be at most 12,000 observations, subject to the byte limit and simulator validation. The stockout flag preserves the distinction between observed sales and uncensored demand. It must not be dropped or converted into an invented demand count.

The whole validated history wrapper is passed to the simulator:

    { "op": "simulate", "config": <complete settings>, "history": <history wrapper> }

This schema validates record structure, not authenticity or empirical provenance.
Keep the source and collection context of real records separately inspectable;
never relabel simulator output or the synthetic fixture as observed operations.
The simulator updates forecasts from its own generated past sales during a run,
so that run's forecast scores remain simulation results. See the
[model and provenance notes](MODELS.md).

## Output directory

The simulator must accept the configuration and return a successful exchange.sim.v3 response before the runner creates an output directory. A successful simulation can still contain insolvency or a model-error terminal outcome; those are recorded outcomes, not grounds for fabricating additional days.

    runs/example/
      configuration.cfg          parsed central configuration snapshot
      history.json               parsed input history snapshot
      request.json               complete simulator command
      result.json                complete simulation result
      daily/
        optimized/day-0001.json   complete balancing-policy daily record
        fixed/day-0001.json       complete fixed-policy daily record
        ...
      assurance-events.json       policy-tagged assurance events
      manifest.json              completion marker, written last

JSON values are retained; formatting and whitespace are normalized. These local records are not digitally signed or tamper-proof audit evidence.

The copied configuration preserves its original relative references; moving that snapshot does not rewrite them. To replay the exact saved command with the same simulator build, pass request.json to the legacy stdin interface. That computes a response without creating or replacing native record files. To create another persisted run, prepare a new .cfg with a new output directory and the intended history path.

The result retains each policy's events and terminal summary, including terminal_status and terminal_day. Day files are created only for rows actually returned. Their counts may be below the requested horizon when a policy terminates early; files are not padded with fabricated sales or repeated prices.

The assurance log uses:

    {
      "schema_version": "exchange.assurance-log.v1",
      "run_id": "...",
      "events": [
        {"policy": "optimized", "event": <unaltered simulator event>}
      ]
    }

The policy is either optimized or fixed. Underlying exchange.assurance.v1 events can represent assurance_requested or insolvency_declared. Their IDs, reasons, requested support, cash, inventory, arrears and forecast snapshots remain intact. Writing an event does not contact a provider, obtain funding, send a message, or transfer money.

Event identity is the tuple `(run_id, policy, event.id)`, using the log's run ID
and the policy entry. `event.id` alone is local to a simulation; separate runs
can produce the same policy/day/type ID. The runner validates run-ID syntax but
does not enforce uniqueness across output directories. A caller aggregating runs
must assign distinct run IDs within its namespace and retain the source manifest.
The wrapper policy must agree with `event.policy`. These records do not implement
an ingestion service or idempotent payment processing. See the proposed
[assurance boundary](../../post-profit-assurance/CONTRACT.md).

The exchange.manifest.v1 manifest records the run ID, source paths, requested horizon, simulation schema, both policy summaries, and the relative path and byte count of each preceding output file. Byte counts are inventory information, not cryptographic integrity checks. The manifest is written and closed only after all other records have been written and closed.

If writing fails after directory creation, the runner preserves the partial directory and reports its path. A missing manifest identifies an incomplete run. Investigate or archive it separately; the runner will not overwrite it on retry. Completion is an application-level record, not a promise of power-loss durability or protection against later external file changes.

The standalone HTML page remains separate from the native file runner. Browser import/export must explicitly transport these files; loading a page does not grant arbitrary filesystem access or automatically load native records.
