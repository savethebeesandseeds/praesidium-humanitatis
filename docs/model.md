# Model contract and design

This document specifies the implemented forecasting component of the standalone
engine. Its tensor interface is independent of any humanitarian or other domain;
applications supply feature meanings, target definitions, and data preparation.
The proposed connection to query inspection is a separate design task. No query
format or query-inspection API is implemented by this model contract.

## Inputs

`praesidium_humanitatis::Config` fixes model dimensions, variable counts, categorical vocabularies,
dropout, and quantiles. `praesidium_humanitatis::Batch` deliberately separates `statics`, `past`,
and `future`. Each is a `FeatureBatch` containing `continuous` and `categorical`
tensors. Variable order is continuous columns followed by categorical columns.

| Tensor | Shape | Meaning |
| --- | --- | --- |
| `statics.continuous` | `[B, S_cont]` | Static real-valued covariates |
| `statics.categorical` | `[B, S_cat]` | Static integer category IDs |
| `past.continuous` | `[B, E, P_cont]` | Historical targets/observations and known covariates |
| `past.categorical` | `[B, E, P_cat]` | Historical categories |
| `future.continuous` | `[B, H, F_cont]` | Covariates available at forecast creation time |
| `future.categorical` | `[B, H, F_cat]` | Categories available at forecast creation time |
| loss targets | `[B, H]` | Future labels, supplied to the loss only |

`B`, `E`, and `H` must be positive. Each input group needs at least one variable.
Continuous values must be finite float32 or float64 tensors matching the model's
dtype and device. The model starts in float32; use `model->to(torch::kFloat64)`
before supplying double-precision inputs.
Categorical values must be int64 and lie in `[0, cardinality)`. Both tensors are
required even when one has zero columns. All windows in one batch have equal
length; different forward calls may have different lengths. No implicit padding,
missing-value imputation, automatic category remapping, or normalization occurs.

The input schema is a deliberate small foundation. Establish column names,
units, order, category vocabularies, target definition, data cutoff, and fitted
normalization before making a real dataset. By default, historical and future
groups have separate learned projections. Set `future_continuous_past_indices`
and `future_categorical_past_indices` to share known-variable embeddings with
their historical counterparts. Empty maps preserve legacy behavior; a map entry
of `-1` keeps that future variable independent. Shared categories require equal
cardinalities. Shared parameters are registered and updated once.

## Computation

Per-variable projections/embeddings feed separate static, past, and future
selection networks. Four static contexts condition temporal selection,
enrichment, and the encoder LSTM's hidden and cell states. Separate LSTMs encode
history and decode the known future, carrying state across the boundary. Gated
residual connections surround recurrent processing, enrichment, attention, and
position-wise processing. The final residual returns to the recurrent features.

Interpretable attention has head-specific queries/keys, shared values, and
averages heads before output projection. The causal mask includes its diagonal.
Only decoder queries are computed, over encoder and decoder keys. The forecast
head produces one target's quantiles at every future position.

These choices follow [the TFT paper](https://arxiv.org/abs/1912.09363) and
[the reference model](https://github.com/google-research/google-research/blob/master/tft/libs/tft_model.py).
Numerical parity with the TensorFlow implementation is being tested separately.
Legacy defaults retain independent embeddings and LayerNorm epsilon `1e-5`.
The reference configuration uses shared known-variable embeddings and
`Config::layer_norm_epsilon = 1e-3`. Initial weights and recurrent parameter
conventions still require explicit mapping. See the
[TensorFlow layer contract](https://www.tensorflow.org/api_docs/python/tf/keras/layers/LayerNormalization).

The 2026-09-18 replication audit uses authors' source revision
`5b09c22d73a9d35eb6c5d2a99b95677a45053466`. Temporal selection flattens
`[variable, hidden]` here and `[hidden, variable]` in that reference; transferring
weights requires a corresponding column permutation. Decoder-only attention
avoids unused historical query outputs, but also changes dropout RNG consumption.
Neither layout difference alone demonstrates an incorrect forecasting equation.

LibTorch initialization defaults are also retained. They differ from the
reference's Keras initialization; the synthetic trainer's global gradient
clipping and Adam update are not an exact reproduction of its training procedure.
Matching a random seed across frameworks is therefore not a parity test.
The [replication gate](tft-replication.md) requires mapped weights, explicit
settings, and direct reference comparisons before a benchmark-equivalence claim.

`forward(batch, &diagnostics)` optionally exposes intermediate tensors for direct
comparison. These retain their autograd graph; release the map after each batch.
This inspection surface does not yet define the future query-inspection product.

## Outputs

| Field | Shape | Interpretation |
| --- | --- | --- |
| `predictions` | `[B, H, Q]` | Quantiles, ordered as `Config::quantiles` |
| `static_weights` | `[B, S]` | Static variable-selection probabilities |
| `past_weights` | `[B, E, P]` | Historical variable-selection probabilities |
| `future_weights` | `[B, H, F]` | Future variable-selection probabilities |
| `attention_weights` | `[B, heads, H, E+H]` | Attention probabilities; dropout is applied to head outputs |

For horizon index `h` starting at zero, keys after `E+h` receive zero probability.
The attention diagnostic and selection weights sum to one along their final
dimension. Use `model->eval()` and `torch::NoGradGuard` for repeatable inspection
without training dropout or an autograd graph.

The loss is `mean_B,H(sum_Q(max(q*(target-prediction),
(q-1)*(target-prediction))))`. It sums quantiles, so changing the number of
quantiles changes its scale. Quantiles must be strictly increasing and inside
`(0,1)`. The independent output head can produce crossing quantiles. Coverage,
interval width, and crossing frequency need explicit evaluation; quantiles are
not a guarantee of calibrated uncertainty. The optional
`Config::ordered_quantiles = true` head uses an unconstrained central output
and outward softplus gaps to produce nondecreasing quantiles. It retains the
same projection dimensions and is trained directly with pinball loss, without
sorting. The default and legacy checkpoints retain independent outputs.
See [uncertainty validation](uncertainty-validation.md) for this variant and
separate interval calibration.

Selection and attention weights are diagnostics of this fitted model. Correlated
features can exchange importance, and a prediction's associations do not identify
the effect of an intervention. These outputs may inform the future query-inspection
design, but are not yet a query-inspection interface or a causal explanation.
Applications own the meaning and consequences of decisions made using forecasts;
the suffering-mitigation allocation demonstration must separately evaluate
whether its decisions and actions help people. See the tool and demonstration
work in the [research plan](research-plan.md).
