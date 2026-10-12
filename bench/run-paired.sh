#!/bin/bash

set -euo pipefail

if [ "$#" -ne 4 ]; then
    printf 'usage: %s NAME_A NAME_B /path/to/libvgpu.so REPETITIONS\n' "$0" >&2
    exit 2
fi

NAME_A=$1
NAME_B=$2
LIBVGPU=$3
REPETITIONS=$4
BENCH_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT_DIR=$(cd "$BENCH_DIR/.." && pwd)
WORKER_COUNTS=${WORKER_COUNTS:-"16 64 128 240"}
MODES=${MODES:-"cuinit primary"}
WORKER_GPUS=${WORKER_GPUS:-0}
WORKER_CACHES=${WORKER_CACHES:-0}
TIMEOUT_SECONDS=${TIMEOUT_SECONDS:-600}
RUN_ID=${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}
OUT_DIR=${OUT_DIR:-"$BENCH_DIR/results/paired-${NAME_A}-vs-${NAME_B}-${RUN_ID}"}
CACHE_PREFIX=${CACHE_PREFIX:-"$OUT_DIR/cache/cache"}
GATE_A=${GATE_A:-unset}
GATE_B=${GATE_B:-1}
BENCH=${BENCH:-"$BENCH_DIR/build/cuinit_bench"}
LIBVGPU_B=${LIBVGPU_B:-$LIBVGPU}

unset LD_PRELOAD DYLD_INSERT_LIBRARIES DYLD_FORCE_FLAT_NAMESPACE

safe_label() {
    case "$1" in
        *[!A-Za-z0-9._-]*|'') return 1 ;;
        *) return 0 ;;
    esac
}

positive_integer() {
    case "$1" in
        *[!0-9]*|'') return 1 ;;
        *) [ "$1" -gt 0 ] ;;
    esac
}

quote_command() {
    local word
    for word in "$@"; do
        printf '%q ' "$word"
    done
    printf '\n'
}

for label in "$NAME_A" "$NAME_B" "$RUN_ID"; do
    safe_label "$label" || { printf 'error: unsafe label: %s\n' "$label" >&2; exit 2; }
done
[ "$NAME_A" != "$NAME_B" ] || { printf 'error: condition names must differ\n' >&2; exit 2; }
positive_integer "$REPETITIONS" || { printf 'error: repetitions must be positive\n' >&2; exit 2; }
positive_integer "$TIMEOUT_SECONDS" || { printf 'error: TIMEOUT_SECONDS must be positive\n' >&2; exit 2; }
case "$WORKER_GPUS:$WORKER_CACHES" in
    0:0) ;;
    * )
        positive_integer "$WORKER_GPUS" || { printf 'error: WORKER_GPUS must be positive or 0\n' >&2; exit 2; }
        positive_integer "$WORKER_CACHES" || { printf 'error: WORKER_CACHES must be positive or 0\n' >&2; exit 2; }
        [ "$WORKER_CACHES" -ge "$WORKER_GPUS" ] || {
            printf 'error: WORKER_CACHES must be >= WORKER_GPUS\n' >&2
            exit 2
        }
        ;;
esac

mkdir -p "$OUT_DIR/$NAME_A" "$OUT_DIR/$NAME_B" "$(dirname "$CACHE_PREFIX")"
make -C "$BENCH_DIR" >/dev/null

{
    printf 'hami_core_revision=%s\n' "$(git -C "$ROOT_DIR" rev-parse HEAD 2>/dev/null || printf unknown)"
    printf 'git_status=%s\n' "$(git -C "$ROOT_DIR" status --short 2>/dev/null | tr '\n' ';')"
    uname -a
    if command -v nvidia-smi >/dev/null 2>&1; then
        nvidia-smi
    else
        printf 'nvidia-smi=not found\n'
    fi
    printf 'worker_counts=%s\nmodes=%s\nrepetitions=%s\n' "$WORKER_COUNTS" "$MODES" "$REPETITIONS"
    printf 'worker_gpus=%s\nworker_caches=%s\ncache_prefix=%s\n' "$WORKER_GPUS" "$WORKER_CACHES" "$CACHE_PREFIX"
    printf 'gate_a=%s\ngate_b=%s\nlibvgpu_a=%s\nlibvgpu_b=%s\n' "$GATE_A" "$GATE_B" "$LIBVGPU" "$LIBVGPU_B"
} > "$OUT_DIR/environment.txt" 2>&1
: > "$OUT_DIR/commands.log"
printf 'sequence\tcondition\tmode\tworkers\trepetition\tstatus\n' > "$OUT_DIR/run-order.tsv"

run_one() {
    local condition=$1 gate=$2 mode=$3 workers=$4 repetition=$5 library=$6
    local dir="$OUT_DIR/$condition"
    local stem="${mode}-n${workers}-r${repetition}"
    local cache_base="$CACHE_PREFIX-${condition}-${stem}"
    local csv="$dir/${stem}.csv"
    local command=( "$BENCH" --workers "$workers" --mode "$mode" --timeout-seconds "$TIMEOUT_SECONDS"
        --worker-preload "$library" --output "$csv" )
    local environment=( env )

    # env options such as -u must come before any NAME=value assignment.
    if [ "$gate" = "unset" ]; then
        environment+=( -u LIBVGPU_HOSTPID_BROKER )
    else
        environment+=( "LIBVGPU_HOSTPID_BROKER=$gate" )
    fi
    if [ "$WORKER_GPUS" -gt 0 ]; then
        rm -f -- "$cache_base"-*.cache
        command+=( --worker-gpus "$WORKER_GPUS" --worker-caches "$WORKER_CACHES" --cache-prefix "$cache_base" )
    else
        rm -f -- "$cache_base.cache"
        environment+=( "CUDA_DEVICE_MEMORY_SHARED_CACHE=$cache_base.cache" )
    fi

    sequence=$((sequence + 1))
    printf '%s\t%s\t%s\t%s\t%s\t' "$sequence" "$condition" "$mode" "$workers" "$repetition" >> "$OUT_DIR/run-order.tsv"
    quote_command "${environment[@]}" "${command[@]}" >> "$OUT_DIR/commands.log"
    set +e
    "${environment[@]}" "${command[@]}" > "$dir/${stem}.stdout" 2> "$dir/${stem}.stderr"
    status=$?
    set -e
    printf '%s\n' "$status" >> "$OUT_DIR/run-order.tsv"
    return "$status"
}

overall=0
sequence=0
for mode in $MODES; do
    for workers in $WORKER_COUNTS; do
        repetition=1
        while [ "$repetition" -le "$REPETITIONS" ]; do
            if [ $((repetition % 2)) -eq 1 ]; then
                run_one "$NAME_A" "$GATE_A" "$mode" "$workers" "$repetition" "$LIBVGPU" || overall=1
                run_one "$NAME_B" "$GATE_B" "$mode" "$workers" "$repetition" "$LIBVGPU_B" || overall=1
            else
                run_one "$NAME_B" "$GATE_B" "$mode" "$workers" "$repetition" "$LIBVGPU_B" || overall=1
                run_one "$NAME_A" "$GATE_A" "$mode" "$workers" "$repetition" "$LIBVGPU" || overall=1
            fi
            repetition=$((repetition + 1))
        done
    done
done

shopt -s nullglob
csv_a=( "$OUT_DIR/$NAME_A"/*.csv )
csv_b=( "$OUT_DIR/$NAME_B"/*.csv )
shopt -u nullglob
python3 "$BENCH_DIR/summarize.py" \
    --condition "$NAME_A" "${csv_a[@]}" \
    --condition "$NAME_B" "${csv_b[@]}" \
    --json "$OUT_DIR/summary.json" > "$OUT_DIR/summary.txt" || overall=1
printf '%s\n' "$OUT_DIR"
exit "$overall"
