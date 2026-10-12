#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;

static void append_log(const char *event) {
    const char *path = getenv("FAKE_INTERPOSER_LOG");
    char record[96];
    int length;
    int fd;

    if (path == NULL || path[0] == '\0') {
        return;
    }
    length = snprintf(record, sizeof(record), "%s %jd\n", event, (intmax_t)getpid());
    if (length <= 0 || (size_t)length >= sizeof(record)) {
        return;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) {
        return;
    }
    (void)write(fd, record, (size_t)length);
    (void)close(fd);
}

static void configured_delay(const char *name) {
    const char *value = getenv(name);
    char *end = NULL;
    intmax_t microseconds;
    struct timespec delay;

    if (value == NULL || value[0] == '\0') {
        return;
    }
    errno = 0;
    microseconds = strtoimax(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || microseconds <= 0) {
        return;
    }
    delay.tv_sec = microseconds / 1000000;
    delay.tv_nsec = (microseconds % 1000000) * 1000;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

__attribute__((constructor)) static void record_load(void) {
    append_log("load");
}

CUresult cuInit(unsigned int flags) {
    (void)flags;
    append_log("cuInit");
    configured_delay("FAKE_CUDA_INIT_US");
    return 0;
}

CUresult cuDeviceGet(CUdevice *device, int ordinal) {
    if (device == NULL || ordinal < 0) {
        return 1;
    }
    configured_delay("FAKE_CUDA_DEVICE_US");
    *device = ordinal;
    return 0;
}

CUresult cuDevicePrimaryCtxRetain(CUcontext *context, CUdevice device) {
    if (context == NULL || device < 0) {
        return 1;
    }
    configured_delay("FAKE_CUDA_RETAIN_US");
    *context = (void *)0x1;
    return 0;
}

CUresult cuDevicePrimaryCtxRelease_v2(CUdevice device) {
    if (device < 0) {
        return 1;
    }
    configured_delay("FAKE_CUDA_RELEASE_US");
    return 0;
}
