#!/usr/bin/env python3
"""Export a direct, pinned TensorFlow 1.15 TFT numerical parity fixture.

Run with the isolated reference Python, not the project's LibTorch Python.
The original module is imported unchanged. Wrappers observe layer creation and
activations; all model equations, losses and Adam operations execute TensorFlow.
Binary arrays are little endian and already permuted to the public C++ layout.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

os.environ.setdefault("CUDA_VISIBLE_DEVICES", "")
os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")

import numpy as np

REFERENCE_SHA256 = "53f0046e12b9b79ffea096516b2278774e65d760df684f97600341b9abef9a2c"
REFERENCE_COMMIT = "5b09c22d73a9d35eb6c5d2a99b95677a45053466"
HIDDEN, HEADS, HISTORY, HORIZON, BATCH = 8, 2, 5, 3, 3


class Fixture:
    def __init__(self, directory):
        self.directory = Path(directory)
        if not self.directory.is_dir():
            raise RuntimeError("Fixture output must have been exclusively created before model construction")
        self.rows = []

    def add(self, kind, name, value):
        array = np.asarray(value)
        dtype = "i64" if np.issubdtype(array.dtype, np.integer) else "f32"
        shape = array.shape
        array = np.ascontiguousarray(array, dtype="<i8" if dtype == "i64" else "<f4").reshape(shape)
        filename = "{:04d}.{}".format(len(self.rows), dtype)
        array.tofile(str(self.directory / filename))
        self.rows.append((kind, name, dtype, ",".join(map(str, array.shape)), filename))

    def finish(self, metadata):
        with (self.directory / "manifest.tsv").open("w") as handle:
            handle.write("kind\tname\tdtype\tshape\tfile\n")
            for row in self.rows:
                handle.write("\t".join(row) + "\n")
        (self.directory / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-root", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--seed", type=int, default=1729)
    parser.add_argument("--preset", choices=("mixed", "electricity"), default="mixed")
    parser.add_argument("--initialization", choices=("mapped", "keras"), default="mapped")
    parser.add_argument("--static-cardinality", type=int, default=370,
                        help="Electricity entity vocabulary size; set from prepared dataset metadata")
    args = parser.parse_args()
    if args.static_cardinality < 12:
        parser.error("--static-cardinality must be at least 12 for the repeated-ID Electricity fixture")
    output = Path(args.output).resolve()
    # Failed attempts are evidence too. Never overwrite or reuse their paths.
    output.mkdir(parents=True, exist_ok=False)
    harness_sha256 = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in (
        Path(__file__), Path(__file__).parent / "reference_optimizer.h",
        Path(__file__).parent / "reference_loss.h",
        Path(__file__).parent.parent / "examples" / "compare_reference.cpp")}
    global HIDDEN, HEADS, HISTORY, HORIZON, BATCH
    mixed = args.preset == "mixed"
    if not mixed:
        HIDDEN, HEADS, HISTORY, HORIZON, BATCH = 160, 4, 168, 24, 2
    static_variables, past_variables, future_variables = (2, 4, 2) if mixed else (1, 4, 3)
    source = Path(args.reference_root) / "libs" / "tft_model.py"
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    if digest != REFERENCE_SHA256:
        raise RuntimeError("Pinned reference source hash mismatch: " + digest)
    sys.path.insert(0, str(Path(args.reference_root).resolve()))
    import tensorflow as tf
    if not tf.__version__.startswith("1.15."):
        raise RuntimeError("Requires original TensorFlow 1.15.x, got " + tf.__version__)
    import libs.tft_model as reference

    tf.reset_default_graph()
    tf.set_random_seed(args.seed)
    session = tf.Session(config=tf.ConfigProto(
        intra_op_parallelism_threads=1, inter_op_parallelism_threads=1,
        device_count={"GPU": 0}))
    tf.keras.backend.set_session(session)
    rng = np.random.RandomState(args.seed)
    mappings = []
    activations = {}
    normalizations = []
    nesting = [0]

    # C++ variable order is continuous then categorical. Reference historical
    # order is unknown, known continuous, known category, observed target.
    cpp_to_tf_past = np.array([0, 1, 3, 2]) if mixed else np.arange(4)

    def identity(x):
        return x

    def add(variable, cpp_name, transform=identity):
        mappings.append((variable, "network." + cpp_name, transform))

    def dense(variables, prefix, input_permutation=None, output_permutation=None):
        for var in variables:
            suffix = var.name.rsplit("/", 1)[-1].split(":")[0]
            if suffix == "kernel":
                def convert(x, ip=input_permutation, op=output_permutation):
                    if ip is not None:
                        x = x[ip, :]
                    if op is not None:
                        x = x[:, op]
                    return x.T
                add(var, prefix + ".weight", convert)
            elif suffix == "bias":
                add(var, prefix + ".bias", lambda x, op=output_permutation: x if op is None else x[op])
            else:
                raise RuntimeError("Unexpected dense variable " + var.name)

    def norm(variables, prefix, permutation=None):
        for var in variables:
            suffix = var.name.rsplit("/", 1)[-1].split(":")[0]
            if suffix not in ("gamma", "beta"):
                raise RuntimeError("Unexpected normalization variable " + var.name)
            add(var, prefix + (".weight" if suffix == "gamma" else ".bias"),
                lambda x, p=permutation: x if p is None else x[p])

    grn_specs = [("static_selection.weights_network", static_variables, None)]
    grn_specs += [("static_selection.variable_{}".format(i), None, None) for i in range(static_variables)]
    grn_specs += [(n, None, None) for n in (
        "selection_context", "enrichment_context", "hidden_context", "cell_context")]
    grn_specs += [("past_selection.weights_network", 4, cpp_to_tf_past)]
    grn_specs += [("past_selection.variable_{}".format(int(np.where(cpp_to_tf_past == i)[0][0])), None, None)
                  for i in range(4)]
    grn_specs += [("future_selection.weights_network", future_variables, np.arange(future_variables))]
    grn_specs += [("future_selection.variable_{}".format(i), None, None) for i in range(future_variables)]
    grn_specs += [("enrichment", None, None), ("positionwise", None, None)]
    grn_count = [0]
    original_grn = reference.gated_residual_network

    def observed_grn(*positional, **keywords):
        name, output_variables, order = grn_specs[grn_count[0]]
        grn_count[0] += 1
        before = len(tf.trainable_variables())
        if name == "selection_context":
            activations["static_selected"] = positional[0]
        nesting[0] += 1
        result = original_grn(*positional, **keywords)
        nesting[0] -= 1
        variables = tf.trainable_variables()[before:]
        cursor = 0
        permutation = None
        if order is not None:
            # C++ flattened [variable, hidden] -> TF [hidden, variable].
            permutation = np.array([h * len(order) + int(order[v])
                                    for v in range(len(order)) for h in range(HIDDEN)])
        if output_variables is not None:
            dense(variables[cursor:cursor + 2], name + ".residual_projection", permutation, order)
            cursor += 2
        dense(variables[cursor:cursor + 2], name + ".input_projection", permutation)
        cursor += 2
        if keywords.get("additional_context") is not None:
            dense(variables[cursor:cursor + 1], name + ".context_projection")
            cursor += 1
        dense(variables[cursor:cursor + 2], name + ".hidden_projection")
        cursor += 2
        dense(variables[cursor:cursor + 2], name + ".glu.value", output_permutation=order)
        cursor += 2
        dense(variables[cursor:cursor + 2], name + ".glu.gate", output_permutation=order)
        cursor += 2
        norm(variables[cursor:cursor + 2], name + ".norm", order)
        cursor += 2
        if cursor != len(variables):
            raise RuntimeError("Unexpected GRN variables: " + name)
        tensor = result[0] if isinstance(result, tuple) else result
        if name in ("selection_context", "enrichment_context", "hidden_context", "cell_context",
                    "enrichment", "positionwise"):
            activations[{"enrichment": "enriched", "positionwise": "processed"}.get(name, name)] = tensor
        return result

    reference.gated_residual_network = observed_grn
    original_glu = reference.apply_gating_layer
    glu_names = iter(("recurrent_glu", "attention_glu", "final_glu"))

    def observed_glu(*positional, **keywords):
        if nesting[0]:
            return original_glu(*positional, **keywords)
        name = next(glu_names)
        if name == "recurrent_glu":
            activations["recurrent"] = positional[0]
        before = len(tf.trainable_variables())
        result = original_glu(*positional, **keywords)
        variables = tf.trainable_variables()[before:]
        dense(variables[:2], name + ".value")
        dense(variables[2:], name + ".gate")
        return result

    reference.apply_gating_layer = observed_glu
    original_norm = reference.add_and_norm
    norm_names = iter((("recurrent_norm", "temporal"), ("attention_norm", "attention_skip"),
                       ("final_norm", "final")))

    def observed_norm(*positional, **keywords):
        before = len(tf.trainable_variables())
        result = original_norm(*positional, **keywords)
        if not nesting[0]:
            name, activation = next(norm_names)
            norm(tf.trainable_variables()[before:], name)
            activations[activation] = result
        return result

    reference.add_and_norm = observed_norm
    original_embeddings = reference.TemporalFusionTransformer.get_tft_embeddings

    def observed_embeddings(self, inputs):
        before = len(tf.trainable_variables())
        result = original_embeddings(self, inputs)
        variables = tf.trainable_variables()[before:]
        if len(variables) != (10 if mixed else 9):
            raise RuntimeError("Unexpected embedding variable count: " + str(len(variables)))
        add(variables[0], "static_embedding.categorical_0.weight")
        if mixed:
            add(variables[1], "past_embedding.categorical_0.weight")
        embedding_specs = ((2, "static_embedding.continuous_0"), (4, "past_embedding.continuous_2"),
                           (6, "past_embedding.continuous_0"), (8, "past_embedding.continuous_1")) if mixed else (
                               (1, "past_embedding.continuous_3"), (3, "past_embedding.continuous_0"),
                               (5, "past_embedding.continuous_1"), (7, "past_embedding.continuous_2"))
        for start, prefix in embedding_specs:
            dense(variables[start:start + 2], prefix)
        unknown, known_inputs, observed, statics = result
        activations["static_embedded"] = statics
        historical = reference.concat(([unknown[:, :HISTORY]] if unknown is not None else []) +
                                      [known_inputs[:, :HISTORY], observed[:, :HISTORY]], axis=-1)
        activations["past_embedded"] = tf.gather(tf.transpose(historical, [0, 1, 3, 2]), cpp_to_tf_past, axis=2)
        activations["future_embedded"] = tf.transpose(known_inputs[:, HISTORY:], [0, 1, 3, 2])
        return result

    reference.TemporalFusionTransformer.get_tft_embeddings = observed_embeddings
    original_attention = reference.InterpretableMultiHeadAttention

    class ObservedAttention(original_attention):
        def __call__(self, *positional, **keywords):
            result = super(ObservedAttention, self).__call__(*positional, **keywords)
            dense(self.vs_layers[0].trainable_weights, "attention.value")
            dense(self.w_o.trainable_weights, "attention.output")
            for i in range(HEADS):
                dense(self.qs_layers[i].trainable_weights, "attention.query_{}".format(i))
                dense(self.ks_layers[i].trainable_weights, "attention.key_{}".format(i))
            activations["attention_output"] = result[0]
            return result

    reference.InterpretableMultiHeadAttention = ObservedAttention
    original_lstm = tf.keras.layers.LSTM
    lstms = []

    def observed_lstm(*positional, **keywords):
        layer = original_lstm(*positional, **keywords)
        lstms.append(layer)
        return layer

    tf.keras.layers.LSTM = observed_lstm
    params = dict(total_time_steps=HISTORY + HORIZON, input_size=6 if mixed else 5, output_size=1,
                  category_counts="[3,4]" if mixed else json.dumps([args.static_cardinality]), multiprocessing_workers=1, input_obs_loc="[0]",
                  static_input_loc="[3,4]" if mixed else "[4]",
                  known_regular_inputs="[2,3]" if mixed else "[1,2,3]",
                  known_categorical_inputs="[0,1]" if mixed else "[0]",
                  column_definition=[], hidden_layer_size=HIDDEN, dropout_rate=0.,
                  max_gradient_norm=0.01, learning_rate=0.001, minibatch_size=BATCH,
                  num_epochs=1, early_stopping_patience=1, num_encoder_steps=HISTORY,
                  stack_size=1, num_heads=HEADS, model_folder=str(output / "reference_model"))
    model = reference.TemporalFusionTransformer(params, use_cudnn=False)
    tf.keras.layers.LSTM = original_lstm
    if grn_count[0] != len(grn_specs):
        raise RuntimeError("Not all GRNs visited")
    for layer, name in zip(lstms, ("encoder", "decoder")):
        kernel, recurrent, bias = layer.trainable_weights
        add(kernel, name + ".weight_ih_l0", lambda x: x.T)
        add(recurrent, name + ".weight_hh_l0", lambda x: x.T)
        add(bias, name + ".bias_ih_l0")
        activations[name] = layer.output[0] if isinstance(layer.output, list) else layer.output
        activations["past_selected" if name == "encoder" else "future_selected"] = layer.input[0]
    known = {v.name for v, _, _ in mappings}
    remaining = [v for v in model.model.trainable_weights if v.name not in known]
    dense(remaining, "projection")
    if len(set(n for _, n, _ in mappings)) != len(mappings):
        raise RuntimeError("Duplicate C++ parameter mapping")
    if {v.name for v, _, _ in mappings} != {v.name for v in model.model.trainable_weights}:
        raise RuntimeError("Mapping must cover every reference parameter exactly once")

    # Use explicit matched values, independent of each framework's initializer.
    initializers = []
    for var, name, _ in mappings:
        shape = var.shape.as_list()
        if name.endswith("norm.weight"):
            value = 1. + rng.normal(0., .06, shape)
        elif name.endswith("norm.bias") or name.endswith(".bias") or "bias_ih" in name:
            value = rng.normal(0., .03, shape)
        else:
            value = rng.normal(0., .17, shape)
        initializers.append(tf.assign(var, value.astype(np.float32)))

    inputs = rng.normal(0., .7, (BATCH, HISTORY + HORIZON, params["input_size"])).astype(np.float32)
    if mixed:
        inputs[:, :, 3] = rng.normal(0., .7, (BATCH, 1))
        inputs[:, :, 4] = np.array([1, 1, 2])[:, None]
        inputs[:, :, 5] = rng.randint(0, 4, (BATCH, HISTORY + HORIZON))
    else:
        inputs[:, :, 4] = 7  # repeated category exercises native sparse clipping
    targets = rng.normal(0., .7, (BATCH, HORIZON)).astype(np.float32)
    target_placeholder = tf.placeholder(tf.float32, [BATCH, HORIZON, 3])
    # Execute the loss compiled by the original model. Reduce its complete-window
    # loss over samples/time; targets repeat for each quantile as upstream fit().
    loss = tf.reduce_mean(model.model.loss(target_placeholder, model.model.output))
    raw_gradients = tf.gradients(loss, [v for v, _, _ in mappings])
    gradients = [tf.convert_to_tensor(g) for g in raw_gradients]
    input_gradient = tf.gradients(loss, model._input_placeholder)[0]

    # Actual native Keras Adam, including sparse IndexedSlices clipping before
    # duplicate category contributions are summed into table gradients.
    optimizer = tf.keras.optimizers.Adam(lr=.001, clipnorm=.01, epsilon=1e-7)
    updates = optimizer.get_updates(loss, [v for v, _, _ in mappings])
    raw_norms = [tf.norm(g.values if isinstance(g, tf.IndexedSlices) else g) for g in raw_gradients]
    feed = {model._input_placeholder: inputs,
            target_placeholder: np.repeat(targets[..., None], 3, axis=-1),
            tf.keras.backend.learning_phase(): 0}
    session.run(tf.global_variables_initializer())
    if args.initialization == "mapped":
        session.run(initializers)
    tensors = {"predictions": model.model.output,
               "static_weights": model._attention_components["static_flags"],
               "past_weights": model._attention_components["historical_flags"],
               "future_weights": model._attention_components["future_flags"],
               "attention_weights": model._attention_components["decoder_self_attn"],
               "loss": loss}
    values, parameter_values, gradient_values, input_gradient_value, intermediates, gradient_norms = session.run(
        [tensors, [v for v, _, _ in mappings], gradients, input_gradient, activations, raw_norms], feed)
    fixture = Fixture(output)
    np.savez(str(output / "original_tf_weights.npz"), **{
        var.name: value for (var, _, _), value in zip(mappings, parameter_values)})
    past_continuous_order = [1, 2, 0] if mixed else [1, 2, 3, 0]
    future_continuous_order = [2] if mixed else [1, 2, 3]
    fixture.add("input", "static_continuous", inputs[:, 0, 3:4] if mixed else inputs[:, 0, :0])
    fixture.add("input", "static_categorical", inputs[:, 0, 4:5].astype(np.int64))
    fixture.add("input", "past_continuous", inputs[:, :HISTORY, :][:, :, past_continuous_order])
    fixture.add("input", "past_categorical", inputs[:, :HISTORY, 5:6].astype(np.int64) if mixed else inputs[:, :HISTORY, :0].astype(np.int64))
    fixture.add("input", "future_continuous", inputs[:, HISTORY:, :][:, :, future_continuous_order])
    fixture.add("input", "future_categorical", inputs[:, HISTORY:, 5:6].astype(np.int64) if mixed else inputs[:, HISTORY:, :0].astype(np.int64))
    fixture.add("input", "targets", targets)
    values["past_weights"] = values["past_weights"][..., cpp_to_tf_past]
    values["attention_weights"] = values["attention_weights"].transpose(1, 0, 2, 3)[:, :, HISTORY:, :]
    for name, value in values.items():
        fixture.add("output", name, value)
    for name, value in intermediates.items():
        if name in ("attention_output", "attention_skip", "processed", "final"):
            value = value[:, HISTORY:, :]
        fixture.add("activation", name, value)
    fixture.add("input_gradient", "static_continuous", input_gradient_value[:, 0, 3:4] if mixed else input_gradient_value[:, 0, :0])
    fixture.add("input_gradient", "past_continuous", input_gradient_value[:, :HISTORY, :][:, :, past_continuous_order])
    fixture.add("input_gradient", "future_continuous", input_gradient_value[:, HISTORY:, :][:, :, future_continuous_order])
    for (var, name, transform), value, gradient, gradient_norm in zip(mappings, parameter_values, gradient_values, gradient_norms):
        fixture.add("parameter", name, transform(value))
        fixture.add("gradient", name, transform(gradient))
        fixture.add("gradient_norm", name, gradient_norm)
    for name in ("encoder", "decoder"):
        fixture.add("frozen_parameter", "network." + name + ".bias_hh_l0", np.zeros(HIDDEN * 4))
    session.run(updates, feed)
    updated = session.run([v for v, _, _ in mappings])
    for (_, name, transform), value in zip(mappings, updated):
        fixture.add("updated_parameter", name, transform(value))
    # Change which category IDs occur. Previously visited rows become absent,
    # exposing sparse Adam's full-table momentum decay/update on later steps.
    for step in (2, 3):
        prefix = "step{}_".format(step)
        if mixed:
            inputs[:, :, 4] = (0 if step == 2 else 2)
            inputs[:, :, 5] = (0 if step == 2 else 3)
        else:
            inputs[:, :, 4] = (11 if step == 2 else 7)
        targets = np.roll(targets, 1, axis=1) * -.8
        feed[model._input_placeholder] = inputs
        feed[target_placeholder] = np.repeat(targets[..., None], 3, axis=-1)
        values, gradient_values, gradient_norms = session.run([tensors, gradients, raw_norms], feed)
        fixture.add(prefix + "input", "static_continuous", inputs[:, 0, 3:4] if mixed else inputs[:, 0, :0])
        fixture.add(prefix + "input", "static_categorical", inputs[:, 0, 4:5].astype(np.int64))
        fixture.add(prefix + "input", "past_continuous", inputs[:, :HISTORY, :][:, :, past_continuous_order])
        fixture.add(prefix + "input", "past_categorical", inputs[:, :HISTORY, 5:6].astype(np.int64) if mixed else inputs[:, :HISTORY, :0].astype(np.int64))
        fixture.add(prefix + "input", "future_continuous", inputs[:, HISTORY:, :][:, :, future_continuous_order])
        fixture.add(prefix + "input", "future_categorical", inputs[:, HISTORY:, 5:6].astype(np.int64) if mixed else inputs[:, HISTORY:, :0].astype(np.int64))
        fixture.add(prefix + "input", "targets", targets)
        values["past_weights"] = values["past_weights"][..., cpp_to_tf_past]
        values["attention_weights"] = values["attention_weights"].transpose(1, 0, 2, 3)[:, :, HISTORY:, :]
        for name, value in values.items():
            fixture.add(prefix + "output", name, value)
        for (_, name, transform), gradient, gradient_norm in zip(mappings, gradient_values, gradient_norms):
            fixture.add(prefix + "gradient", name, transform(gradient))
            fixture.add(prefix + "gradient_norm", name, gradient_norm)
        session.run(updates, feed)
        updated = session.run([v for v, _, _ in mappings])
        for (_, name, transform), value in zip(mappings, updated):
            fixture.add(prefix + "updated_parameter", name, transform(value))
    # A separate controlled optimizer fixture isolates clipping and epsilon
    # semantics from accumulated floating-point differences in the TFT graph.
    before_probe = set(v.name for v in tf.global_variables())
    probe_variables = {
        "dense_large": tf.Variable([.2, -.3], dtype=tf.float32, use_resource=True),
        "dense_tiny": tf.Variable([.1, -.2], dtype=tf.float32, use_resource=True),
        "sparse": tf.Variable(np.arange(8, dtype=np.float32).reshape(4, 2) * .03, use_resource=True)}
    large_gradient = tf.placeholder(tf.float32, [2])
    tiny_gradient = tf.placeholder(tf.float32, [2])
    sparse_ids = tf.placeholder(tf.int32, [3])
    sparse_values = tf.placeholder(tf.float32, [3, 2])
    probe_loss = (tf.reduce_sum(probe_variables["dense_large"] * large_gradient) +
                  tf.reduce_sum(probe_variables["dense_tiny"] * tiny_gradient) +
                  tf.reduce_sum(tf.nn.embedding_lookup(probe_variables["sparse"], sparse_ids) * sparse_values))
    probe_optimizer = tf.keras.optimizers.Adam(lr=.001, clipnorm=.01, epsilon=1e-7)
    probe_updates = probe_optimizer.get_updates(probe_loss, list(probe_variables.values()))
    session.run(tf.variables_initializer([v for v in tf.global_variables() if v.name not in before_probe]))
    for name, value in session.run(probe_variables).items():
        fixture.add("optimizer_probe_initial", name, value)
    for step in (1, 2, 3):
        prefix = "optimizer_probe{}_".format(step)
        big = np.array([3., -4.], dtype=np.float32) * (-1 if step == 2 else 1)
        tiny = np.array([1e-9, -2e-8], dtype=np.float32) * step
        indices = np.array([1, 1, 2] if step != 2 else [0, 0, 3], dtype=np.int32)
        values = np.array([[.3, -.4], [-.1, .2], [.05, .08]], dtype=np.float32) * step
        for name, value in (("dense_large", big), ("dense_tiny", tiny), ("sparse_values", values), ("sparse_indices", indices)):
            fixture.add(prefix + "gradient", name, value)
        session.run(probe_updates, {large_gradient: big, tiny_gradient: tiny,
                                   sparse_ids: indices, sparse_values: values})
        for name, value in session.run(probe_variables).items():
            fixture.add(prefix + "updated_parameter", name, value)
    probe_predictions = np.array([[[-1., 0., 1.], [2., 2., 2.], [-2., -1., 0.]],
                                  [[5.0001, 5., 4.999], [-.1, 0., .1], [.3, .2, .1]]], dtype=np.float32)
    probe_targets = np.array([[0., 2., -1.], [5., 0., .2]], dtype=np.float32)
    probe_prediction_placeholder = tf.placeholder(tf.float32, probe_predictions.shape)
    probe_target_placeholder = tf.placeholder(tf.float32, probe_predictions.shape)
    probe_loss = tf.reduce_mean(model.model.loss(probe_target_placeholder, probe_prediction_placeholder))
    probe_loss_gradient = tf.gradients(probe_loss, probe_prediction_placeholder)[0]
    loss_value, loss_gradient = session.run([probe_loss, probe_loss_gradient], {
        probe_prediction_placeholder: probe_predictions,
        probe_target_placeholder: np.repeat(probe_targets[..., None], 3, axis=-1)})
    for name, value in (("predictions", probe_predictions), ("targets", probe_targets),
                        ("loss", loss_value), ("prediction_gradient", loss_gradient)):
        fixture.add("loss_probe", name, value)
    mapping_metadata = [{"tensorflow": v.name, "cpp": name, "tf_shape": v.shape.as_list(),
                         "gradient_type": type(g).__name__}
                        for (v, name, _), g in zip(mappings, raw_gradients)]
    fixture.finish({
        "schema": 1, "preset": args.preset, "reference_commit": REFERENCE_COMMIT, "reference_sha256": digest,
        "original_tf_weights_sha256": hashlib.sha256((output / "original_tf_weights.npz").read_bytes()).hexdigest(),
        "harness_sha256": harness_sha256,
        "tensorflow": tf.__version__, "numpy": np.__version__, "seed": args.seed, "initialization": args.initialization,
        "device": "CPU", "dtype": "float32", "dropout": 0., "layer_norm_epsilon": .001,
        "hidden_size": HIDDEN, "attention_heads": HEADS, "history": HISTORY, "horizon": HORIZON,
        "static_cardinality": 3 if mixed else args.static_cardinality,
        "batch_size": BATCH, "atol": 2e-5, "rtol": 2e-4,
        "optimizer_atol": 2e-6, "optimizer_rtol": 2e-4,
        "optimizer": {"type": "TensorFlow 1.15 Keras Adam", "learning_rate": .001,
                      "beta1": .9, "beta2": .999, "epsilon": 1e-7, "clipnorm": .01,
                      "scope": "three native steps with changing/repeated IDs, IndexedSlices clipping and sparse momentum"},
        "optimizer_probe": "Independent three-step dense-large, dense-tiny and repeated-ID sparse update fixture",
        "parameter_mapping": mapping_metadata})
    config = dict(hidden_size=HIDDEN, attention_heads=HEADS, static_continuous=1 if mixed else 0,
                  past_continuous=3 if mixed else 4, future_continuous=1 if mixed else 3,
                  static_categorical_cardinalities="3" if mixed else str(args.static_cardinality),
                  past_categorical_cardinalities="4" if mixed else "",
                  future_categorical_cardinalities="4" if mixed else "",
                  future_continuous_past_indices="1" if mixed else "0,1,2",
                  future_categorical_past_indices="0" if mixed else "")
    (output / "config.tsv").write_text("".join("{}\t{}\n".format(k, v) for k, v in config.items()))
    print("Exported {} mapped parameters and {} tensors to {}".format(len(mappings), len(fixture.rows), output))


if __name__ == "__main__":
    main()
