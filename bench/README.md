# HAMi-core Initialization Benchmark Suite

Concurrent CUDA initialization performance benchmarks for HAMi-core's device sharing layer. These tools measure the cost of multiprocess cuInit() and primary context operations under various conditions, supporting the analysis in issue #1662.

## Tools

### bench_init
Measures concurrent cuInit() latency distribution across N worker processes, each spawned via fork+exec through `libvgpu.so` via `LD_PRELOAD`.

```bash
./bench_init N
```

Outputs: per-process initialization time in milliseconds, percentile distribution (p50, p95, p99, max), and aggregated cost. Requires a live GPU and CUDA libraries.

### phase_probe
Breaks down the cost of `set_task_pid()` (host PID detection via NVML snapshot diffing) into phases:
- nvmlInit
- Device snapshot enumeration
- Primary context retain/release cycles

Useful for identifying which phase dominates the initialization overhead.

```bash
./phase_probe
```

### nested_retain
Tests whether `cuDevicePrimaryCtxRetain` cost is proportional to refcount depth or constant once a context exists. Answers: is it cheap to hold a context across set_task_pid() calls?

```bash
./nested_retain
```

### warm_holder
Keeps a GPU context alive in a background process. Used by other tools to test whether context probe cost changes when a context is already held vs. cold.

```bash
./warm_holder [primary|nonprimary]
```

### abi_check
Compares `struct shared_region_t`'s sizeof and offsetof for key fields against the values captured from the Go-side mirror struct (`pkg/monitor/nvidia/v1/spec.go` in the HAMi repo), and exits nonzero on any mismatch. Used to catch C/Go struct drift that would silently corrupt `/tmp/cudevshr.cache`.

```bash
./abi_check
```

### nstgid_probe

Prints the current process's PID namespace IDs, namespace inodes, and procfs
mount as one JSON record. It has no CUDA dependency and is intended to verify
whether `NStgid` is host-relative in a specific runtime layout instead of
assuming that its leftmost value is always the host PID.

```bash
make nstgid_probe
./nstgid_probe
unshare --user --map-root-user --pid --fork ./nstgid_probe
unshare --user --map-root-user --pid --fork --mount-proc ./nstgid_probe
./nstgid_probe --proc-root /host/proc
```

### hostpid_broker_probe

Queries the fixed, root-owned host-PID broker socket and prints the
container PID, returned host PID, and round-trip latency as one JSON record.
It has no CUDA dependency.

```bash
make hostpid_broker_probe
./hostpid_broker_probe
./hostpid_broker_probe 5  # keep the process alive for node-side PID checks
```

The broker path is `/tmp/vgpulock/hostpid/broker.sock`. The probe rejects a
socket that is not owned by root or whose parent directory is writable by
group or other users.

### Guarded host-PID broker

The broker prototype is opt-in. `vGPUmonitor` must run in the host PID
namespace and listen on the fixed socket path, and HAMi-core must receive:

```bash
export LIBVGPU_HOSTPID_BROKER=1
```

If the broker is disabled, unavailable, untrusted, or returns an invalid
response, HAMi-core continues to the guarded `NStgid` path and then the
existing serialized NVML detector.

### Guarded host-PID fast path

The prototype fast path is enabled only when `LIBVGPU_HOST_PROCFS` points to a
procfs mounted from the initial PID namespace:

```bash
export LIBVGPU_HOST_PROCFS=/host/proc
```

Do not point this variable at a container-private `/proc`: its `NStgid` values
are container-relative. If the variable is absent or cannot be read, HAMi-core
uses the existing serialized NVML detector. A Kubernetes deployment would need
an explicitly reviewed, read-only hostPath mount of host `/proc`; the library
does not silently add or assume that privilege.

## Building

```bash
cd bench
make
```

Requires:
- CUDA headers and libraries (e.g., via `CUDA_HOME`)
- A C11/GNU11 compiler (`bench_init.c` uses `<stdatomic.h>`; the Makefile builds with `-std=gnu11`)
- `libvgpu.so` built in the parent directory

## Reproducing Issue #1662 Findings

Run the full benchmark suite:

```bash
cd bench
./run_benchmarks.sh
```

This executes:
1. `phase_probe` — identify bottlenecks
2. `nested_retain` — check whether holding a context is amortizable
3. `warm_holder primary &` — start context holder
4. `bench_init 1 2 4 8 16 32 64 128` — measure across concurrency levels
5. Kill warm_holder
6. `abi_check` — verify shared_region_t layout compatibility

Expected results:
- ~61.5 ms per-process serialized time (N=1)
- Linear scaling to ~128 processes (each adds ~61.5 ms)
- Phase breakdown: ~80% in cuDevicePrimaryCtx{Retain,Release}, ~20% in NVML
- Nested retain cost: ~1 µs (amortizable)

## Context

These benchmarks were developed to quantify the initialization lock contention identified in #1662. The fix (moving from polling to semaphore-based synchronization) is already merged, but these tools remain useful for regression testing and understanding concurrent device-sharing behavior.
