#!/usr/bin/env python3

import argparse
import csv
import json
import math
from collections import defaultdict
from pathlib import Path

METRICS = ("load_us", "symbol_us", "cuinit_us", "device_us", "retain_us", "hold_us", "release_us", "api_us")


def percentile(values, fraction):
    if not values:
        return 0
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]


def read_csv(path):
    metadata = {}
    body = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("# "):
            key, separator, value = line[2:].partition("=")
            if not separator:
                raise ValueError(f"{path}: invalid metadata line {line!r}")
            metadata[key] = value
        elif line:
            body.append(line)
    if not body:
        raise ValueError(f"{path}: no CSV data")
    rows = list(csv.DictReader(body))
    workers = int(metadata.get("workers", "-1"))
    if workers < 1 or len(rows) != workers:
        raise ValueError(f"{path}: read {len(rows)} rows, expected {workers}")
    return metadata, rows


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--condition", action="append", nargs="+", required=True)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    groups = []
    for group in args.condition:
        condition = group[0]
        files = [Path(item) for item in group[1:]]
        if not files:
            parser.error(f"--condition {condition} has no CSV files")
        groups.append((condition, files))
    return groups, args.json


def summarize_group(condition, files):
    cells = defaultdict(lambda: {"rows": [], "runs": 0, "failures": defaultdict(int)})
    for path in files:
        metadata, rows = read_csv(path)
        if metadata.get("schema") != "cuinit-bench-v1":
            raise ValueError(f"{path}: unsupported schema {metadata.get('schema')}")
        key = (metadata.get("mode", ""), int(metadata["workers"]))
        cells[key]["runs"] += 1
        for row in rows:
            cells[key]["rows"].append(row)
            if row["status"] != "ok":
                cells[key]["failures"][row["stage"]] += 1

    summaries = []
    for (mode, workers), cell in sorted(cells.items()):
        ok_rows = [row for row in cell["rows"] if row["status"] == "ok"]
        metrics = {}
        for metric in METRICS:
            values = [int(row[metric]) for row in ok_rows]
            if any(value < 0 for value in values):
                raise ValueError(f"{condition} {mode} n={workers}: negative {metric}")
            metrics[metric] = {
                "p50": percentile(values, 0.50),
                "p95": percentile(values, 0.95),
                "p99": percentile(values, 0.99),
                "max": max(values, default=0),
            }
        summaries.append(
            {
                "condition": condition,
                "mode": mode,
                "workers": workers,
                "runs": cell["runs"],
                "count": len(cell["rows"]),
                "failures": sum(cell["failures"].values()),
                "failures_by_stage": dict(sorted(cell["failures"].items())),
                "metrics_us": metrics,
            }
        )
    return summaries


def print_summary(result):
    print("condition mode workers runs count failures metric p50 p95 p99 max")
    for cell in result["cells"]:
        for metric in ("api_us",) + tuple(m for m in METRICS if m != "api_us"):
            stats = cell["metrics_us"][metric]
            print(
                f"{cell['condition']} {cell['mode']} {cell['workers']} {cell['runs']} "
                f"{cell['count']} {cell['failures']} {metric} {stats['p50']} "
                f"{stats['p95']} {stats['p99']} {stats['max']}"
            )
        if cell["failures_by_stage"]:
            print(f"{cell['condition']} {cell['mode']} {cell['workers']} failures {cell['failures_by_stage']}")


def print_ratios(result):
    conditions = result["conditions"]
    if len(conditions) != 2:
        return
    by_key = {}
    for cell in result["cells"]:
        by_key[(cell["condition"], cell["mode"], cell["workers"])] = cell
    print("ratio mode workers api_p50_B_over_A api_p99_B_over_A")
    keys = ((cell["mode"], cell["workers"]) for cell in result["cells"] if cell["condition"] == conditions[0])
    for key in sorted(keys):
        left = by_key.get((conditions[0], key[0], key[1]))
        right = by_key.get((conditions[1], key[0], key[1]))
        if left is None or right is None:
            continue
        left_api = left["metrics_us"]["api_us"]
        right_api = right["metrics_us"]["api_us"]
        p50 = right_api["p50"] / left_api["p50"] if left_api["p50"] else None
        p99 = right_api["p99"] / left_api["p99"] if left_api["p99"] else None
        print(f"ratio {key[0]} {key[1]} {p50:.6g} {p99:.6g}")


def main():
    groups, json_path = parse_args()
    cells = []
    conditions = []
    for condition, files in groups:
        conditions.append(condition)
        cells.extend(summarize_group(condition, files))
    result = {"schema": "cuinit-summary-v1", "conditions": conditions, "cells": cells}
    print_summary(result)
    print_ratios(result)
    if json_path:
        json_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
