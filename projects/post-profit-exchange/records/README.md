<!-- SPDX-License-Identifier: MIT -->
# History inputs and their provenance

`empty-history.json` is the default selected by `configs/exchange.cfg`. It
contains no observations. With this input, the simulation starts from the
configured demand and error assumptions and reports zero eligible history.

`sample-history.json` contains fourteen hand-authored synthetic observations:
seven days for each example product. It is an explicit software fixture, not
measured store activity, and is no longer loaded by the default configuration.
It may be selected deliberately for an example or a regression test. It does
not justify a trained model, uncertainty calibration, or economic conclusions.

During a simulation, the consumer model generates sales. Learning from those
past simulated sales exercises the forecasting/operation loop; it does not
create observations about real customers. Configured demand, consumer behavior
and price response are assumptions, including assumptions shared by the
consumer generator and the forecast's normalization.

When importing real pre-run history, keep the original measured records and their source,
collection dates, units, product identifiers and stockout metadata. Point the
central `.cfg` at that separate history file. Do not fill missing days with
invented sales or treat unobserved demand during a stockout as known demand.
The subsequent simulated sales remain synthetic even when the initial history
comes from a real store; this runner does not ingest live transactions.
The current input schema validates values and ordering; it does not verify a
file's origin or transform irregular/missing observations into a complete series.

The file schema is documented in [FILES.md](../docs/FILES.md). The source package
includes only this note, the empty input and the explicit synthetic fixture;
other imported histories and saved runs remain outside that package.
