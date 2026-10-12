#!/bin/bash

set -euo pipefail

BENCH_DIR=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR="$BENCH_DIR/build"
SMOKE_DIR="$BUILD_DIR/smoke"
BENCH="$BUILD_DIR/cuinit_bench"
FAKE_CUDA="$SMOKE_DIR/libcuda.so.1"
LOG="$SMOKE_DIR/interposer.log"
SUMMARY="$SMOKE_DIR/summary.json"

mkdir -p "$SMOKE_DIR"
rm -f "$SMOKE_DIR"/*.csv "$SMOKE_DIR"/*.json "$SMOKE_DIR"/*.log

shared_flags=(-fPIC -shared)
if [ "$(uname -s)" = "Darwin" ]; then
    shared_flags=(-fPIC -dynamiclib)
fi

cc -std=c11 -O2 -Wall -Wextra -Werror "${shared_flags[@]}" \
    "$BENCH_DIR/test/fake_cuda.c" -o "$FAKE_CUDA"

CUINIT_BENCH_LIBCUDA="$FAKE_CUDA" \
FAKE_INTERPOSER_LOG="$LOG" \
FAKE_CUDA_INIT_US=2000 \
    "$BENCH" --workers 16 --mode cuinit --timeout-seconds 10 \
    --worker-preload "$FAKE_CUDA" --output "$SMOKE_DIR/cuinit.csv"

CUINIT_BENCH_LIBCUDA="$FAKE_CUDA" \
FAKE_INTERPOSER_LOG="$LOG" \
FAKE_CUDA_INIT_US=2000 \
FAKE_CUDA_DEVICE_US=1000 \
FAKE_CUDA_RETAIN_US=3000 \
FAKE_CUDA_RELEASE_US=1500 \
    "$BENCH" --workers 16 --mode primary --timeout-seconds 10 \
    --worker-preload "$FAKE_CUDA" --hold-ms 5 \
    --output "$SMOKE_DIR/primary.csv"

python3 "$BENCH_DIR/summarize.py" \
    --condition smoke "$SMOKE_DIR/cuinit.csv" "$SMOKE_DIR/primary.csv" \
    --json "$SUMMARY" > "$SMOKE_DIR/summary.txt"

set +e
CUINIT_BENCH_LIBCUDA="$FAKE_CUDA" \
FAKE_CUDA_INIT_US=2000000 \
    "$BENCH" --workers 16 --mode cuinit --timeout-seconds 1 \
    --output "$SMOKE_DIR/timeout.csv" \
    > "$SMOKE_DIR/timeout.stdout" 2> "$SMOKE_DIR/timeout.stderr"
timeout_status=$?
set -e
[ "$timeout_status" -ne 0 ]

python3 - "$SMOKE_DIR" <<'PY'
import csv
import json
import math
import os
import pathlib
import signal
import sys

directory = pathlib.Path(sys.argv[1])
summary = json.loads((directory / "summary.json").read_text())


def rows(name):
    lines = [
        line
        for line in (directory / name).read_text().splitlines()
        if line and not line.startswith("# ")
    ]
    return list(csv.DictReader(lines))


cuinit_rows = rows("cuinit.csv")
primary_rows = rows("primary.csv")
timeout_rows = rows("timeout.csv")
assert len(cuinit_rows) == 16
assert len(primary_rows) == 16
assert all(row["status"] == "ok" for row in cuinit_rows + primary_rows)
assert all(row["status"] == "failed" and row["stage"] == "timeout" for row in timeout_rows)

cells = {(cell["mode"], cell["workers"]): cell for cell in summary["cells"]}
cuinit = cells[("cuinit", 16)]
primary = cells[("primary", 16)]
assert cuinit["failures"] == 0
assert primary["failures"] == 0
assert cuinit["metrics_us"]["cuinit_us"]["p50"] >= 2000
assert primary["metrics_us"]["device_us"]["p50"] >= 1000
assert primary["metrics_us"]["retain_us"]["p50"] >= 3000
assert primary["metrics_us"]["release_us"]["p50"] >= 1500
assert primary["metrics_us"]["api_us"]["p50"] >= 7500

manual = sorted(int(row["api_us"]) for row in cuinit_rows)
assert cuinit["metrics_us"]["api_us"]["p50"] == manual[math.ceil(len(manual) * 0.50) - 1]

logged = {}
for line in (directory / "interposer.log").read_text().splitlines():
    event, pid = line.split()
    logged.setdefault(event, set()).add(int(pid))
expected = {int(row["pid"]) for row in cuinit_rows + primary_rows}
assert logged["load"] == expected
assert logged["cuInit"] == expected

for row in timeout_rows:
    pid = int(row["pid"])
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        continue
    raise AssertionError(f"timeout worker still exists: {pid}")
PY

printf 'bench smoke passed\n'
