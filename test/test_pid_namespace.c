#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../src/include/pid_namespace.h"

static void test_single_id(void) {
    pid_t ids[PID_NAMESPACE_MAX_LEVELS];
    size_t count = 0;

    assert(parse_nstgid_line("NStgid:\t4217\n", ids,
                            PID_NAMESPACE_MAX_LEVELS, &count) == 0);
    assert(count == 1);
    assert(ids[0] == 4217);
}

static void test_nested_ids(void) {
    pid_t ids[PID_NAMESPACE_MAX_LEVELS];
    size_t count = 0;

    assert(parse_nstgid_line("NStgid:\t4217\t29\t1\n", ids,
                            PID_NAMESPACE_MAX_LEVELS, &count) == 0);
    assert(count == 3);
    assert(ids[0] == 4217);
    assert(ids[1] == 29);
    assert(ids[2] == 1);
}

static void test_rejects_invalid_input(void) {
    pid_t ids[2];
    size_t count = 99;

    errno = 0;
    assert(parse_nstgid_line("NSpid:\t10\t1\n", ids, 2, &count) == -1);
    assert(errno == EINVAL);
    assert(count == 0);

    errno = 0;
    assert(parse_nstgid_line("NStgid:\t10x\n", ids, 2, &count) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(parse_nstgid_line("NStgid:\t0\n", ids, 2, &count) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(parse_nstgid_line("NStgid:\t2147483648\n", ids, 2,
                            &count) == -1);
    assert(errno == EINVAL);
}

static void test_reports_small_destination(void) {
    pid_t ids[1];
    size_t count = 0;

    errno = 0;
    assert(parse_nstgid_line("NStgid:\t4217\t1\n", ids, 1,
                            &count) == -1);
    assert(errno == ENOSPC);
}

static void test_reads_status_file(void) {
    static const char status[] = "Name:\ttest\nNStgid:\t4217\t1\n";
    char path[] = "/tmp/hami-pid-namespace-XXXXXX";
    pid_t ids[PID_NAMESPACE_MAX_LEVELS];
    size_t count = 0;
    int fd = mkstemp(path);

    assert(fd >= 0);
    assert(write(fd, status, sizeof(status) - 1) ==
           (ssize_t)(sizeof(status) - 1));
    assert(close(fd) == 0);
    assert(read_nstgid_file(path, ids, PID_NAMESPACE_MAX_LEVELS,
                            &count) == 0);
    assert(count == 2);
    assert(ids[0] == 4217);
    assert(ids[1] == 1);
    assert(unlink(path) == 0);
}

int main(void) {
    test_single_id();
    test_nested_ids();
    test_rejects_invalid_input();
    test_reports_small_destination();
    test_reads_status_file();
    puts("pid namespace parser tests passed");
    return 0;
}
