# Concurrent CUDA Initialization Benchmark

This bench measures how long many fresh processes spend entering CUDA initialization through HAMi-core's `libvgpu.so`.
It is intended for before/after measurements of the host PID broker gate:

- broker off: `LIBVGPU_HOSTPID_BROKER` unset
- broker on: `LIBVGPU_HOSTPID_BROKER=1`

Each worker is a fresh `exec()` of `cuinit_bench --internal-worker`. The controller clears inherited preload variables and applies `--worker-preload` only to worker exec environments, so controller state is not inherited by workers.

Workers use two barriers. The first releases all workers to `dlopen("libcuda.so.1")` and resolve CUDA symbols. The second releases the CUDA calls together. Symbols are resolved with `RTLD_DEFAULT` after a global `dlopen`, preserving `LD_PRELOAD` interposition.

Modes:

- `cuinit`: measures `cuInit`.
- `primary`: measures `cuInit`, `cuDeviceGet`, `cuDevicePrimaryCtxRetain`, optional `--hold-ms`, and `cuDevicePrimaryCtxRelease`.

The CSV records per-worker `load_us`, `symbol_us`, `cuinit_us`, `device_us`, `retain_us`, `hold_us`, `release_us`, and `api_us`, plus a failure stage. One monotonic deadline from `--timeout-seconds` covers both barriers and result collection. On timeout, the controller kills and reaps every child and still writes rows for completed and timed-out workers.

Build:

```bash
make -C bench
```

One run:

```bash
bench/build/cuinit_bench --workers 128 --mode cuinit \
  --worker-preload /path/to/libvgpu.so \
  --timeout-seconds 600 --output run.csv
python3 bench/summarize.py --condition broker-off run.csv
```

Balanced cache/GPU distribution:

```bash
bench/build/cuinit_bench --workers 240 --mode primary \
  --worker-preload /path/to/libvgpu.so \
  --worker-gpus 8 --worker-caches 48 --cache-prefix /tmp/hami-cache/cache \
  --output primary.csv
```

Worker `N` uses cache `N % worker_caches`; that cache uses GPU `cache_slot % worker_gpus`. The worker environment sets `CUDA_VISIBLE_DEVICES`, `CUDA_DEVICE_MEMORY_SHARED_CACHE`, `CUINIT_BENCH_ASSIGNED_GPU`, and `CUINIT_BENCH_ASSIGNED_CACHE`.

Paired protocol:

```bash
WORKER_GPUS=8 WORKER_CACHES=48 CACHE_PREFIX=/tmp/hami-cache/cache \
  bench/run-paired.sh broker-off broker-on /path/to/libvgpu.so 4
```

Set `LIBVGPU_B=/path/to/other/libvgpu.so` to give condition B a different build, for example current `main` against a patched build. `GATE_A` and `GATE_B` override the `LIBVGPU_HOSTPID_BROKER` value per condition (`unset` removes it).

`run-paired.sh` runs A then B on odd repetitions and B then A on even repetitions. It removes that condition's cache files before every run, records the HAMi-core revision, `uname -a`, `nvidia-smi` when present, exact command lines, and run order, then runs `summarize.py`.

When sharing results, include raw CSVs, the generated summary, run order, command log, HAMi-core revision, kernel/driver/GPU details, worker counts, modes, `WORKER_GPUS`, `WORKER_CACHES`, `CACHE_PREFIX`, and the `LIBVGPU_HOSTPID_BROKER` values used for each condition.

Credit: the earlier `bench/bench_init` harness by @om7057 in Project-HAMi/HAMi-core#239 produced the July baseline and shaped this smaller harness.
