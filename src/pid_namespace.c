#include "include/pid_namespace.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int parse_nstgid_line(const char *line, pid_t *ids, size_t capacity,
                      size_t *count) {
    static const char field[] = "NStgid:";
    const char *cursor;
    size_t parsed = 0;

    if (line == NULL || ids == NULL || count == NULL || capacity == 0) {
        errno = EINVAL;
        return -1;
    }
    *count = 0;

    if (strncmp(line, field, sizeof(field) - 1) != 0) {
        errno = EINVAL;
        return -1;
    }
    cursor = line + sizeof(field) - 1;

    while (*cursor != '\0' && *cursor != '\n') {
        char *end = NULL;
        long value;

        while (isspace((unsigned char)*cursor) && *cursor != '\n') {
            cursor++;
        }
        if (*cursor == '\0' || *cursor == '\n') {
            break;
        }
        if (parsed == capacity) {
            errno = ENOSPC;
            return -1;
        }

        errno = 0;
        value = strtol(cursor, &end, 10);
        if (errno != 0 || end == cursor || value <= 0 || value > INT_MAX) {
            errno = EINVAL;
            return -1;
        }
        if (*end != '\0' && *end != '\n' &&
            !isspace((unsigned char)*end)) {
            errno = EINVAL;
            return -1;
        }

        ids[parsed++] = (pid_t)value;
        cursor = end;
    }

    if (parsed == 0) {
        errno = EINVAL;
        return -1;
    }
    *count = parsed;
    return 0;
}

int read_nstgid_file(const char *path, pid_t *ids, size_t capacity,
                     size_t *count) {
    FILE *status;
    char *line = NULL;
    size_t line_capacity = 0;
    int result = -1;
    int found = 0;
    int saved_errno = 0;

    if (path == NULL || ids == NULL || count == NULL || capacity == 0) {
        errno = EINVAL;
        return -1;
    }
    *count = 0;

    status = fopen(path, "r");
    if (status == NULL) {
        return -1;
    }

    while (getline(&line, &line_capacity, status) != -1) {
        if (strncmp(line, "NStgid:", sizeof("NStgid:") - 1) == 0) {
            found = 1;
            result = parse_nstgid_line(line, ids, capacity, count);
            if (result != 0) {
                saved_errno = errno;
            }
            break;
        }
    }
    if (!found) {
        saved_errno = ferror(status) && errno != 0 ? errno : ENOENT;
    }

    free(line);
    fclose(status);
    if (result != 0) {
        errno = saved_errno;
    }
    return result;
}
