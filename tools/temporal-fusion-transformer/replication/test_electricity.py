#!/usr/bin/env python3
"""Deterministic protocol fixtures; no network, real dataset or model training.

python tools/temporal-fusion-transformer/replication/test_electricity.py --reference-root .build/reference/tft
Use --no-reference only for the smaller numpy-only checks; skipped reference
fixtures do not establish protocol parity.
"""

import argparse
import contextlib
import copy
import io
import json
from pathlib import Path
import tempfile
import types
import unittest
import sys

import numpy as np

sys.dont_write_bytecode = True
import prepare_electricity as preparation


REFERENCE_ROOT = None


def read_array(directory, descriptor):
    return np.fromfile(str(Path(directory) / descriptor["file"]), dtype=descriptor["dtype"]).reshape(descriptor["shape"])


def verify_cpp_export(data_directory, cpp_directory, seed, receipt_path=None):
    """Check real C++ loader/metric outputs on two training windows, not test data."""
    data_directory, cpp_directory = Path(data_directory), Path(cpp_directory)
    metadata = preparation.verify_export(data_directory)
    arrays = {name: np.memmap(str(data_directory / item["file"]), mode="r", dtype=item["dtype"],
                              shape=tuple(item["shape"])) for name, item in metadata["arrays"].items()}
    exported = {}
    for line in (cpp_directory / "tensors.tsv").read_text(encoding="utf-8").splitlines():
        name, dtype, shape, filename = line.split("\t")
        if name in exported:
            raise AssertionError("Duplicate C++ fixture tensor: " + name)
        exported[name] = np.fromfile(str(cpp_directory / filename), dtype=dtype).reshape(tuple(map(int, shape.split(","))))
    windows = arrays["train_windows_{}".format(seed)][:2]
    entities, starts = windows[:, 0], windows[:, 1]
    offsets = starts[:, None] + np.arange(192)
    full = arrays["features"][offsets]
    normalized_targets = full[:, 168:, 0]
    predictions = np.repeat((normalized_targets * np.float32(0.9))[:, :, None], 3, axis=2)
    expected = {
        "static_continuous": np.empty((2, 0), dtype="<f4"),
        "static_categorical": arrays["entity_categories"][entities].reshape(2, 1),
        "past_continuous": full[:, :168, [1, 2, 3, 0]],
        "past_categorical": np.empty((2, 168, 0), dtype="<i8"),
        "future_continuous": full[:, 168:, [1, 2, 3]],
        "future_categorical": np.empty((2, 24, 0), dtype="<i8"),
        "targets": normalized_targets, "predictions": predictions,
        "entity_indices": entities, "start_rows": starts}
    if set(exported) != set(expected):
        raise AssertionError("C++ fixture tensor names mismatch")
    for name, value in expected.items():
        np.testing.assert_array_equal(exported[name], value, err_msg=name)
        if exported[name].dtype != value.dtype:
            raise AssertionError("C++ fixture dtype mismatch: " + name)
    targets = np.asarray(arrays["raw_targets"][offsets[:, 168:]], dtype=np.float64)
    model_predictions = preparation.inverse_targets(predictions, entities, arrays["target_mean_scale"])
    last = arrays["raw_targets"][starts + 167]
    forecasts = {"tft": model_predictions,
                 "persistence": np.broadcast_to(last[:, None, None], predictions.shape),
                 "seasonal_naive_24h": np.repeat(arrays["raw_targets"][offsets[:, 144:168]][:, :, None], 3, axis=2)}
    actual_metrics = json.loads((cpp_directory / "metrics.json").read_text(encoding="utf-8"))
    if actual_metrics["seed"] != seed:
        raise AssertionError("C++ fixture seed mismatch")
    maximum_error = 0.0
    for name, values in forecasts.items():
        if actual_metrics[name]["windows"] != 2:
            raise AssertionError("C++ metric fixture window count mismatch")
        for index, quantile in enumerate((0.1, 0.5, 0.9)):
            metric = preparation.quantile_risks(targets, values[:, :, index], quantile)
            actual = actual_metrics[name]["quantiles"]["p{:02d}".format(int(quantile * 100))]
            for key in ("paper_pooled", "reference_horizon_mean", "by_horizon"):
                np.testing.assert_allclose(actual[key], metric[key], rtol=1e-12, atol=1e-12,
                                           err_msg="{} q={} {}".format(name, quantile, key))
                maximum_error = max(maximum_error, float(np.abs(np.asarray(actual[key]) - metric[key]).max()))
    receipt = {"fixture": "cpp_electricity_loader_and_metrics", "passed": True, "seed": seed,
               "split": "training", "windows": 2, "tensors_exact": len(expected),
               "metric_maximum_absolute_error": maximum_error, "metric_rtol": 1e-12, "metric_atol": 1e-12,
               "data_metadata_sha256": preparation.sha256(data_directory / "metadata.json"),
               "cpp_tensor_manifest_sha256": preparation.sha256(cpp_directory / "tensors.tsv"),
               "cpp_metrics_sha256": preparation.sha256(cpp_directory / "metrics.json"),
               "verification_source_sha256": preparation.sha256(__file__)}
    if receipt_path is not None:
        preparation.write_json(receipt_path, receipt)
    print(json.dumps(receipt, sort_keys=True), flush=True)
    return receipt


class IndexedWindowTests(unittest.TestCase):
    def test_hourly_origins_respect_entity_and_label_boundaries(self):
        spans = np.array([[0, 240], [240, 240]], dtype=np.int64)
        hours = np.tile(np.arange(240), 2)
        train = preparation.build_candidates(spans, hours, 0, 6, 72)
        valid = preparation.build_candidates(spans, hours, 4, 8, 72)
        test = preparation.build_candidates(spans, hours, 6, 10, 72)
        self.assertEqual((len(train), len(valid), len(test)), (146, 50, 50))
        for windows, lower, upper in ((train, 2, 6), (valid, 6, 8), (test, 8, 10)):
            for entity, start in windows:
                self.assertGreaterEqual(start, spans[entity, 0])
                self.assertLess(start + 71, spans[entity].sum())
                self.assertGreaterEqual(hours[start + 48], lower * 24)
                self.assertLess(hours[start + 71], upper * 24)
        np.testing.assert_array_equal(valid[:25, 1], np.arange(96, 121))
        broken = hours.copy()
        broken[10] += 1
        with self.assertRaisesRegex(ValueError, "contiguous"):
            preparation.build_candidates(spans, broken, 0, 6, 72)

    def test_sample_order_reproducible_and_without_replacement(self):
        train = np.column_stack((np.zeros(100, dtype=np.int64), np.arange(100)))
        valid = np.column_stack((np.ones(60, dtype=np.int64), np.arange(100, 160)))
        first = preparation.sample_windows(train, valid, 20260918, 20, 15)
        second = preparation.sample_windows(train, valid, 20260918, 20, 15)
        expected_rng = np.random.RandomState(20260918)
        np.testing.assert_array_equal(first[0], train[expected_rng.choice(100, 20, replace=False)])
        np.testing.assert_array_equal(first[1], valid[expected_rng.choice(60, 15, replace=False)])
        for a, b in zip(first, second):
            np.testing.assert_array_equal(a, b)
            self.assertEqual(len(a), len(np.unique(a[:, 1])))
        with self.assertRaisesRegex(ValueError, "Insufficient"):
            preparation.sample_windows(train, valid, 7, 101, 15)

    def test_metric_aggregation_exposes_non_equivalence(self):
        targets = np.array([[1.0, 100.0], [1.0, 100.0]])
        forecasts = np.array([[0.0, 100.0], [0.0, 100.0]])
        result = preparation.quantile_risks(targets, forecasts, 0.5)
        self.assertAlmostEqual(result["paper_pooled"], 1.0 / 101.0)
        self.assertEqual(result["reference_horizon_mean"], 0.5)
        self.assertEqual(result["by_horizon"], [1.0, 0.0])
        with self.assertRaisesRegex(ValueError, "undefined"):
            preparation.quantile_risks(np.zeros((2, 3)), np.ones((2, 3)), 0.5)

    def test_inverse_is_entity_specific_and_keeps_quantile_axis(self):
        normalized = np.array([[[0., 1.], [2., 3.]], [[-1., 0.], [1., 2.]]])
        result = preparation.inverse_targets(normalized, [1, 0], [[2., 3.], [10., 20.]])
        np.testing.assert_array_equal(result[0], [[10., 30.], [50., 70.]])
        np.testing.assert_array_equal(result[1], [[-1., 2.], [5., 8.]])


class OriginalReferenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if REFERENCE_ROOT is None:
            raise unittest.SkipTest("Explicit --no-reference: original-source fixtures were not run")
        cls.reference = preparation.load_reference(REFERENCE_ROOT)

    @staticmethod
    def fixture():
        import pandas as pd
        rows = []
        # Reversed input order exposes the pinned formatter's positional category
        # assignment after groupby sorting. Call it unchanged before export sorting.
        for entity, level in (("MT_002", 100.), ("MT_001", 10.)):
            for hour in range(30 * 24):
                day = hour // 24
                value = level + hour * 0.5 + (10000. if day >= 20 else 0.)
                rows.append((entity, hour, value, hour % 24, day % 7, entity, day))
        return pd.DataFrame(rows, columns=["id", "hours_from_start", "power_usage", "hour",
                                           "day_of_week", "categorical_id", "days_from_start"])

    def batcher(self, formatter):
        instance = types.SimpleNamespace(time_steps=192, num_encoder_steps=168, input_size=5,
                                         output_size=1, column_definition=formatter.get_column_definition())
        for name in ("_get_single_col_by_type", "_batch_sampled_data", "_batch_data"):
            setattr(instance, name, types.MethodType(self.reference.batch_methods[name], instance))
        return instance

    def test_original_scaling_windows_and_binary_export(self):
        raw = self.fixture()
        formatter = self.reference.formatter_class()
        with contextlib.redirect_stdout(io.StringIO()):
            train, valid, test = formatter.split_data(raw, valid_boundary=20, test_boundary=25)
        expected_train_mean = raw.loc[(raw.id == "MT_001") & (raw.days_from_start < 20), "power_usage"].mean()
        self.assertEqual(formatter._target_scaler["MT_001"].mean_[0], expected_train_mean)
        self.assertGreater(raw.loc[raw.id == "MT_001", "power_usage"].mean(), expected_train_mean + 1000.)
        protocol = copy.deepcopy(preparation.read_protocol())
        protocol.update(data_days=[0, 30], validation_boundary_day=20, test_boundary_day=25,
                        seeds=[20260918], training_samples=17, validation_samples=11)
        with tempfile.TemporaryDirectory(prefix="tft-electricity-fixture-") as temporary:
            output = Path(temporary) / "export"
            metadata = preparation.export_dataset(raw, self.reference, output, protocol)
            preparation.verify_export(output)
            arrays = {key: read_array(output, descriptor) for key, descriptor in metadata["arrays"].items()}
            self.assertEqual(metadata["entities"], ["MT_001", "MT_002"])
            # This source-order fixture intentionally preserves the upstream quirk;
            # the official preprocessed dataset is already entity sorted.
            np.testing.assert_array_equal(arrays["entity_categories"], [1, 0])
            self.assertEqual(arrays["features"].dtype.str, "<f4")
            batcher = self.batcher(formatter)
            np.random.seed(20260918)
            with contextlib.redirect_stdout(io.StringIO()):
                original_train = batcher._batch_sampled_data(train.copy(), max_samples=17)
                original_valid = batcher._batch_sampled_data(valid.copy(), max_samples=11)
                original_test = batcher._batch_data(test.copy())
            for key, reference_batch in (("train_windows_20260918", original_train),
                                         ("valid_windows_20260918", original_valid),
                                         ("test_windows", original_test)):
                windows = arrays[key]
                offsets = windows[:, 1, None] + np.arange(192)
                full_features = arrays["features"][offsets]
                # Original input order is target + known features + static ID.
                np.testing.assert_array_equal(full_features, reference_batch["inputs"][:, :, :4].astype("<f4"))
                np.testing.assert_array_equal(arrays["entity_categories"][windows[:, 0]],
                                              reference_batch["inputs"][:, 0, 4].astype(np.int64))
                np.testing.assert_array_equal(full_features[:, 168:, :1],
                                              reference_batch["outputs"].astype("<f4"))
                expected_ids = np.array(metadata["entities"])[windows[:, 0]]
                np.testing.assert_array_equal(expected_ids, reference_batch["identifier"][:, 0, 0])
                reconstructed = preparation.inverse_targets(full_features[:, 168:, 0], windows[:, 0],
                                                              arrays["target_mean_scale"])
                np.testing.assert_allclose(reconstructed, arrays["raw_targets"][offsets[:, 168:]], rtol=1e-6, atol=1e-4)
            self.assertEqual(len((output / "tensors.tsv").read_text().splitlines()), len(arrays))
            # Preservation contract rejects accidental overwrite and detects tampering.
            with self.assertRaisesRegex(ValueError, "must be empty"):
                preparation.export_dataset(raw, self.reference, output, protocol)
            with open(str(output / "test_windows.bin"), "r+b") as handle:
                handle.write(b"\xff")
            with self.assertRaisesRegex(ValueError, "checksum"):
                preparation.verify_export(output)

    def test_original_metric_and_correct_shape_inverse_scaling(self):
        import pandas as pd
        targets = np.array([[1., 100.], [3., 200.]])
        forecasts = np.array([[0., 110.], [1., 190.]])
        for quantile in (0.1, 0.5, 0.9):
            original = self.reference.reference_risk(pd.DataFrame(targets), pd.DataFrame(forecasts), quantile)
            ours = preparation.quantile_risks(targets, forecasts, quantile)
            np.testing.assert_allclose(original.values, ours["by_horizon"], rtol=1e-14)
            self.assertAlmostEqual(float(original.mean()), ours["reference_horizon_mean"])
        formatter = self.reference.formatter_class()
        with contextlib.redirect_stdout(io.StringIO()):
            formatter.set_scalers(self.fixture().loc[lambda df: df.days_from_start < 20])
        scaler = formatter._target_scaler["MT_001"]
        normalized = np.array([[-1., 0., 1.]])
        expected = scaler.inverse_transform(normalized.reshape(-1, 1)).reshape(1, 3)
        ours = preparation.inverse_targets(normalized, [0], [[scaler.mean_[0], scaler.scale_[0]]])
        np.testing.assert_array_equal(ours, expected)
        predictions = pd.DataFrame({"identifier": ["MT_001"] * 3, "t+0": [-1., 0., 1.]})
        try:
            original = formatter.format_predictions(predictions)
        except ValueError as error:
            self.assertIn("2D", str(error))
            print("Reference format_predictions needs a shape-only 2D inverse_transform adapter.")
        else:
            np.testing.assert_array_equal(original["t+0"].values, expected[0])

    def test_original_preprocessing_averaging_and_internal_zero(self):
        import pandas as pd
        protocol = preparation.read_protocol()
        with tempfile.TemporaryDirectory(prefix="tft-electricity-raw-fixture-") as temporary:
            directory = Path(temporary)
            # Keep the original time origin; sparse intervening dates are expanded
            # by the unchanged reference resampler, then filtered to 2014.
            dates = [pd.Timestamp("2011-01-01")] + list(pd.date_range("2014-01-01", periods=12, freq="15min"))
            raw = pd.DataFrame({"MT_001": [1.] + [1., 2., 3., 4.] + [0.] * 4 + [4.] * 4}, index=dates)
            raw_path = directory / protocol["raw_filename"]
            raw.to_csv(str(raw_path), sep=";", decimal=",")
            record = preparation.acquire_raw(directory, protocol)
            with contextlib.redirect_stdout(io.StringIO()):
                path = preparation.preprocess(self.reference, directory, protocol, record)
            result = pd.read_csv(str(path), index_col=0)
            np.testing.assert_array_equal(result.power_usage.values, [2.5, 0., 4.])
            np.testing.assert_array_equal(result.hours_from_start.values, [1096 * 24, 1096 * 24 + 1, 1096 * 24 + 2])
            self.assertEqual(preparation.preprocess(self.reference, directory, protocol, record), path)
            with self.assertRaisesRegex(ValueError, "checksum"):
                preparation.acquire_raw(directory, protocol, expected_sha256="0" * 64)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-root", type=Path, default=Path(".build/reference/tft"))
    parser.add_argument("--no-reference", action="store_true")
    parser.add_argument("--cpp-export", type=Path, help="Verify an actual C++ loader/metric export instead of unit fixtures")
    parser.add_argument("--data", type=Path)
    parser.add_argument("--seed", type=int, default=20260918)
    parser.add_argument("--receipt", type=Path)
    arguments, remaining = parser.parse_known_args()
    if arguments.cpp_export is not None:
        if arguments.data is None:
            parser.error("--cpp-export requires --data")
        verify_cpp_export(arguments.data, arguments.cpp_export, arguments.seed, arguments.receipt)
        raise SystemExit(0)
    REFERENCE_ROOT = None if arguments.no_reference else arguments.reference_root
    unittest.main(argv=[__file__] + remaining, verbosity=2)
