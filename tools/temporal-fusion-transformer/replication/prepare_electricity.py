#!/usr/bin/env python3
"""Prepare shared indexed Electricity data without materializing training windows.

Run with Python 3.7 and the pinned reference-compatible numpy/pandas/scikit-learn
environment. The downloaded reference sources are hash checked before executing
only their selected data-processing definitions, never their script entry points.
No TensorFlow import, model training, reference download or dataset download occurs
on import. Acquisition needs --download; existing files are preserved.
"""

import argparse
import ast
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import shutil
import sys
import types
import urllib.request
import uuid
import zipfile

import numpy as np


PROTOCOL_PATH = Path(__file__).with_name("electricity_protocol.json")


def read_protocol(path=PROTOCOL_PATH):
    with open(str(path), encoding="utf-8") as handle:
        return json.load(handle)


def sha256(path):
    digest = hashlib.sha256()
    with open(str(path), "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path, value):
    with open(str(path), "x", encoding="utf-8", newline="\n") as handle:
        json.dump(value, handle, indent=2, sort_keys=True, allow_nan=False)
        handle.write("\n")


def _definitions(path, names, namespace):
    """Execute unchanged selected upstream definitions, excluding all imports/main."""
    tree = ast.parse(Path(path).read_text(encoding="utf-8"), filename=str(path))
    selected = [node for node in tree.body
                if isinstance(node, (ast.FunctionDef, ast.ClassDef))
                and node.name in names]
    if {node.name for node in selected} != set(names):
        raise ValueError("Missing reference definitions in {}".format(path))
    tree.body = selected  # Retain version-specific AST Module fields.
    exec(compile(tree, str(path), "exec"), namespace)


def _class_methods(path, class_name, names, namespace):
    tree = ast.parse(Path(path).read_text(encoding="utf-8"), filename=str(path))
    source_class = next(node for node in tree.body
                        if isinstance(node, ast.ClassDef) and node.name == class_name)
    tree.body = [node for node in source_class.body
                 if isinstance(node, ast.FunctionDef) and node.name in names]
    if {node.name for node in tree.body} != set(names):
        raise ValueError("Missing reference batching methods")
    exec(compile(tree, str(path), "exec"), namespace)


def load_reference(root, protocol=None):
    """Load exact official formatter/preprocessor/helper bodies without TF."""
    import abc
    import enum
    import pandas as pd
    import sklearn.preprocessing

    protocol = protocol or read_protocol()
    root = Path(root)
    for relative, expected in protocol["reference_sha256"].items():
        if sha256(root / relative) != expected:
            raise ValueError("Reference checksum mismatch: " + relative)
    base = {"abc": abc, "enum": enum, "__name__": "pinned_tft_formatter"}
    _definitions(root / "data_formatters/base.py",
                 ["DataTypes", "InputTypes", "GenericDataFormatter"], base)
    helpers = {"np": np, "__name__": "pinned_tft_helpers"}
    helper_names = ["get_single_col_by_input_type", "extract_cols_from_data_type",
                    "numpy_normalised_quantile_loss"]
    _definitions(root / "libs/utils.py", helper_names, helpers)
    namespace = dict(base, pd=pd, sklearn=sklearn,
                     utils=types.SimpleNamespace(**{k: helpers[k] for k in helper_names}))
    _definitions(root / "data_formatters/electricity.py", ["ElectricityFormatter"], namespace)
    downloader = {"np": np, "pd": pd, "os": os, "__name__": "pinned_tft_download"}
    _definitions(root / "script_download_data.py", ["download_electricity"], downloader)
    batches = dict(np=np, InputTypes=base["InputTypes"], utils=namespace["utils"])
    _class_methods(root / "libs/tft_model.py", "TemporalFusionTransformer",
                   ["_batch_sampled_data", "_batch_data", "_get_single_col_by_type"], batches)
    return types.SimpleNamespace(
        formatter_class=namespace["ElectricityFormatter"],
        preprocess=downloader["download_electricity"],
        downloader_globals=downloader,
        reference_risk=helpers["numpy_normalised_quantile_loss"], batch_methods=batches)


def acquire_raw(data_dir, protocol, allow_download=False, expected_sha256=None):
    """Only extract the expected raw member; no broad extraction or replacement."""
    data_dir = Path(data_dir)
    raw = data_dir / protocol["raw_filename"]
    archive = raw.with_name(raw.name + ".zip")
    if not raw.exists():
        if not archive.exists():
            if not allow_download:
                raise ValueError("Raw data missing; supply it or explicitly use --download")
            data_dir.mkdir(parents=True, exist_ok=True)
            partial = archive.with_name(archive.name + ".partial-" + uuid.uuid4().hex)
            # A failed partial download is retained for diagnosis, never reused.
            with urllib.request.urlopen(protocol["dataset_url"], timeout=60) as source:
                with open(str(partial), "xb") as output:
                    shutil.copyfileobj(source, output, 1024 * 1024)
            if archive.exists():
                raise FileExistsError(str(archive))
            partial.rename(archive)
        data_dir.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(str(archive)) as zipped:
            members = [name for name in zipped.namelist()
                       if name.replace("\\", "/").split("/")[-1] == raw.name]
            if len(members) != 1:
                raise ValueError("Archive must contain exactly one expected raw file")
            partial = raw.with_name(raw.name + ".partial-" + uuid.uuid4().hex)
            with zipped.open(members[0]) as source, open(str(partial), "xb") as output:
                shutil.copyfileobj(source, output, 1024 * 1024)
            if raw.exists():
                raise FileExistsError(str(raw))
            partial.rename(raw)
    actual = sha256(raw)
    if expected_sha256 is not None and actual != expected_sha256.lower():
        raise ValueError("Raw dataset checksum mismatch; preserved file: " + str(raw))
    return {"path": str(raw.resolve()), "sha256": actual,
            "archive_sha256": sha256(archive) if archive.exists() else None,
            "url": protocol["dataset_url"]}


def preprocess(reference, data_dir, protocol, raw_record):
    data_dir = Path(data_dir)
    csv = data_dir / "hourly_electricity.csv"
    receipt = data_dir / "hourly_electricity.provenance.json"
    expected = {"raw_sha256": raw_record["sha256"],
                "reference_revision": protocol["reference_revision"],
                "preprocessor_sha256": protocol["reference_sha256"]["script_download_data.py"]}
    if csv.exists() or receipt.exists():
        if not (csv.exists() and receipt.exists()):
            raise ValueError("Incomplete existing preprocessing; preserve and inspect: " + str(csv))
        old = json.loads(receipt.read_text(encoding="utf-8"))
        if any(old.get(k) != v for k, v in expected.items()) or old.get("csv_sha256") != sha256(csv):
            raise ValueError("Existing preprocessing provenance mismatch: " + str(csv))
        return csv

    def require_raw(url, zip_path, csv_path, folder):
        if (url != protocol["dataset_url"] or Path(csv_path).resolve() != Path(raw_record["path"])):
            raise ValueError("Unexpected reference acquisition request")
        if not Path(csv_path).is_file():
            raise ValueError("Reference raw file missing")

    reference.downloader_globals["download_and_unzip"] = require_raw
    reference.preprocess(types.SimpleNamespace(data_folder=str(data_dir), data_csv_path=str(csv)))
    expected["csv_sha256"] = sha256(csv)
    write_json(receipt, expected)
    return csv


def build_candidates(entity_spans, hours, lower_day, upper_day, total_steps):
    """Enumerate the same entity/time order as upstream _batch_sampled_data."""
    pieces = []
    hours = np.asarray(hours, dtype=np.int64)
    for entity, (offset, length) in enumerate(entity_spans):
        offset, length = int(offset), int(length)
        clock = hours[offset:offset + length]
        if len(clock) > 1 and not np.all(np.diff(clock) == 1):
            raise ValueError("Electricity rows must be contiguous hourly observations")
        first = int(np.searchsorted(clock, lower_day * 24, side="left"))
        end = int(np.searchsorted(clock, upper_day * 24, side="left"))
        count = end - first - total_steps + 1
        if count > 0:
            start = np.arange(offset + first, offset + first + count, dtype=np.int64)
            pieces.append(np.column_stack((np.full(count, entity, dtype=np.int64), start)))
    return np.concatenate(pieces) if pieces else np.empty((0, 2), dtype=np.int64)


def sample_windows(training, validation, seed, train_count, valid_count):
    """Mirror the reference's sequential RandomState choices; never pad with zeros."""
    rng = np.random.RandomState(seed)
    results = []
    for candidates, count in ((training, train_count), (validation, valid_count)):
        if count <= 0 or len(candidates) < count:
            raise ValueError("Insufficient complete windows for requested sample count")
        indices = (rng.choice(len(candidates), count, replace=False)
                   if len(candidates) > count else np.arange(count))
        results.append(candidates[indices])
    return tuple(results)


def inverse_targets(values, entity_indices, mean_scale):
    values = np.asarray(values, dtype=np.float64)
    entity_indices = np.asarray(entity_indices, dtype=np.int64)
    mean_scale = np.asarray(mean_scale, dtype=np.float64)
    if values.ndim not in (2, 3) or len(entity_indices) != len(values):
        raise ValueError("Expected values [windows,horizon,(quantile)] and one entity per window")
    if entity_indices.ndim != 1 or np.any(entity_indices < 0) or np.any(entity_indices >= len(mean_scale)):
        raise ValueError("Invalid inverse-scaling entity indices")
    selected = mean_scale[entity_indices]
    shape = (len(values),) + (1,) * (values.ndim - 1)
    return values * selected[:, 1].reshape(shape) + selected[:, 0].reshape(shape)


def quantile_risks(targets, predictions, quantile):
    """Return paper-pooled and exact reference-DataFrame aggregations in float64."""
    targets, predictions = np.asarray(targets, dtype=np.float64), np.asarray(predictions, dtype=np.float64)
    if (targets.ndim != 2 or not all(targets.shape) or targets.shape != predictions.shape
            or not np.isfinite(targets).all() or not np.isfinite(predictions).all()
            or not 0 < quantile < 1):
        raise ValueError("Expected finite matching nonempty [windows,horizon] arrays and 0<q<1")
    error = targets - predictions
    loss = np.maximum(quantile * error, (quantile - 1.0) * error)
    denominators = np.abs(targets).sum(axis=0, dtype=np.float64)
    if np.any(denominators == 0):
        raise ValueError("Normalized quantile risk is undefined for an all-zero horizon")
    risks = 2.0 * loss.sum(axis=0, dtype=np.float64) / denominators
    return {"paper_pooled": float(2.0 * loss.sum(dtype=np.float64) / denominators.sum()),
            "reference_horizon_mean": float(risks.mean()), "by_horizon": risks.tolist()}


def write_array(directory, filename, array, dtype):
    array = np.ascontiguousarray(array, dtype=np.dtype(dtype))
    path = Path(directory) / filename
    with open(str(path), "xb") as handle:
        array.tofile(handle)
    return {"file": filename, "dtype": array.dtype.str, "shape": list(array.shape),
            "bytes": path.stat().st_size, "sha256": sha256(path)}


def export_dataset(raw_frame, reference, directory, protocol, provenance=None):
    """Store normalized entity rows once plus compact per-seed window indices."""
    import pandas as pd
    import sklearn

    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    if any(directory.iterdir()):
        raise ValueError("Export directory must be empty; existing data are preserved")
    columns = protocol["feature_columns"]
    # Preserve source row order while calling the original formatter. Its
    # categorical assignment is positional after groupby concatenation, so
    # sorting first changes the reference result for an unsorted input frame.
    # The official preprocessor already emits sorted entity/hour blocks.
    frame = raw_frame.copy()
    lower, upper = protocol["data_days"]
    if (not frame.index.is_unique or frame.empty
            or not frame["days_from_start"].between(lower, upper - 1).all()):
        raise ValueError("Source frame must have unique row indices within protocol dates")
    formatter = reference.formatter_class()
    with contextlib.redirect_stdout(io.StringIO()):
        formatter.set_scalers(frame.loc[frame["days_from_start"] < protocol["validation_boundary_day"]])
        transformed = formatter.transform_inputs(frame)
    # transform_inputs preserves indices; retain raw hours/targets before the
    # normalized hours_from_start replaces the model feature.
    transformed = transformed.sort_values(["id", "hours_from_start"])
    ordered_raw = frame.loc[transformed.index]
    if not np.isfinite(transformed[columns].values).all():
        raise ValueError("Non-finite transformed features")
    entities, spans, categories, target_stats, feature_stats = [], [], [], [], []
    offset = 0
    for identifier, group in transformed.groupby("id", sort=True):
        entities.append(str(identifier))
        spans.append([offset, len(group)])
        offset += len(group)
        category = group["categorical_id"].unique()
        if len(category) != 1:
            raise ValueError("Entity static category changes within its trajectory")
        categories.append(int(category[0]))
        scaler = formatter._target_scaler[identifier]
        target_stats.append([float(scaler.mean_[0]), float(scaler.scale_[0])])
        scaler = formatter._real_scalers[identifier]
        feature_stats.append(np.column_stack((scaler.mean_, scaler.scale_)))
    hours_float = ordered_raw["hours_from_start"].values.astype(np.float64)
    hours = hours_float.astype(np.int64)
    if not np.array_equal(hours_float, hours):
        raise ValueError("Expected integer hourly time index")
    if not np.array_equal(ordered_raw["days_from_start"].values, hours // 24):
        raise ValueError("Day/hour origin mismatch")
    spans = np.asarray(spans, dtype=np.int64)
    total = protocol["history"] + protocol["horizon"]
    if protocol["history"] % 24:
        raise ValueError("Electricity protocol requires whole-day history")
    context_days = protocol["history"] // 24
    valid, test = protocol["validation_boundary_day"], protocol["test_boundary_day"]
    candidates = {
        "train": build_candidates(spans, hours, lower, valid, total),
        "valid": build_candidates(spans, hours, valid - context_days, test, total),
        "test": build_candidates(spans, hours, test - context_days, upper, total)}
    arrays = {}
    def save(key, value, dtype):
        arrays[key] = write_array(directory, key + ".bin", value, dtype)
    save("features", transformed[columns].values.astype(np.float64), "<f4")
    save("entity_spans", spans, "<i8")
    save("entity_categories", categories, "<i8")
    save("target_mean_scale", target_stats, "<f8")
    save("feature_mean_scale", feature_stats, "<f8")
    save("raw_targets", ordered_raw["power_usage"].values, "<f8")
    save("hours_from_start", hours, "<i8")
    save("test_windows", candidates["test"], "<i8")
    for seed in protocol["seeds"]:
        training, validation = sample_windows(candidates["train"], candidates["valid"], seed,
                                               protocol["training_samples"], protocol["validation_samples"])
        save("train_windows_{}".format(seed), training, "<i8")
        save("valid_windows_{}".format(seed), validation, "<i8")
    with open(str(directory / "tensors.tsv"), "x", encoding="utf-8", newline="\n") as handle:
        for name, descriptor in arrays.items():
            handle.write("{}\t{}\t{}\t{}\n".format(
                name, descriptor["dtype"], ",".join(str(n) for n in descriptor["shape"]), descriptor["file"]))
    metadata = {"schema_version": 1, "protocol": protocol, "entities": entities,
                "arrays": arrays, "candidate_counts": {k: len(v) for k, v in candidates.items()},
                "window_columns": ["entity_index", "absolute_start_row"],
                "layout": "row-major little-endian raw binary; no headers",
                "feature_dtype_note": "Official StandardScaler computes float64; cast once to float32 for both models.",
                "runtime": {"python": platform.python_version(), "numpy": np.__version__,
                            "pandas": pd.__version__, "scikit_learn": sklearn.__version__},
                "tensor_manifest_sha256": sha256(directory / "tensors.tsv"),
                "provenance": provenance or {}}
    write_json(directory / "metadata.json", metadata)
    return metadata


def verify_export(directory):
    directory = Path(directory)
    metadata = json.loads((directory / "metadata.json").read_text(encoding="utf-8"))
    if sha256(directory / "tensors.tsv") != metadata["tensor_manifest_sha256"]:
        raise ValueError("Tensor manifest checksum mismatch")
    for descriptor in metadata["arrays"].values():
        path = directory / descriptor["file"]
        if (path.stat().st_size != descriptor["bytes"] or sha256(path) != descriptor["sha256"]
                or descriptor["bytes"] != int(np.prod(descriptor["shape"])) * np.dtype(descriptor["dtype"]).itemsize):
            raise ValueError("Export checksum or dimensions mismatch: " + str(path))
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-root", type=Path, default=Path(".build/reference/tft"))
    parser.add_argument("--data-dir", type=Path, default=Path(".build/datasets/electricity"))
    parser.add_argument("--output", type=Path, default=Path(".build/replication/electricity"))
    parser.add_argument("--download", action="store_true", help="Permit public raw dataset acquisition if absent")
    parser.add_argument("--expected-raw-sha256", help="Optional previously trusted raw checksum")
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    if args.verify_only:
        print(json.dumps({"verified": str(args.output), "arrays": len(verify_export(args.output)["arrays"])}))
        return
    if args.output.exists():
        raise ValueError("Output exists; use --verify-only or a new output path. No replacement is performed.")
    protocol = read_protocol()
    reference = load_reference(args.reference_root, protocol)
    raw = acquire_raw(args.data_dir, protocol, args.download, args.expected_raw_sha256)
    csv = preprocess(reference, args.data_dir, protocol, raw)
    import pandas as pd
    frame = pd.read_csv(str(csv), index_col=0)
    staging = args.output.with_name(args.output.name + ".preparing-" + uuid.uuid4().hex)
    provenance = {"raw": raw, "csv_sha256": sha256(csv), "protocol_sha256": sha256(PROTOCOL_PATH),
                  "preparation_sha256": sha256(Path(__file__))}
    metadata = export_dataset(frame, reference, staging, protocol, provenance)
    if args.output.exists():
        raise FileExistsError(str(args.output))
    staging.rename(args.output)
    print(json.dumps({"output": str(args.output), "entities": len(metadata["entities"]),
                      "candidate_counts": metadata["candidate_counts"]}, indent=2))


if __name__ == "__main__":
    main()
