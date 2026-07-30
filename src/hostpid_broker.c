#include "include/hostpid_broker.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define HOSTPID_PROTOCOL_VERSION 1
#define HOSTPID_COMMAND_GET_PID 1
#define HOSTPID_STATUS_OK 0
#define HOSTPID_REQUEST_SIZE 8
#define HOSTPID_RESPONSE_SIZE 12
#define HOSTPID_TIMEOUT_MS 500

static const unsigned char hostpid_magic[4] = {'H', 'P', 'I', 'D'};

static int wait_for_connect(int fd) {
    struct pollfd descriptor = {
        .fd = fd,
        .events = POLLOUT,
    };
    int result;

    do {
        result = poll(&descriptor, 1, HOSTPID_TIMEOUT_MS);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    if (result < 0) {
        return -1;
    }

    int socket_error = 0;
    socklen_t error_length = sizeof(socket_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) < 0) {
        return -1;
    }
    if (socket_error != 0) {
        errno = socket_error;
        return -1;
    }
    return 0;
}

static int connect_with_timeout(int fd, const struct sockaddr_un *address,
                                socklen_t address_length) {
    int original_flags = fcntl(fd, F_GETFL, 0);
    if (original_flags < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) < 0) {
        return -1;
    }

    int result = connect(fd, (const struct sockaddr *)address, address_length);
    if (result < 0 && errno != EINPROGRESS && errno != EAGAIN) {
        int saved_errno = errno;
        (void)fcntl(fd, F_SETFL, original_flags);
        errno = saved_errno;
        return -1;
    }
    if (result < 0 && wait_for_connect(fd) < 0) {
        int saved_errno = errno;
        (void)fcntl(fd, F_SETFL, original_flags);
        errno = saved_errno;
        return -1;
    }
    if (fcntl(fd, F_SETFL, original_flags) < 0) {
        return -1;
    }
    return 0;
}

static int write_full(int fd, const unsigned char *buffer, size_t length) {
    while (length > 0) {
#ifdef MSG_NOSIGNAL
        ssize_t written = send(fd, buffer, length, MSG_NOSIGNAL);
#else
        ssize_t written = send(fd, buffer, length, 0);
#endif
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            if (written == 0) {
                errno = EPIPE;
            }
            return -1;
        }
        buffer += written;
        length -= (size_t)written;
    }
    return 0;
}

static int read_full(int fd, unsigned char *buffer, size_t length) {
    while (length > 0) {
        ssize_t received = recv(fd, buffer, length, 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            if (received == 0) {
                errno = ECONNRESET;
            }
            return -1;
        }
        buffer += received;
        length -= (size_t)received;
    }
    return 0;
}

static uint16_t read_u16(const unsigned char *buffer) {
    return (uint16_t)((uint16_t)buffer[0] << 8 | buffer[1]);
}

static uint32_t read_u32(const unsigned char *buffer) {
    return (uint32_t)buffer[0] << 24 |
           (uint32_t)buffer[1] << 16 |
           (uint32_t)buffer[2] << 8 |
           (uint32_t)buffer[3];
}

static int validate_trusted_socket(const char *socket_path) {
    struct stat socket_stat;
    struct stat directory_stat;
    char directory[sizeof(((struct sockaddr_un *)0)->sun_path)];
    const char *last_slash = strrchr(socket_path, '/');
    size_t directory_length;

    if (last_slash == NULL || last_slash == socket_path) {
        errno = EINVAL;
        return -1;
    }
    directory_length = (size_t)(last_slash - socket_path);
    if (directory_length >= sizeof(directory)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(directory, socket_path, directory_length);
    directory[directory_length] = '\0';

    if (lstat(directory, &directory_stat) < 0 ||
        lstat(socket_path, &socket_stat) < 0) {
        return -1;
    }
    if (!S_ISDIR(directory_stat.st_mode) ||
        !S_ISSOCK(socket_stat.st_mode)) {
        errno = ENOTSOCK;
        return -1;
    }
    if (directory_stat.st_uid != 0 || socket_stat.st_uid != 0 ||
        (directory_stat.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

int hostpid_broker_query(const char *socket_path, pid_t *host_pid) {
    static const unsigned char request[HOSTPID_REQUEST_SIZE] = {
        'H', 'P', 'I', 'D',
        0, HOSTPID_PROTOCOL_VERSION,
        0, HOSTPID_COMMAND_GET_PID,
    };
    unsigned char response[HOSTPID_RESPONSE_SIZE];
    struct sockaddr_un address;
    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = HOSTPID_TIMEOUT_MS * 1000,
    };
    size_t path_length;
    int fd;
    int saved_errno;

    if (socket_path == NULL || host_pid == NULL) {
        errno = EINVAL;
        return -1;
    }
    *host_pid = 0;
    path_length = strlen(socket_path);
    if (path_length == 0 || path_length >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) < 0) {
        saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
#ifdef SO_NOSIGPIPE
    int no_sigpipe = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe,
                   sizeof(no_sigpipe)) < 0) {
        saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
#endif

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, path_length + 1);
    socklen_t address_length =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_length + 1);
#ifdef __APPLE__
    address.sun_len = address_length;
#endif
    if (connect_with_timeout(fd, &address, address_length) < 0 ||
        write_full(fd, request, sizeof(request)) < 0 ||
        read_full(fd, response, sizeof(response)) < 0) {
        saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    close(fd);

    if (memcmp(response, hostpid_magic, sizeof(hostpid_magic)) != 0 ||
        read_u16(response + 4) != HOSTPID_PROTOCOL_VERSION ||
        read_u16(response + 6) != HOSTPID_STATUS_OK) {
        errno = EPROTO;
        return -1;
    }
    uint32_t pid = read_u32(response + 8);
    if (pid == 0 || pid > INT_MAX) {
        errno = ERANGE;
        return -1;
    }
    *host_pid = (pid_t)pid;
    return 0;
}

int hostpid_broker_query_trusted(const char *socket_path, pid_t *host_pid) {
    if (socket_path == NULL ||
        strcmp(socket_path, HOSTPID_BROKER_SOCKET_PATH) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (validate_trusted_socket(socket_path) < 0) {
        return -1;
    }
    return hostpid_broker_query(socket_path, host_pid);
}
