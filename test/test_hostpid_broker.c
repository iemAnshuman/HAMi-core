#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/include/hostpid_broker.h"

#define REQUEST_SIZE 8
#define RESPONSE_SIZE 12

static void write_response(int connection, uint16_t status, uint32_t pid) {
    unsigned char request[REQUEST_SIZE];
    unsigned char response[RESPONSE_SIZE] = {
        'H', 'P', 'I', 'D',
        0, 1,
        (unsigned char)(status >> 8), (unsigned char)status,
        (unsigned char)(pid >> 24), (unsigned char)(pid >> 16),
        (unsigned char)(pid >> 8), (unsigned char)pid,
    };

    assert(read(connection, request, sizeof(request)) ==
           (ssize_t)sizeof(request));
    assert(memcmp(request, "HPID\0\1\0\1", sizeof(request)) == 0);
    assert(write(connection, response, sizeof(response)) ==
           (ssize_t)sizeof(response));
}

static int make_listener(char *socket_path, size_t socket_path_size,
                         char *directory, size_t directory_size) {
    struct sockaddr_un address;
    int listener;

    assert(directory_size >= sizeof("/tmp/hami-hostpid-test-XXXXXX"));
    strcpy(directory, "/tmp/hami-hostpid-test-XXXXXX");
    assert(mkdtemp(directory) != NULL);
    assert(snprintf(socket_path, socket_path_size, "%s/broker.sock",
                    directory) > 0);

    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(listener >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, socket_path);
    socklen_t address_length =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                    strlen(socket_path) + 1);
#ifdef __APPLE__
    address.sun_len = address_length;
#endif
    if (bind(listener, (struct sockaddr *)&address, address_length) != 0) {
        perror("bind host PID broker test socket");
        abort();
    }
    assert(listen(listener, 4) == 0);
    return listener;
}

static void run_query_case(uint16_t status, uint32_t response_pid,
                           int expected_result, int expected_errno) {
    char directory[PATH_MAX];
    char socket_path[PATH_MAX];
    pid_t child;
    pid_t host_pid = 0;
    int listener = make_listener(socket_path, sizeof(socket_path),
                                 directory, sizeof(directory));

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int connection = accept(listener, NULL, NULL);
        assert(connection >= 0);
        write_response(connection, status, response_pid);
        close(connection);
        close(listener);
        _exit(0);
    }

    errno = 0;
    assert(hostpid_broker_query(socket_path, &host_pid) == expected_result);
    if (expected_result == 0) {
        assert(host_pid == (pid_t)response_pid);
    } else {
        assert(host_pid == 0);
        assert(errno == expected_errno);
    }

    close(listener);
    assert(waitpid(child, NULL, 0) == child);
    assert(unlink(socket_path) == 0);
    assert(rmdir(directory) == 0);
}

static void test_success(void) {
    run_query_case(0, 43210, 0, 0);
}

static void test_server_error(void) {
    run_query_case(2, 0, -1, EPROTO);
}

static void test_invalid_pid(void) {
    run_query_case(0, 0, -1, ERANGE);
}

static void test_missing_socket(void) {
    pid_t host_pid = 99;

    errno = 0;
    assert(hostpid_broker_query("/tmp/hami-hostpid-does-not-exist",
                                &host_pid) == -1);
    assert(host_pid == 0);
    assert(errno == ENOENT);
}

static void test_rejects_untrusted_path(void) {
    pid_t host_pid = 99;

    errno = 0;
    assert(hostpid_broker_query_trusted("/tmp/other.sock", &host_pid) == -1);
    assert(errno == EINVAL);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_success();
    test_server_error();
    test_invalid_pid();
    test_missing_socket();
    test_rejects_untrusted_path();
    puts("host PID broker client tests passed");
    return 0;
}
