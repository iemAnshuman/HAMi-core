#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../src/include/hostpid_broker.h"

static long long elapsed_microseconds(const struct timespec *start,
                                      const struct timespec *end) {
    return (long long)(end->tv_sec - start->tv_sec) * 1000000LL +
           (end->tv_nsec - start->tv_nsec) / 1000LL;
}

int main(int argc, char **argv) {
    struct timespec start;
    struct timespec end;
    pid_t host_pid = 0;
    unsigned int hold_seconds = 0;

    if (argc > 2) {
        fprintf(stderr, "usage: %s [hold-seconds]\n", argv[0]);
        return 2;
    }
    if (argc == 2) {
        char *end_pointer = NULL;
        unsigned long parsed = strtoul(argv[1], &end_pointer, 10);
        if (argv[1][0] == '\0' || end_pointer == NULL ||
            *end_pointer != '\0' || parsed > 60) {
            fprintf(stderr, "invalid hold-seconds: %s\n", argv[1]);
            return 2;
        }
        hold_seconds = (unsigned int)parsed;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        perror("clock_gettime");
        return 1;
    }
    int result = hostpid_broker_query_trusted(
        HOSTPID_BROKER_SOCKET_PATH, &host_pid);
    int saved_errno = errno;
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        perror("clock_gettime");
        return 1;
    }

    printf("{\"container_pid\":%d,\"host_pid\":%d,"
           "\"latency_us\":%lld,\"socket\":\"%s\","
           "\"result\":%d,\"errno\":%d,\"error\":\"%s\"}\n",
           getpid(), host_pid, elapsed_microseconds(&start, &end),
           HOSTPID_BROKER_SOCKET_PATH, result, saved_errno,
           result == 0 ? "" : strerror(saved_errno));
    fflush(stdout);
    if (result == 0 && hold_seconds > 0) {
        sleep(hold_seconds);
    }
    return result == 0 ? 0 : 1;
}
