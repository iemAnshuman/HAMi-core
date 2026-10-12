#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_TIMEOUT_SECONDS 600
#define MAX_WORKERS 2048
#define MAX_DISTRIBUTION_GPUS 64

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;

typedef CUresult (*cu_init_fn)(unsigned int flags);
typedef CUresult (*cu_device_get_fn)(CUdevice *device, int ordinal);
typedef CUresult (*cu_primary_retain_fn)(CUcontext *context, CUdevice device);
typedef CUresult (*cu_primary_release_fn)(CUdevice device);

enum benchmark_mode { MODE_CUINIT = 0, MODE_PRIMARY = 1 };
enum worker_stage { STAGE_OK = 0, STAGE_CONFIG, STAGE_DLOPEN, STAGE_SYMBOL, STAGE_CUINIT, STAGE_DEVICE,
                    STAGE_RETAIN, STAGE_RELEASE, STAGE_TIMEOUT, STAGE_LOST };

struct worker_result {
    int32_t worker, pid, cache_slot, gpu_slot, stage, api_result;
    int64_t load_us, symbol_us, cuinit_us, device_us, retain_us, hold_us, release_us, api_us;
};

_Static_assert(sizeof(struct worker_result) <= PIPE_BUF, "worker result must fit in one pipe write");

struct worker_runtime {
    void *library;
    cu_init_fn cu_init;
    cu_device_get_fn cu_device_get;
    cu_primary_retain_fn cu_primary_retain;
    cu_primary_release_fn cu_primary_release;
};

struct options {
    int workers, device, hold_ms, worker_gpus, worker_caches, timeout_seconds;
    enum benchmark_mode mode;
    const char *worker_preload, *cache_prefix, *output_path;
};

static const char *mode_name(enum benchmark_mode mode) {
    return mode == MODE_PRIMARY ? "primary" : "cuinit";
}

static const char *stage_name(int stage) {
    static const char *const names[] = {
        "ok", "config", "dlopen", "symbol", "cuinit", "device", "retain", "release", "timeout", "lost",
    };
    return stage >= 0 && (size_t)stage < sizeof(names) / sizeof(names[0]) ? names[stage] : "unknown";
}

static void usage(const char *program) {
    fprintf(stderr,
            "usage: %s --workers N [--mode cuinit|primary] [--worker-preload PATH]\n"
            "       [--worker-gpus N --worker-caches N --cache-prefix PATH]\n"
            "       [--device N] [--hold-ms N] [--timeout-seconds N] [--output PATH]\n",
            program);
}

static int parse_int(const char *value, int minimum, int maximum, int *parsed) {
    char *end = NULL;
    intmax_t number;

    errno = 0;
    number = strtoimax(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || number < minimum || number > maximum) {
        return -1;
    }
    *parsed = (int)number;
    return 0;
}

static const char *next_arg(int argc, char **argv, int *index) {
    return *index + 1 < argc ? argv[++(*index)] : NULL;
}

static int parse_options(int argc, char **argv, struct options *options) {
    int index;

    *options = (struct options){
        .workers = 0, .device = 0, .hold_ms = 0, .worker_gpus = 0, .worker_caches = 0,
        .timeout_seconds = DEFAULT_TIMEOUT_SECONDS, .mode = MODE_CUINIT, .worker_preload = NULL,
        .cache_prefix = NULL, .output_path = NULL,
    };
    for (index = 1; index < argc; index++) {
        const char *value;

        if (strcmp(argv[index], "--workers") == 0) {
            value = next_arg(argc, argv, &index);
            if (value == NULL || parse_int(value, 1, MAX_WORKERS, &options->workers) != 0) return -1;
        } else if (strcmp(argv[index], "--mode") == 0) {
            value = next_arg(argc, argv, &index);
            if (value == NULL) return -1;
            if (strcmp(value, "cuinit") == 0) {
                options->mode = MODE_CUINIT;
            } else if (strcmp(value, "primary") == 0) {
                options->mode = MODE_PRIMARY;
            } else {
                return -1;
            }
        } else if (strcmp(argv[index], "--device") == 0) {
            value = next_arg(argc, argv, &index);
            if (value == NULL || parse_int(value, 0, INT_MAX, &options->device) != 0) return -1;
        } else if (strcmp(argv[index], "--hold-ms") == 0) {
            value = next_arg(argc, argv, &index);
            if (value == NULL || parse_int(value, 0, 600000, &options->hold_ms) != 0) return -1;
        } else if (strcmp(argv[index], "--timeout-seconds") == 0) {
            value = next_arg(argc, argv, &index);
            if (value == NULL || parse_int(value, 1, 86400, &options->timeout_seconds) != 0) return -1;
        } else if (strcmp(argv[index], "--worker-preload") == 0) {
            options->worker_preload = next_arg(argc, argv, &index);
            if (options->worker_preload == NULL || options->worker_preload[0] == '\0') return -1;
        } else if (strcmp(argv[index], "--worker-gpus") == 0) {
            value = next_arg(argc, argv, &index);
            if (value == NULL || parse_int(value, 1, MAX_DISTRIBUTION_GPUS, &options->worker_gpus) != 0) return -1;
        } else if (strcmp(argv[index], "--worker-caches") == 0) {
            value = next_arg(argc, argv, &index);
            if (value == NULL || parse_int(value, 1, MAX_WORKERS, &options->worker_caches) != 0) return -1;
        } else if (strcmp(argv[index], "--cache-prefix") == 0) {
            options->cache_prefix = next_arg(argc, argv, &index);
            if (options->cache_prefix == NULL || options->cache_prefix[0] == '\0') return -1;
        } else if (strcmp(argv[index], "--output") == 0) {
            options->output_path = next_arg(argc, argv, &index);
            if (options->output_path == NULL) return -1;
        } else {
            return -1;
        }
    }
    if (options->workers <= 0) {
        return -1;
    }
    if (options->worker_gpus == 0 && options->worker_caches == 0 && options->cache_prefix == NULL) {
        return 0;
    }
    if (options->worker_gpus <= 0 || options->worker_caches <= 0 || options->cache_prefix == NULL ||
        options->worker_caches < options->worker_gpus) {
        return -1;
    }
    return 0;
}

static int monotonic_now(struct timespec *value) {
    return clock_gettime(CLOCK_MONOTONIC, value);
}

static int64_t elapsed_us(const struct timespec *begin, const struct timespec *end) {
    return (int64_t)(end->tv_sec - begin->tv_sec) * 1000000LL + (end->tv_nsec - begin->tv_nsec) / 1000LL;
}

static int64_t remaining_ms(const struct timespec *deadline) {
    struct timespec now;
    int64_t nanoseconds;

    if (monotonic_now(&now) != 0) {
        return -1;
    }
    nanoseconds = (int64_t)(deadline->tv_sec - now.tv_sec) * 1000000000LL + deadline->tv_nsec - now.tv_nsec;
    if (nanoseconds <= 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    return (nanoseconds + 999999LL) / 1000000LL;
}

static int make_deadline(struct timespec *deadline, int timeout_seconds) {
    if (monotonic_now(deadline) != 0) {
        return -1;
    }
    deadline->tv_sec += timeout_seconds;
    return 0;
}

static void sleep_ms(int milliseconds) {
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L,
    };

    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static int read_exact(int fd, void *buffer, size_t length) {
    unsigned char *cursor = buffer;

    while (length > 0) {
        ssize_t count = read(fd, cursor, length);

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            errno = count == 0 ? ECONNRESET : errno;
            return -1;
        }
        cursor += count;
        length -= (size_t)count;
    }
    return 0;
}

static int write_exact(int fd, const void *buffer, size_t length) {
    const unsigned char *cursor = buffer;

    while (length > 0) {
        ssize_t count = write(fd, cursor, length);

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return -1;
        }
        cursor += count;
        length -= (size_t)count;
    }
    return 0;
}

static int read_exact_before(int fd, void *buffer, size_t length, const struct timespec *deadline) {
    unsigned char *cursor = buffer;

    while (length > 0) {
        struct pollfd descriptor = {.fd = fd, .events = POLLIN};
        int64_t timeout = remaining_ms(deadline);
        int result;
        ssize_t count;

        if (timeout < 0) {
            return -1;
        }
        if (timeout > INT_MAX) {
            timeout = INT_MAX;
        }
        do {
            result = poll(&descriptor, 1, (int)timeout);
        } while (result < 0 && errno == EINTR);
        if (result == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (result < 0) {
            return -1;
        }
        do {
            count = read(fd, cursor, length);
        } while (count < 0 && errno == EINTR);
        if (count <= 0) {
            errno = count == 0 ? ECONNRESET : errno;
            return -1;
        }
        cursor += count;
        length -= (size_t)count;
    }
    return 0;
}

static void clear_loader_environment(void) {
    (void)unsetenv("LD_PRELOAD");
    (void)unsetenv("DYLD_INSERT_LIBRARIES");
    (void)unsetenv("DYLD_FORCE_FLAT_NAMESPACE");
}

static int reexec_controller_without_preload(char **argv) {
    if (getenv("CUINIT_BENCH_CLEAN_CONTROLLER") != NULL) {
        (void)unsetenv("CUINIT_BENCH_CLEAN_CONTROLLER");
        clear_loader_environment();
        return 0;
    }
    if (getenv("LD_PRELOAD") == NULL && getenv("DYLD_INSERT_LIBRARIES") == NULL &&
        getenv("DYLD_FORCE_FLAT_NAMESPACE") == NULL) {
        return 0;
    }
    clear_loader_environment();
    if (setenv("CUINIT_BENCH_CLEAN_CONTROLLER", "1", 1) != 0) {
        return -1;
    }
    execvp(argv[0], argv);
    return -1;
}

static int validate_distribution(struct worker_result *result) {
    const char *assigned_cache = getenv("CUINIT_BENCH_ASSIGNED_CACHE");
    const char *assigned_gpu = getenv("CUINIT_BENCH_ASSIGNED_GPU");
    const char *visible_gpu = getenv("CUDA_VISIBLE_DEVICES");

    if (assigned_cache == NULL && assigned_gpu == NULL) {
        return 0;
    }
    if (assigned_cache == NULL || assigned_gpu == NULL ||
        parse_int(assigned_cache, 0, MAX_WORKERS - 1, &result->cache_slot) != 0 ||
        parse_int(assigned_gpu, 0, MAX_DISTRIBUTION_GPUS - 1, &result->gpu_slot) != 0 ||
        visible_gpu == NULL || strcmp(visible_gpu, assigned_gpu) != 0) {
        result->stage = STAGE_CONFIG;
        result->api_result = EINVAL;
        return -1;
    }
    return 0;
}

static void prepare_worker(int worker, const struct options *options, struct worker_result *result,
                           struct worker_runtime *runtime) {
    const char *library_path = getenv("CUINIT_BENCH_LIBCUDA");
    struct timespec begin, end;

    *result = (struct worker_result){
        .worker = worker,
        .pid = (int32_t)getpid(),
        .cache_slot = -1,
        .gpu_slot = -1,
        .stage = STAGE_OK,
    };
    *runtime = (struct worker_runtime){0};
    if (validate_distribution(result) != 0) {
        return;
    }
    if (library_path == NULL || library_path[0] == '\0') {
        library_path = "libcuda.so.1";
    }
    if (monotonic_now(&begin) != 0) {
        result->stage = STAGE_DLOPEN;
        result->api_result = errno;
        return;
    }
    runtime->library = dlopen(library_path, RTLD_NOW | RTLD_GLOBAL);
    (void)monotonic_now(&end);
    result->load_us = elapsed_us(&begin, &end);
    if (runtime->library == NULL) {
        result->stage = STAGE_DLOPEN;
        result->api_result = ENOENT;
        return;
    }

    (void)monotonic_now(&begin);
    runtime->cu_init = (cu_init_fn)dlsym(RTLD_DEFAULT, "cuInit");
    if (options->mode == MODE_PRIMARY) {
        runtime->cu_device_get = (cu_device_get_fn)dlsym(RTLD_DEFAULT, "cuDeviceGet");
        runtime->cu_primary_retain = (cu_primary_retain_fn)dlsym(RTLD_DEFAULT, "cuDevicePrimaryCtxRetain");
        runtime->cu_primary_release = (cu_primary_release_fn)dlsym(RTLD_DEFAULT, "cuDevicePrimaryCtxRelease_v2");
        if (runtime->cu_primary_release == NULL) {
            runtime->cu_primary_release = (cu_primary_release_fn)dlsym(RTLD_DEFAULT, "cuDevicePrimaryCtxRelease");
        }
    }
    (void)monotonic_now(&end);
    result->symbol_us = elapsed_us(&begin, &end);
    if (runtime->cu_init == NULL ||
        (options->mode == MODE_PRIMARY &&
         (runtime->cu_device_get == NULL || runtime->cu_primary_retain == NULL ||
          runtime->cu_primary_release == NULL))) {
        result->stage = STAGE_SYMBOL;
        result->api_result = ENOSYS;
    }
}

static void execute_worker(const struct options *options, struct worker_result *result,
                           const struct worker_runtime *runtime) {
    struct timespec api_begin, api_end, begin, end;
    CUdevice device = 0;
    CUcontext context = NULL;

    (void)monotonic_now(&api_begin);
    (void)monotonic_now(&begin);
    result->api_result = runtime->cu_init(0);
    (void)monotonic_now(&end);
    result->cuinit_us = elapsed_us(&begin, &end);
    if (result->api_result != 0) {
        result->stage = STAGE_CUINIT;
        goto finish;
    }
    if (options->mode == MODE_PRIMARY) {
        (void)monotonic_now(&begin);
        result->api_result = runtime->cu_device_get(&device, options->device);
        (void)monotonic_now(&end);
        result->device_us = elapsed_us(&begin, &end);
        if (result->api_result != 0) {
            result->stage = STAGE_DEVICE;
            goto finish;
        }
        (void)monotonic_now(&begin);
        result->api_result = runtime->cu_primary_retain(&context, device);
        (void)monotonic_now(&end);
        result->retain_us = elapsed_us(&begin, &end);
        if (result->api_result != 0) {
            result->stage = STAGE_RETAIN;
            goto finish;
        }
        if (options->hold_ms > 0) {
            (void)monotonic_now(&begin);
            sleep_ms(options->hold_ms);
            (void)monotonic_now(&end);
            result->hold_us = elapsed_us(&begin, &end);
        }
        (void)monotonic_now(&begin);
        result->api_result = runtime->cu_primary_release(device);
        (void)monotonic_now(&end);
        result->release_us = elapsed_us(&begin, &end);
        if (result->api_result != 0) {
            result->stage = STAGE_RELEASE;
        }
    }

finish:
    (void)monotonic_now(&api_end);
    result->api_us = elapsed_us(&api_begin, &api_end);
}

static void exit_worker(struct worker_runtime *runtime, int code) {
    if (runtime->library != NULL) {
        (void)dlclose(runtime->library);
    }
    _exit(code);
}

static void child_main(int worker, const struct options *options, int ready_fd, int setup_fd, int prepared_fd,
                       int api_fd, int result_fd) {
    unsigned char token = 1;
    struct worker_result result;
    struct worker_runtime runtime;

    if (write_exact(ready_fd, &token, sizeof(token)) != 0 || read_exact(setup_fd, &token, sizeof(token)) != 0) {
        _exit(125);
    }
    prepare_worker(worker, options, &result, &runtime);
    if (write_exact(prepared_fd, &token, sizeof(token)) != 0 || read_exact(api_fd, &token, sizeof(token)) != 0) {
        exit_worker(&runtime, 125);
    }
    if (result.stage == STAGE_OK) {
        execute_worker(options, &result, &runtime);
    }
    if (write_exact(result_fd, &result, sizeof(result)) != 0) {
        exit_worker(&runtime, 126);
    }
    exit_worker(&runtime, result.stage == STAGE_OK ? 0 : 1);
}

static int internal_worker_main(int argc, char **argv) {
    struct options options = {.device = 0, .hold_ms = 0, .mode = MODE_CUINIT};
    int worker, ready_fd, setup_fd, prepared_fd, api_fd, result_fd;

    if (argc != 11 || parse_int(argv[2], 0, MAX_WORKERS - 1, &worker) != 0 ||
        parse_int(argv[4], 0, INT_MAX, &options.device) != 0 ||
        parse_int(argv[5], 0, 600000, &options.hold_ms) != 0 ||
        parse_int(argv[6], 0, INT_MAX, &ready_fd) != 0 ||
        parse_int(argv[7], 0, INT_MAX, &setup_fd) != 0 ||
        parse_int(argv[8], 0, INT_MAX, &prepared_fd) != 0 ||
        parse_int(argv[9], 0, INT_MAX, &api_fd) != 0 ||
        parse_int(argv[10], 0, INT_MAX, &result_fd) != 0) {
        return 2;
    }
    if (strcmp(argv[3], "primary") == 0) {
        options.mode = MODE_PRIMARY;
    } else if (strcmp(argv[3], "cuinit") != 0) {
        return 2;
    }
    child_main(worker, &options, ready_fd, setup_fd, prepared_fd, api_fd, result_fd);
    return 1;
}

static void exec_worker(const char *program, int worker, const struct options *options, int ready_fd, int setup_fd,
                        int prepared_fd, int api_fd, int result_fd) {
    char worker_text[32], device_text[32], hold_text[32], ready_text[32], setup_text[32];
    char prepared_text[32], api_text[32], result_text[32], gpu_text[32], cache_slot_text[32];
    char cache_path[PATH_MAX];
    char *child_argv[] = {
        (char *)program, "--internal-worker", worker_text, (char *)mode_name(options->mode), device_text, hold_text,
        ready_text,     setup_text,          prepared_text, api_text,                         result_text, NULL,
    };

    (void)snprintf(worker_text, sizeof(worker_text), "%d", worker);
    (void)snprintf(device_text, sizeof(device_text), "%d", options->device);
    (void)snprintf(hold_text, sizeof(hold_text), "%d", options->hold_ms);
    (void)snprintf(ready_text, sizeof(ready_text), "%d", ready_fd);
    (void)snprintf(setup_text, sizeof(setup_text), "%d", setup_fd);
    (void)snprintf(prepared_text, sizeof(prepared_text), "%d", prepared_fd);
    (void)snprintf(api_text, sizeof(api_text), "%d", api_fd);
    (void)snprintf(result_text, sizeof(result_text), "%d", result_fd);
    clear_loader_environment();
    (void)unsetenv("CUINIT_BENCH_ASSIGNED_GPU");
    (void)unsetenv("CUINIT_BENCH_ASSIGNED_CACHE");
    if (options->worker_preload != NULL) {
#if defined(__APPLE__)
        if (setenv("DYLD_INSERT_LIBRARIES", options->worker_preload, 1) != 0 ||
            setenv("DYLD_FORCE_FLAT_NAMESPACE", "1", 1) != 0) {
#else
        if (setenv("LD_PRELOAD", options->worker_preload, 1) != 0) {
#endif
            perror("set worker preload");
            _exit(127);
        }
    }
    if (options->worker_gpus > 0) {
        int cache_slot = worker % options->worker_caches;
        int gpu_slot = cache_slot % options->worker_gpus;
        int cache_length;

        (void)snprintf(gpu_text, sizeof(gpu_text), "%d", gpu_slot);
        (void)snprintf(cache_slot_text, sizeof(cache_slot_text), "%d", cache_slot);
        cache_length = snprintf(cache_path, sizeof(cache_path), "%s-%04d.cache", options->cache_prefix, cache_slot);
        if (cache_length < 0 || (size_t)cache_length >= sizeof(cache_path) ||
            setenv("CUDA_VISIBLE_DEVICES", gpu_text, 1) != 0 ||
            setenv("CUDA_DEVICE_MEMORY_SHARED_CACHE", cache_path, 1) != 0 ||
            setenv("CUINIT_BENCH_ASSIGNED_GPU", gpu_text, 1) != 0 ||
            setenv("CUINIT_BENCH_ASSIGNED_CACHE", cache_slot_text, 1) != 0) {
            perror("set worker distribution");
            _exit(127);
        }
    }
    execvp(program, child_argv);
    perror("exec worker");
    _exit(127);
}

static void close_pipe(int descriptors[2]) {
    int index;

    for (index = 0; index < 2; index++) {
        if (descriptors[index] >= 0) {
            (void)close(descriptors[index]);
            descriptors[index] = -1;
        }
    }
}

static void terminate_children(pid_t *children, int workers) {
    int index;

    for (index = 0; index < workers; index++) {
        if (children[index] > 0) {
            (void)kill(children[index], SIGKILL);
        }
    }
    for (index = 0; index < workers; index++) {
        if (children[index] > 0) {
            while (waitpid(children[index], NULL, 0) < 0 && errno == EINTR) {
            }
        }
    }
}

static int wait_for_children(pid_t *children, int workers) {
    int failed = 0;
    int index;

    for (index = 0; index < workers; index++) {
        int status;
        pid_t waited;

        if (children[index] <= 0) {
            continue;
        }
        do {
            waited = waitpid(children[index], &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited != children[index] || !WIFEXITED(status)) {
            failed = 1;
        }
        children[index] = 0;
    }
    return failed;
}

static int compare_worker(const void *left, const void *right) {
    const struct worker_result *a = left;
    const struct worker_result *b = right;

    return (a->worker > b->worker) - (a->worker < b->worker);
}

static void fill_missing_results(const struct options *options, struct worker_result *results, const pid_t *children,
                                 const unsigned char *received, int stage) {
    int index;

    for (index = 0; index < options->workers; index++) {
        if (received[index]) {
            continue;
        }
        results[index] = (struct worker_result){
            .worker = index,
            .pid = (int32_t)children[index],
            .cache_slot = options->worker_gpus > 0 ? index % options->worker_caches : -1,
            .gpu_slot = options->worker_gpus > 0 ? (index % options->worker_caches) % options->worker_gpus : -1,
            .stage = stage,
            .api_result = ETIMEDOUT,
        };
    }
}

static int write_output(const struct options *options, struct worker_result *results, const char *outcome,
                        int results_received, int64_t setup_wall_us, int64_t api_wall_us) {
    FILE *output = stdout;
    int failures = 0;
    int index;

    if (options->output_path != NULL) {
        output = fopen(options->output_path, "w");
        if (output == NULL) {
            return -1;
        }
    }
    qsort(results, (size_t)options->workers, sizeof(*results), compare_worker);
    fprintf(output, "# schema=cuinit-bench-v1\n# workers=%d\n# mode=%s\n# symbol_scope=process\n# device=%d\n",
            options->workers, mode_name(options->mode), options->device);
    if (options->worker_gpus > 0) {
        fprintf(output, "# distribution=balanced-cache-gpu\n");
        fprintf(output, "# worker_gpus=%d\n", options->worker_gpus);
        fprintf(output, "# worker_caches=%d\n", options->worker_caches);
        fprintf(output, "# cache_assignment=worker_mod_cache_count\n");
        fprintf(output, "# gpu_assignment=cache_slot_mod_gpu_count\n");
    }
    fprintf(output,
            "# hold_ms=%d\n# worker_launch=exec\n# worker_preload=%s\n# barriers=load-symbol,api\n"
            "# outcome=%s\n# results_received=%d\n# setup_wall_us=%" PRId64 "\n# api_wall_us=%" PRId64 "\n",
            options->hold_ms, options->worker_preload == NULL ? "disabled" : "enabled", outcome, results_received,
            setup_wall_us, api_wall_us);
    fprintf(output,
            "worker,pid,cache_slot,gpu_slot,status,stage,api_result,load_us,symbol_us,cuinit_us,device_us,"
            "retain_us,hold_us,release_us,api_us\n");
    for (index = 0; index < options->workers; index++) {
        const struct worker_result *result = &results[index];
        const char *status = result->stage == STAGE_OK ? "ok" : "failed";

        if (result->stage != STAGE_OK) {
            failures++;
        }
        fprintf(output,
                "%d,%d,%d,%d,%s,%s,%d,%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64
                ",%" PRId64 ",%" PRId64 "\n",
                result->worker, result->pid, result->cache_slot, result->gpu_slot, status,
                stage_name(result->stage), result->api_result, result->load_us, result->symbol_us,
                result->cuinit_us, result->device_us, result->retain_us, result->hold_us, result->release_us,
                result->api_us);
    }
    if (output != stdout && fclose(output) != 0) {
        return -1;
    }
    return failures == 0 ? 0 : 1;
}

static int collect_results(int fd, const struct options *options, const struct timespec *deadline,
                           struct worker_result *results, unsigned char *received, int *results_received) {
    int index;

    *results_received = 0;
    for (index = 0; index < options->workers; index++) {
        struct worker_result result;

        if (read_exact_before(fd, &result, sizeof(result), deadline) != 0) {
            return -1;
        }
        if (result.worker >= 0 && result.worker < options->workers && !received[result.worker]) {
            results[result.worker] = result;
            received[result.worker] = 1;
            (*results_received)++;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    struct options options;
    struct timespec deadline, setup_begin = {0}, setup_end = {0}, api_begin = {0}, api_end = {0};
    struct worker_result *results = NULL;
    pid_t *children = NULL;
    unsigned char *tokens = NULL, *received = NULL;
    int ready_pipe[2] = {-1, -1}, setup_pipe[2] = {-1, -1}, prepared_pipe[2] = {-1, -1};
    int api_pipe[2] = {-1, -1}, result_pipe[2] = {-1, -1};
    int children_started = 0, results_received = 0, exit_code = 1, output_result, index;
    int64_t setup_wall_us = -1, api_wall_us = -1;
    const char *outcome = "error";

    if (argc > 1 && strcmp(argv[1], "--internal-worker") == 0) {
        return internal_worker_main(argc, argv);
    }
    if (reexec_controller_without_preload(argv) != 0) {
        perror("clear controller preload");
        return 127;
    }
    if (parse_options(argc, argv, &options) != 0) {
        usage(argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    results = calloc((size_t)options.workers, sizeof(*results));
    children = calloc((size_t)options.workers, sizeof(*children));
    tokens = malloc((size_t)options.workers);
    received = calloc((size_t)options.workers, sizeof(*received));
    if (results == NULL || children == NULL || tokens == NULL || received == NULL || pipe(ready_pipe) != 0 ||
        pipe(setup_pipe) != 0 || pipe(prepared_pipe) != 0 || pipe(api_pipe) != 0 || pipe(result_pipe) != 0) {
        perror("prepare benchmark");
        goto cleanup;
    }
    memset(tokens, 1, (size_t)options.workers);

    for (index = 0; index < options.workers; index++) {
        pid_t child = fork();

        if (child < 0) {
            perror("fork worker");
            goto cleanup;
        }
        if (child == 0) {
            (void)close(ready_pipe[0]);
            (void)close(setup_pipe[1]);
            (void)close(prepared_pipe[0]);
            (void)close(api_pipe[1]);
            (void)close(result_pipe[0]);
            exec_worker(argv[0], index, &options, ready_pipe[1], setup_pipe[0], prepared_pipe[1], api_pipe[0],
                        result_pipe[1]);
        }
        children[index] = child;
        children_started++;
    }
    close(ready_pipe[1]);
    ready_pipe[1] = -1;
    close(setup_pipe[0]);
    setup_pipe[0] = -1;
    close(prepared_pipe[1]);
    prepared_pipe[1] = -1;
    close(api_pipe[0]);
    api_pipe[0] = -1;
    close(result_pipe[1]);
    result_pipe[1] = -1;

    if (make_deadline(&deadline, options.timeout_seconds) != 0 ||
        read_exact_before(ready_pipe[0], tokens, (size_t)options.workers, &deadline) != 0) {
        outcome = errno == ETIMEDOUT ? "timeout" : "ready-error";
        perror("wait for worker ready");
        goto cleanup;
    }
    close(ready_pipe[0]);
    ready_pipe[0] = -1;
    if (monotonic_now(&setup_begin) != 0 ||
        write_exact(setup_pipe[1], tokens, (size_t)options.workers) != 0) {
        outcome = "setup-release-error";
        perror("release setup barrier");
        goto cleanup;
    }
    close(setup_pipe[1]);
    setup_pipe[1] = -1;
    if (read_exact_before(prepared_pipe[0], tokens, (size_t)options.workers, &deadline) != 0 ||
        monotonic_now(&setup_end) != 0) {
        outcome = errno == ETIMEDOUT ? "timeout" : "prepare-error";
        perror("wait for worker preparation");
        goto cleanup;
    }
    setup_wall_us = elapsed_us(&setup_begin, &setup_end);
    close(prepared_pipe[0]);
    prepared_pipe[0] = -1;
    if (monotonic_now(&api_begin) != 0 || write_exact(api_pipe[1], tokens, (size_t)options.workers) != 0) {
        outcome = "api-release-error";
        perror("release API barrier");
        goto cleanup;
    }
    close(api_pipe[1]);
    api_pipe[1] = -1;
    if (collect_results(result_pipe[0], &options, &deadline, results, received, &results_received) != 0 ||
        monotonic_now(&api_end) != 0) {
        outcome = errno == ETIMEDOUT ? "timeout" : "collect-error";
        perror("collect worker results");
        goto cleanup;
    }
    api_wall_us = elapsed_us(&api_begin, &api_end);
    close(result_pipe[0]);
    result_pipe[0] = -1;
    outcome = "complete";
    exit_code = wait_for_children(children, options.workers);

cleanup:
    close_pipe(ready_pipe);
    close_pipe(setup_pipe);
    close_pipe(prepared_pipe);
    close_pipe(api_pipe);
    close_pipe(result_pipe);
    if (children != NULL) {
        terminate_children(children, children_started);
    }
    if (results != NULL && received != NULL && children != NULL) {
        fill_missing_results(&options, results, children, received,
                             strcmp(outcome, "timeout") == 0 ? STAGE_TIMEOUT : STAGE_LOST);
        output_result = write_output(&options, results, outcome, results_received, setup_wall_us, api_wall_us);
        if (output_result < 0) {
            perror("write benchmark output");
            exit_code = 1;
        } else if (output_result != 0 || strcmp(outcome, "complete") != 0) {
            exit_code = 1;
        }
    }
    free(received);
    free(tokens);
    free(children);
    free(results);
    return exit_code;
}
