#!/usr/bin/env python3
"""Assess the frozen five-seed Electricity criterion; never accept smoke runs."""
import argparse
import itertools
import json
from pathlib import Path

import numpy as np

from prepare_electricity import read_protocol, sha256, write_json


def records(paths, implementation, protocol):
    seeds = protocol["seeds"]
    result = {}
    for path in paths:
        report = json.loads(path.read_text(encoding="utf-8"))
        seed = report.get("seed")
        if seed not in seeds or seed in result:
            raise ValueError("Unexpected or duplicate {} seed: {}".format(implementation, seed))
        evaluation = report.get("evaluation")
        if (report.get("benchmark_protocol_complete") is not True
                or report.get("is_smoke", False)
                or not isinstance(evaluation, dict) or evaluation.get("split") != "test"):
            raise ValueError("Incomplete/development report cannot establish replication: " + str(path))
        if implementation == "reference":
            if report.get("implementation") != "original_tensorflow" or report.get("status") != "complete":
                raise ValueError("Expected completed original TensorFlow report")
            dataset = report["data_metadata_sha256"]
            initial = report["initial_weights_sha256"]
        else:
            if report.get("implementation") != "C++ LibTorch TFT":
                raise ValueError("Expected C++ TFT report")
            dataset = sha256(path.parent / "dataset-metadata.json")
            metadata = json.loads((path.parent / "dataset-metadata.json").read_text())
            if metadata["protocol"] != protocol:
                raise ValueError("Report uses a different frozen protocol: " + str(path))
            initial = json.loads((path.parent / "initialization-metadata.json").read_text())["original_tf_weights_sha256"]
        result[seed] = {"report": report, "path": str(path.resolve()),
                        "sha256": sha256(path), "dataset": dataset, "initial_weights": initial}
    if set(result) != set(seeds):
        raise ValueError("Missing {} seeds: {}".format(implementation, sorted(set(seeds) - set(result))))
    return result


def assess(reference, cpp, protocol):
    seeds = protocol["seeds"]
    if len({run["dataset"] for runs in (reference, cpp) for run in runs.values()}) != 1:
        raise ValueError("All ten runs must use the same prepared dataset manifest")
    for seed in seeds:
        if (reference[seed]["dataset"] != cpp[seed]["dataset"]
                or reference[seed]["initial_weights"] != cpp[seed]["initial_weights"]):
            raise ValueError("Paired data or initialization mismatch for seed {}".format(seed))
    comparison = protocol["comparison"]
    # Exhaust all n**n paired bootstrap resamples; no simulation RNG or selection.
    resamples = np.array(list(itertools.product(range(len(seeds)), repeat=len(seeds))))
    metrics = {}
    for quantile in comparison["required_metrics"]:
        metrics[quantile] = {}
        for aggregation in comparison["required_aggregations"]:
            def values(runs):
                return np.array([runs[seed]["report"]["evaluation"]["metrics"]["tft"]
                                 ["quantiles"][quantile][aggregation] for seed in seeds], dtype=np.float64)
            original, replication = values(reference), values(cpp)
            if (not np.isfinite(original).all() or not np.isfinite(replication).all()
                    or np.any(original <= 0) or np.any(replication < 0)):
                raise ValueError("Invalid normalized-risk results")
            difference = replication - original
            interval = np.percentile(difference[resamples].mean(axis=1), [2.5, 97.5])
            relative = float(replication.mean() / original.mean() - 1.0)
            metrics[quantile][aggregation] = {
                "reference_mean": float(original.mean()), "cpp_mean": float(replication.mean()),
                "relative_mean_degradation": relative,
                "passes": relative <= comparison["maximum_relative_mean_degradation"],
                "paired_difference_mean": float(difference.mean()),
                "paired_difference_bootstrap_95_percent_interval": interval.tolist(),
                "seeds": [{"seed": seed, "reference": float(original[i]), "cpp": float(replication[i]),
                           "difference": float(difference[i])} for i, seed in enumerate(seeds)]}
        score = protocol["published_scores"][quantile]
        for aggregation in comparison["required_aggregations"]:
            for implementation in ("reference", "cpp"):
                mean = metrics[quantile][aggregation][implementation + "_mean"]
                metrics[quantile][aggregation][implementation + "_relative_to_published"] = mean / score - 1.0
    passed = all(metrics[q][a]["passes"] for q in comparison["required_metrics"]
                 for a in comparison["required_aggregations"])
    return {"schema_version": 1, "status": "assessed", "passes_engineering_similarity": passed,
            "protocol": protocol, "metrics": metrics,
            "uncertainty_method": "Exact paired percentile bootstrap over all 5^5 resamples; linear percentiles. Five runs give limited uncertainty evidence; this is not proof of statistical equivalence.",
            "claim_limit": "Engineering similarity on Electricity only. Published-score gaps still require assessment; this does not establish replication of all TFT paper results.",
            "sources": {name: [{k: v for k, v in runs[seed].items() if k != "report"} for seed in seeds]
                        for name, runs in (("reference", reference), ("cpp", cpp))}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", nargs="+", type=Path, required=True)
    parser.add_argument("--cpp", nargs="+", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError("Existing assessment preserved: " + str(args.output))
    protocol = read_protocol()
    try:
        report = assess(records(args.reference, "reference", protocol),
                        records(args.cpp, "cpp", protocol), protocol)
    except (ValueError, KeyError, TypeError, OSError) as error:
        report = {"schema_version": 1, "status": "rejected", "passes_engineering_similarity": False,
                  "error": str(error), "reference_paths": list(map(str, args.reference)),
                  "cpp_paths": list(map(str, args.cpp))}
    report["assessment_source_sha256"] = sha256(__file__)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_json(args.output, report)
    print(json.dumps({"output": str(args.output), "passes_engineering_similarity": report["passes_engineering_similarity"]}))
    return 2 if report["status"] == "rejected" else (0 if report["passes_engineering_similarity"] else 1)


if __name__ == "__main__":
    raise SystemExit(main())
