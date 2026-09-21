# Simple exponential smoothing (EWMA)

MIT-licensed, standalone C++17 simple exponential smoothing for projects in
praesidium-humanitatis. This tool supplies an inspectable, nonseasonal,
single-level exponentially weighted moving average (EWMA) baseline, with
residual uncertainty and past-only diagnostics. Its name identifies this
specific method; it is not a general forecasting engine. It has no optimizer,
exchange, JSON, LibTorch, database, or network
dependency. Native and WebAssembly callers use the same source.

The Temporal Fusion Transformer remains a separate tool at
[`../temporal-fusion-transformer`](../temporal-fusion-transformer/README.md).
There is no TFT training, inference adapter, automatic model selection, or
empirical demand fit in this component. Moving this method into a tool does
not make assumed inputs or simulated sales into real observations.

## Typed interface

Include `ph/exponential_smoothing/ewma.hpp` and link CMake target `ph::exponential_smoothing`
(concrete target `ph_exponential_smoothing`). Types live in `ph::exponential_smoothing`:

- `EwmaConfig`: alpha, sigma multiplier, eligible-history warmup threshold,
  and a declared prior uncertainty floor.
- `EwmaForecaster`: constructor with a declared prior level, read-only
  `predict(response_factor, horizon_days)`, `observe(Observation)`, and
  `diagnostics()`.
- `Observation`: strictly increasing day, observed nonnegative value,
  declared response factor, and explicit `usable`, `censored`, or `closed`
  status. A closed record requires value zero.
- `ForecastPoint`: one-step and cumulative-horizon mean, sigma, stress
  bounds, eligible history count, and warmup state.
- `UpdateResult`: the prediction issued before learning, optional residual
  and coverage result, and the updated normalized level.
- `Diagnostics`: counts, optional last day, normalized level and sigma,
  plus all-eligible and post-warmup walk-forward error metrics.

`std::optional` distinguishes missing evidence from measured zero. With no
history, predictions use only the declared prior and sigma; evaluation counts
are zero and accuracy metrics are absent. No observations are generated or
imputed. Prediction does not mutate state. The tool never reads future rows.
The caller must supply records in chronological order and determine whether
they were genuinely available at the forecast's origin. Days need not be
consecutive; missing days do not become zero observations.

`post-profit-exchange/simulation/models.*` is the application adapter. It owns
JSON parsing, model selection, sales/stock validation and conservative stockout
censoring. The exchange owns product price-response assumptions, `.cfg` files,
record provenance, reserve scheduling, and assurance policy. The tool receives
only observed values and the caller's censor/closure decision. Other projects
can use it without importing exchange's consumer or economic model.

## Computation and limits

For level `L`, response factor `r`, observed value `y`, and smoothing fraction
`a = alpha_bps / 10000`, the issued mean is `L*r`. A usable observation is
normalized as `x = y/r`. The pre-update normalized residual is `e = x-L`.
After scoring the issued forecast:

```
error_square = (1-a)*error_square + a*e*e
level        = (1-a)*level        + a*x
sigma       = max(prior_sigma_units, sqrt(error_square)) * r
```

The initial `error_square` is the square of the declared prior sigma. Reported
MAE/RMSE use residuals on the observed scale, before learning. Post-warmup
metrics include only predictions that were already warmed up when issued.
Censored and closed records advance the time/count bookkeeping but change
neither model state nor evaluation metrics. Rejected input changes nothing.

One-step stress bounds are `max(0, mean-z*sigma)` and `mean+z*sigma`, where
`z = sigma_multiplier_bps / 10000`. For cumulative horizon `H`, mean is
`H*mean` and sigma is `sigma * sqrt(sum((1+a*j)^2, j=0..H-1))`. This is an
additive-innovation SES calculation assuming uncorrelated, equal-variance
future innovations. Configured sigma bands are stress assumptions, not a
guarantee of coverage or a claim that demand is normally distributed.

This baseline has no trend, seasonality, learned price elasticity, feature
selection, or structural-change detection. Censoring can create selection
bias. A warmup threshold records how many usable observations have arrived;
it does not certify forecast quality. Calibration and comparisons require
real observations held out chronologically. Applications remain responsible
for matching model assumptions and data provenance to their research claims.

Bounds are explicit in the public validation and implementation: alpha
1..10000 basis points, sigma multiplier 0..100000, minimum history 1..3650,
prior level/sigma and observed values 0..1000000, day -1000000..1000000,
response factor `(0,1000000]`, normalized observation at most `1e12`, and
horizon 1..365. All floating-point inputs must be finite.

## Build and verification

From the repository root, with an existing C++17 compiler and CMake 3.25+:

```
cmake -S tools/exponential-smoothing -B .build/exponential-smoothing -DBUILD_TESTING=ON
cmake --build .build/exponential-smoothing
ctest --test-dir .build/exponential-smoothing --output-on-failure
```

CTest `exponential_smoothing_ewma` checks hand-computable algebra fixtures for cold
starts, score-before-learning behavior, response normalization, censored and
closed observations, sigma floors, horizon uncertainty, missing metrics,
chronological validation, and immutable issued predictions. These fixtures
test software behavior; they are not a fabricated dataset or empirical model
validation. Exchange's existing integration and native/WASM comparison tests
verify the application adapter separately.

The full MIT grant is in [LICENSE](LICENSE). The exponential-smoothing component does
not inherit the separate price optimizer's restricted license.
