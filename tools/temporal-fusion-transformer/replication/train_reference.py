#!/usr/bin/env python3
"""Train pinned original TF1.15 TFT using shared memory-mapped Electricity data.

Adaptation: stream indexed windows, apply persisted explicit epoch permutations,
and run one Keras epoch at a time. The original graph, compiled loss and Adam are
unchanged. Manual callbacks match original stopping versus checkpoint semantics.
--smoke and --profile-batches never produce a benchmark-replication claim.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import sys
import time
import traceback
import uuid

import numpy as np

sys.dont_write_bytecode = True
import prepare_electricity as preparation


def atomic_json(path, value):
    path = Path(path)
    temporary = path.with_name(path.name + ".tmp-" + uuid.uuid4().hex)
    preparation.write_json(temporary, value)
    os.replace(str(temporary), str(path))


def peak_rss_bytes():
    import resource
    value = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return int(value if sys.platform == "darwin" else value * 1024)


class Dataset:
    def __init__(self, directory, seed):
        self.directory = Path(directory)
        self.metadata = preparation.verify_export(self.directory)
        self.protocol = self.metadata["protocol"]
        self.arrays = {}
        for name, spec in self.metadata["arrays"].items():
            self.arrays[name] = np.memmap(str(self.directory / spec["file"]), mode="r",
                                          dtype=spec["dtype"], shape=tuple(spec["shape"]))
        self.seed = seed
        self.train = self.arrays["train_windows_{}".format(seed)]
        self.valid = self.arrays["valid_windows_{}".format(seed)]
        self.test = self.arrays["test_windows"]
        self.history, self.horizon = self.protocol["history"], self.protocol["horizon"]
        self.total = self.history + self.horizon
        self.batch_size = self.protocol["training"]["batch_size"]

    def batch(self, windows):
        entity, starts = windows[:, 0], windows[:, 1]
        offsets = starts[:, None] + np.arange(self.total, dtype=np.int64)
        features = self.arrays["features"][offsets]
        categories = self.arrays["entity_categories"][entity]
        inputs = np.empty((len(windows), self.total, 5), dtype=np.float32)
        inputs[:, :, :4] = features
        inputs[:, :, 4] = categories[:, None]
        targets = features[:, self.history:, 0]
        labels = np.repeat(targets[:, :, None], 3, axis=-1)
        sample_weights = np.ones(targets.shape, dtype=np.float32)
        return inputs, labels, sample_weights


def epoch_order(directory, dataset, epoch, count):
    """Persist numpy MT19937 permutations shared verbatim with C++ training."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    description = {
        "schema_version": 1, "seed": dataset.seed, "window_count": count,
        "training_manifest_sha256": dataset.metadata["arrays"]["train_windows_{}".format(dataset.seed)]["sha256"],
        "rng": "numpy.random.RandomState(((seed ^ 0x51F7) + zero_based_epoch) % 2**32).permutation(count)",
        "dtype": "<i8", "base": "ascending row indices of the persisted training-window manifest"}
    receipt = directory / "shuffle.json"
    if receipt.exists():
        if json.loads(receipt.read_text(encoding="utf-8")) != description:
            raise ValueError("Shared order directory belongs to a different dataset or sampling protocol")
    else:
        preparation.write_json(receipt, description)
    tsv = directory / "shuffle.tsv"
    tsv_text = "".join("{}\t{}\n".format(key, value) for key, value in description.items())
    if tsv.exists():
        if tsv.read_text(encoding="utf-8") != tsv_text:
            raise ValueError("Existing shuffle TSV metadata differs; preserved")
    else:
        with open(str(tsv), "x", encoding="utf-8", newline="\n") as handle:
            handle.write(tsv_text)
    rng_seed = ((dataset.seed ^ 0x51F7) + epoch) % (2 ** 32)
    order = np.asarray(np.random.RandomState(rng_seed).permutation(count), dtype="<i8")
    path = directory / "epoch-{:04d}.i64.bin".format(epoch)
    expected_hash = hashlib.sha256(order.tobytes(order="C")).hexdigest()
    if path.exists():
        if path.stat().st_size != count * 8 or preparation.sha256(path) != expected_hash:
            raise ValueError("Existing shared epoch order differs; preserved: " + str(path))
    else:
        temporary = path.with_name(path.name + ".tmp-" + uuid.uuid4().hex)
        with open(str(temporary), "xb") as handle:
            order.tofile(handle)
        if path.exists():
            raise FileExistsError(str(path))
        temporary.rename(path)
    return order, {"file": str(path.resolve()), "sha256": expected_hash, "epoch_index": epoch,
                   "rng_seed": rng_seed, "count": count}


class EarlyStop:
    def __init__(self, min_delta, patience):
        self.min_delta, self.patience = min_delta, patience
        self.stopping_best = float("inf")
        self.checkpoint_best = float("inf")
        self.wait = 0

    def update(self, loss):
        if not math.isfinite(loss):
            raise ValueError("Non-finite validation loss")
        checkpoint = loss < self.checkpoint_best
        if checkpoint:
            self.checkpoint_best = loss
        if loss < self.stopping_best - self.min_delta:
            self.stopping_best, self.wait = loss, 0
        else:
            self.wait += 1
        return checkpoint, self.wait >= self.patience


class RiskAccumulator:
    def __init__(self, horizon, quantiles):
        self.quantiles = np.asarray(quantiles, dtype=np.float64)
        self.loss = np.zeros((horizon, len(quantiles)), dtype=np.float64)
        self.denominator = np.zeros(horizon, dtype=np.float64)
        self.count = 0

    def add(self, targets, predictions):
        targets, predictions = np.asarray(targets, dtype=np.float64), np.asarray(predictions, dtype=np.float64)
        if (predictions.shape != targets.shape + (len(self.quantiles),)
                or not np.isfinite(targets).all() or not np.isfinite(predictions).all()):
            raise ValueError("Invalid forecast batch")
        error = targets[:, :, None] - predictions
        self.loss += np.maximum(self.quantiles * error, (self.quantiles - 1.0) * error).sum(axis=0)
        self.denominator += np.abs(targets).sum(axis=0)
        self.count += len(targets)

    def report(self):
        if not self.count or np.any(self.denominator <= 0):
            raise ValueError("Normalized risks are undefined for empty or all-zero horizons")
        result = {}
        for index, quantile in enumerate(self.quantiles):
            risks = 2.0 * self.loss[:, index] / self.denominator
            result["p{:02d}".format(int(round(quantile * 100)))] = {
                "paper_pooled": float(2.0 * self.loss[:, index].sum() / self.denominator.sum()),
                "reference_horizon_mean": float(risks.mean()), "by_horizon": risks.tolist()}
        return {"windows": self.count, "quantiles": result}


def evaluate_windows(model, data, windows, output, split):
    quantiles = data.protocol["quantiles"]
    scores = {name: RiskAccumulator(data.horizon, quantiles)
              for name in ("tft", "persistence", "seasonal_naive_24h")}
    prediction_path = output / (split + "_predictions.normalized.f32.bin")
    started = time.perf_counter()
    with open(str(prediction_path), "xb") as handle:
        for first in range(0, len(windows), data.batch_size):
            selected = windows[first:first + data.batch_size]
            entity, starts = selected[:, 0], selected[:, 1]
            normalized = np.asarray(model.predict_on_batch(data.batch(selected)[0]), dtype="<f4")
            normalized.tofile(handle)
            prediction = preparation.inverse_targets(normalized, entity, data.arrays["target_mean_scale"])
            offsets = starts[:, None] + data.history + np.arange(data.horizon)
            targets = np.asarray(data.arrays["raw_targets"][offsets], dtype=np.float64)
            scores["tft"].add(targets, prediction)
            last = data.arrays["raw_targets"][starts + data.history - 1]
            scores["persistence"].add(targets, np.broadcast_to(last[:, None, None], prediction.shape))
            # Protocol horizon is 24, so each future hour repeats yesterday's matching hour.
            yesterday = data.arrays["raw_targets"][offsets - 24]
            scores["seasonal_naive_24h"].add(targets, np.repeat(yesterday[:, :, None], len(quantiles), axis=2))
    return {"split": split, "elapsed_seconds": time.perf_counter() - started,
            "metrics": {name: value.report() for name, value in scores.items()},
            "predictions": {"file": prediction_path.name, "dtype": "<f4",
                            "shape": [len(windows), data.horizon, len(quantiles)],
                            "scale": "normalized; apply target_mean_scale indexed by evaluation window entity",
                            "sha256": preparation.sha256(prediction_path)}}


def make_sequence(tf, data, windows, order):
    class IndexedSequence(tf.keras.utils.Sequence):
        def __len__(self):
            return (len(order) + data.batch_size - 1) // data.batch_size

        def __getitem__(self, batch_index):
            rows = order[batch_index * data.batch_size:(batch_index + 1) * data.batch_size]
            return data.batch(windows[rows])
    return IndexedSequence()


def weighted_validation(model, sequence):
    """Match TF1.15 array evaluation, not its equal-batch generator aggregation.

    tensorflow/v1.15.0/.../keras/engine/training_generator.py passes use_steps=True
    to MetricsAggregator. training_arrays.py uses sample counts for array inputs.
    All TFT temporal sample weights here are one, so weighting each batch loss by
    its actual window count exactly restores the original array reduction.
    """
    total, count = 0.0, 0
    for index in range(len(sequence)):
        inputs, labels, weights = sequence[index]
        loss = float(np.asarray(model.test_on_batch(inputs, labels, sample_weight=weights,
                                                    reset_metrics=True)).reshape(-1)[0])
        total += loss * len(inputs)
        count += len(inputs)
    if not count:
        raise ValueError("Validation cannot be empty")
    return total / count


def training_loss_callback(tf):
    class SampleWeightedLoss(tf.keras.callbacks.Callback):
        def on_epoch_begin(self, epoch, logs=None):
            self.total, self.count = 0.0, 0
            self.batch_count, self.steady_seconds = 0, 0.0

        def on_batch_begin(self, batch, logs=None):
            self.batch_started = time.perf_counter()

        def on_batch_end(self, batch, logs=None):
            self.total += float(logs["loss"]) * int(logs["size"])
            self.count += int(logs["size"])
            now = time.perf_counter()
            if self.batch_count == 0:
                self.first_batch_including_compile_seconds = now - self.fit_started
            else:
                self.steady_seconds += now - self.batch_started
            self.batch_count += 1
    return SampleWeightedLoss()


def verify_batch_weighting():
    """Small actual-TF fixture catches a last batch weighted as a full batch."""
    os.environ["CUDA_VISIBLE_DEVICES"] = ""
    import tensorflow as tf
    if not tf.__version__.startswith("1.15."):
        raise RuntimeError("Reduction fixture requires TensorFlow 1.15.x")
    tf.reset_default_graph()
    with tf.Session(config=tf.ConfigProto(device_count={"GPU": 0},
                                          intra_op_parallelism_threads=1,
                                          inter_op_parallelism_threads=1)) as session:
        tf.keras.backend.set_session(session)
        inputs = tf.keras.layers.Input(shape=(1,))
        model = tf.keras.Model(inputs, tf.keras.layers.Lambda(lambda value: value * 0)(inputs))
        model.compile(optimizer=tf.keras.optimizers.Adam(), loss="mean_squared_error")
        session.run(tf.global_variables_initializer())
        x = np.zeros((3, 1), dtype=np.float32)
        y = np.array([[0.], [0.], [3.]], dtype=np.float32)
        weights = np.ones(3, dtype=np.float32)
        class PartialBatch(tf.keras.utils.Sequence):
            def __len__(self):
                return 2

            def __getitem__(self, index):
                sl = slice(index * 2, (index + 1) * 2)
                return x[sl], y[sl], weights[sl]
        sequence = PartialBatch()
        original_array = float(model.evaluate(x, y, sample_weight=weights, batch_size=2, verbose=0))
        naive_generator = float(model.evaluate_generator(sequence, workers=0, verbose=0))
        corrected = weighted_validation(model, sequence)
        if abs(original_array - 3.0) > 1e-6 or abs(corrected - original_array) > 1e-6:
            raise AssertionError("Sample-weighted stream differs from original array evaluation")
        if abs(naive_generator - 4.5) > 1e-6:
            raise AssertionError("Unexpected TF1.15 generator behavior; inspect runtime before benchmark")
        print(json.dumps({"fixture": "partial_batch_weighting", "tensorflow": tf.__version__,
                          "original_array_loss": original_array, "naive_generator_loss": naive_generator,
                          "corrected_stream_loss": corrected, "passed": True}), flush=True)


def build_model(args, data, output):
    # Set visibility before importing the legacy runtime.
    os.environ["CUDA_VISIBLE_DEVICES"] = "" if args.device == "cpu" else str(args.gpu_id)
    os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")
    import tensorflow as tf
    if not tf.__version__.startswith("1.15."):
        raise RuntimeError("Reference trainer requires TensorFlow 1.15.x")
    if args.device == "gpu" and not tf.test.is_gpu_available(cuda_only=True):
        raise RuntimeError("GPU requested but unavailable to the original TensorFlow runtime")
    reference_data = preparation.load_reference(args.reference_root, data.protocol)
    sys.path.insert(0, str(Path(args.reference_root).resolve()))
    import libs.tft_model as reference

    tf.reset_default_graph()
    tf.set_random_seed(args.seed)
    np.random.seed(args.seed)
    config = tf.ConfigProto(intra_op_parallelism_threads=args.threads,
                            inter_op_parallelism_threads=1,
                            device_count={"GPU": 0} if args.device == "cpu" else {})
    config.gpu_options.allow_growth = True
    session = tf.Session(config=config)
    tf.keras.backend.set_session(session)
    formatter = reference_data.formatter_class()
    formatter._num_classes_per_cat_input = [int(np.max(data.arrays["entity_categories"])) + 1]
    parameters = formatter.get_experiment_params()
    parameters.update(formatter.get_default_model_params())
    parameters["model_folder"] = str(output / "reference_internal")
    model = reference.TemporalFusionTransformer(parameters, use_cudnn=False)
    session.run(tf.global_variables_initializer())
    variables = {variable.name: variable for variable in tf.trainable_variables()}
    with np.load(str(args.initial_weights), allow_pickle=False) as initial:
        if set(initial.files) != set(variables):
            raise ValueError("Initial weight names mismatch; missing={}, extra={}".format(
                sorted(set(variables) - set(initial.files)), sorted(set(initial.files) - set(variables))))
        assignments = []
        for name, variable in variables.items():
            value = initial[name]
            if list(value.shape) != variable.shape.as_list() or not np.isfinite(value).all():
                raise ValueError("Invalid initial tensor: " + name)
            assignments.append(variable.assign(value))
        session.run(assignments)
    return tf, session, model.model, parameters


def train(args, data, output):
    started = time.perf_counter()
    output.mkdir(parents=True, exist_ok=False)
    report = {"schema_version": 1, "status": "initializing", "implementation": "original_tensorflow",
              "seed": args.seed, "is_smoke": args.smoke, "profile_batches": args.profile_batches,
              "benchmark_protocol_complete": False,
              "device": args.device, "threads": args.threads,
              "reference_revision": data.protocol["reference_revision"],
              "data_metadata_sha256": preparation.sha256(data.directory / "metadata.json"),
              "initial_weights_sha256": preparation.sha256(args.initial_weights),
              "trainer_sha256": preparation.sha256(__file__), "epochs": [],
              "adaptations": ["Indexed float32 window batches, original model/compiled Adam and loss",
                              "Persisted NumPy epoch permutations; Keras shuffle disabled",
                              "One fit_generator epoch at a time; explicit original callback semantics",
                              "Training and validation losses weighted by actual batch window counts, matching original arrays",
                              "use_cudnn=False matches shared original LSTM weight layout on CPU/GPU",
                              "Forecast inverse scaling in float64 via entity mean/scale; original raw float64 targets"]}
    atomic_json(output / "progress.json", report)
    session = None
    try:
        initial_sidecar = args.initial_weights.with_name("metadata.json")
        if initial_sidecar.is_file():
            initial = json.loads(initial_sidecar.read_text(encoding="utf-8"))
            report["initialization"] = {"kind": initial.get("initialization"), "preset": initial.get("preset"),
                                        "seed": initial.get("seed"), "metadata_sha256": preparation.sha256(initial_sidecar),
                                        "reference_revision": initial.get("reference_commit"),
                                        "weights_sha256": report["initial_weights_sha256"]}
        tf, session, model, parameters = build_model(args, data, output)
        report["runtime"] = {"python": platform.python_version(), "numpy": np.__version__, "tensorflow": tf.__version__}
        report["startup_seconds"] = time.perf_counter() - started
        preparation.write_json(output / "reference_parameters.json", parameters)
        training = data.train[:args.smoke_train_windows] if args.smoke else data.train
        validation = data.valid[:args.smoke_valid_windows] if args.smoke else data.valid
        development = args.smoke or bool(args.profile_batches)
        evaluation = data.valid[:args.smoke_evaluation_windows] if development else data.test
        evaluation_split = "validation" if development else "test"
        report["windows"] = {"train": len(training), "valid": len(validation), "evaluation": len(evaluation)}
        report["evaluation_split"] = evaluation_split
        if args.profile_batches:
            count = min(len(training), args.profile_batches * data.batch_size)
            order, order_record = epoch_order(args.order_dir, data, 0, len(training))
            sequence = make_sequence(tf, data, training, order[:count])
            profile_started = time.perf_counter()
            model.train_on_batch(*sequence[0])
            first_batch_seconds = time.perf_counter() - profile_started
            steady_started = time.perf_counter()
            for index in range(1, len(sequence)):
                model.train_on_batch(*sequence[index])
            steady_seconds = time.perf_counter() - steady_started
            elapsed = first_batch_seconds + steady_seconds
            report.update(status="profile_complete", elapsed_seconds=time.perf_counter() - started,
                          peak_process_rss_bytes=peak_rss_bytes(),
                          profile={"batches": len(sequence), "windows": count, "training_seconds": elapsed,
                                   "seconds_per_batch": elapsed / len(sequence), "order": order_record,
                                   "first_batch_including_compile_seconds": first_batch_seconds,
                                   "steady_batches": max(0, len(sequence) - 1),
                                   "steady_seconds_per_batch": steady_seconds / (len(sequence) - 1) if len(sequence) > 1 else None,
                                   "test_evaluated": False})
            atomic_json(output / "report.json", report)
            atomic_json(output / "progress.json", report)
            return report

        stopping = EarlyStop(data.protocol["training"]["early_stopping_min_delta"],
                             data.protocol["training"]["early_stopping_patience"])
        selected_epoch = None
        best_path = output / "best.weights.h5"
        valid_sequence = make_sequence(tf, data, validation, np.arange(len(validation)))
        for epoch in range(args.epochs):
            order, order_record = epoch_order(args.order_dir, data, epoch, len(training))
            train_sequence = make_sequence(tf, data, training, order)
            epoch_started = time.perf_counter()
            collector = training_loss_callback(tf)
            collector.fit_started = time.perf_counter()
            model.fit_generator(train_sequence, epochs=1, shuffle=False, callbacks=[collector],
                                workers=0, use_multiprocessing=False, max_queue_size=1, verbose=0)
            training_seconds = time.perf_counter() - collector.fit_started
            if collector.count != len(training):
                raise ValueError("Training did not consume exactly the declared window count")
            train_loss = collector.total / collector.count
            validation_started = time.perf_counter()
            valid_loss = weighted_validation(model, valid_sequence)
            validation_seconds = time.perf_counter() - validation_started
            if not math.isfinite(train_loss):
                raise ValueError("Non-finite training loss")
            checkpoint, stop = stopping.update(valid_loss)
            if checkpoint:
                temporary = output / ("best.weights.tmp-" + uuid.uuid4().hex + ".h5")
                model.save_weights(str(temporary))
                os.replace(str(temporary), str(best_path))
                selected_epoch = epoch
            record = {"epoch_index": epoch, "train_loss": train_loss, "validation_loss": valid_loss,
                      "training_seconds": training_seconds, "validation_seconds": validation_seconds,
                      "training_batches": collector.batch_count,
                      "first_training_batch_including_compile_seconds": collector.first_batch_including_compile_seconds,
                      "steady_training_batches": max(0, collector.batch_count - 1),
                      "steady_training_seconds_per_batch": (collector.steady_seconds / (collector.batch_count - 1)
                                                            if collector.batch_count > 1 else None),
                      "elapsed_seconds": time.perf_counter() - epoch_started, "checkpoint_selected": checkpoint,
                      "stopping_wait": stopping.wait, "stop": stop, "order": order_record}
            report["epochs"].append(record)
            with open(str(output / "epochs.jsonl"), "a", encoding="utf-8") as handle:
                handle.write(json.dumps(record, sort_keys=True, allow_nan=False) + "\n")
            report.update(status="training", selected_epoch_index=selected_epoch,
                          best_validation_loss=stopping.checkpoint_best,
                          peak_process_rss_bytes=peak_rss_bytes(),
                          elapsed_seconds=time.perf_counter() - started)
            atomic_json(output / "progress.json", report)
            print(json.dumps(record, sort_keys=True), flush=True)
            if stop:
                break
        model.load_weights(str(best_path))
        report.update(status="evaluating", checkpoint={"file": best_path.name,
                      "sha256": preparation.sha256(best_path), "contents": "selected model weights; optimizer state not included"})
        atomic_json(output / "progress.json", report)
        report["evaluation"] = evaluate_windows(model, data, evaluation, output, evaluation_split)
        report.update(status="complete", elapsed_seconds=time.perf_counter() - started,
                      peak_process_rss_bytes=peak_rss_bytes(),
                      benchmark_protocol_complete=not args.smoke and args.epochs == data.protocol["training"]["max_epochs"])
        atomic_json(output / "report.json", report)
        atomic_json(output / "progress.json", report)
        return report
    except Exception as error:
        report.update(status="failed", error_type=type(error).__name__, error=str(error),
                      peak_process_rss_bytes=peak_rss_bytes(),
                      elapsed_seconds=time.perf_counter() - started)
        atomic_json(output / "progress.json", report)
        (output / "failure.txt").write_text(traceback.format_exc(), encoding="utf-8")
        raise
    finally:
        if session is not None:
            session.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path)
    parser.add_argument("--reference-root", type=Path, default=Path(".build/reference/tft"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--seed", type=int)
    parser.add_argument("--initial-weights", type=Path)
    parser.add_argument("--order-dir", type=Path)
    parser.add_argument("--epochs", type=int, default=100)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--device", choices=["cpu", "gpu"], default="cpu")
    parser.add_argument("--gpu-id", type=int, default=0)
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--smoke-train-windows", type=int, default=128)
    parser.add_argument("--smoke-valid-windows", type=int, default=64)
    parser.add_argument("--smoke-evaluation-windows", type=int, default=64,
                        help="Validation-only scoring cap; smoke never evaluates the test set")
    parser.add_argument("--profile-batches", type=int, default=0)
    parser.add_argument("--prepare-orders-only", action="store_true")
    parser.add_argument("--verify-batch-weighting", action="store_true",
                        help="Run a tiny original-TF partial-batch fixture; no dataset or TFT training")
    args = parser.parse_args()
    if args.verify_batch_weighting:
        verify_batch_weighting()
        return
    if args.data is None or args.seed is None:
        parser.error("--data and --seed are required except for --verify-batch-weighting")
    if min(args.epochs, args.threads, args.smoke_train_windows, args.smoke_valid_windows, args.smoke_evaluation_windows) < 1:
        parser.error("Counts and epochs must be positive")
    if args.profile_batches < 0:
        parser.error("--profile-batches must be nonnegative")
    if not args.prepare_orders_only and (args.output is None or args.initial_weights is None):
        parser.error("Training/profile requires --output and --initial-weights")
    if not args.prepare_orders_only and not args.initial_weights.is_file():
        parser.error("Initial weight archive does not exist")
    data = Dataset(args.data, args.seed)
    if (not args.smoke and not args.profile_batches and not args.prepare_orders_only
            and args.epochs != data.protocol["training"]["max_epochs"]):
        parser.error("Reduced epoch budgets require --smoke; full protocol has 100 maximum epochs")
    if not args.smoke and not args.profile_batches and not args.prepare_orders_only:
        if data.protocol != preparation.read_protocol():
            parser.error("Full runs require the frozen original Electricity protocol, not a development fixture")
        initial_metadata = args.initial_weights.with_name("metadata.json")
        if not initial_metadata.is_file():
            parser.error("Full runs require the original initializer metadata alongside initial weights")
        initial = json.loads(initial_metadata.read_text(encoding="utf-8"))
        if (initial.get("preset") != "electricity" or initial.get("initialization") != "keras"
                or initial.get("seed") != args.seed
                or initial.get("reference_commit") != data.protocol["reference_revision"]
                or initial.get("reference_sha256") != data.protocol["reference_sha256"]["libs/tft_model.py"]):
            parser.error("Full runs require seed-matched Electricity weights from original Keras initializers")
    mode = "smoke-orders" if args.smoke else "orders"
    args.order_dir = args.order_dir or args.data / mode / str(args.seed)
    if args.prepare_orders_only:
        count = min(len(data.train), args.smoke_train_windows) if args.smoke else len(data.train)
        for epoch in range(args.epochs):
            _, record = epoch_order(args.order_dir, data, epoch, count)
            print(json.dumps(record, sort_keys=True), flush=True)
        return
    train(args, data, args.output)


if __name__ == "__main__":
    main()
