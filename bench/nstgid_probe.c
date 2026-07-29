#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "../src/include/pid_namespace.h"

#define PROBE_LINE_MAX 16384

static void print_json_string(const char *value) {
    const unsigned char *cursor = (const unsigned char *)value;

    putchar('"');
    while (*cursor != '\0') {
        switch (*cursor) {
            case '\\':
                fputs("\\\\", stdout);
                break;
            case '"':
                fputs("\\\"", stdout);
                break;
            case '\n':
                fputs("\\n", stdout);
                break;
            case '\r':
                fputs("\\r", stdout);
                break;
            case '\t':
                fputs("\\t", stdout);
                break;
            default:
                if (*cursor < 0x20) {
                    printf("\\u%04x", *cursor);
                } else {
                    putchar(*cursor);
                }
        }
        cursor++;
    }
    putchar('"');
}

static int read_status_field(const char *path, const char *name, char *value,
                             size_t capacity) {
    FILE *status = fopen(path, "r");
    char *line = NULL;
    size_t line_capacity = 0;
    size_t name_length = strlen(name);
    int result = -1;

    if (status == NULL) {
        return -1;
    }
    while (getline(&line, &line_capacity, status) != -1) {
        if (strncmp(line, name, name_length) == 0 &&
            line[name_length] == ':') {
            char *start = line + name_length + 1;
            char *end;

            while (*start == ' ' || *start == '\t') {
                start++;
            }
            end = start + strcspn(start, "\r\n");
            *end = '\0';
            if (snprintf(value, capacity, "%s", start) >= (int)capacity) {
                errno = ENOSPC;
                break;
            }
            result = 0;
            break;
        }
    }
    free(line);
    fclose(status);
    return result;
}

static int read_link_value(const char *path, char *value, size_t capacity) {
    ssize_t length = readlink(path, value, capacity - 1);

    if (length < 0) {
        return -1;
    }
    value[length] = '\0';
    return 0;
}

static int read_proc_mount(char *value, size_t capacity) {
    FILE *mountinfo = fopen("/proc/self/mountinfo", "r");
    char *line = NULL;
    size_t line_capacity = 0;
    int result = -1;

    if (mountinfo == NULL) {
        return -1;
    }
    while (getline(&line, &line_capacity, mountinfo) != -1) {
        if (strstr(line, " /proc ") != NULL &&
            strstr(line, " - proc proc ") != NULL) {
            line[strcspn(line, "\r\n")] = '\0';
            if (snprintf(value, capacity, "%s", line) >= (int)capacity) {
                errno = ENOSPC;
                break;
            }
            result = 0;
            break;
        }
    }
    free(line);
    fclose(mountinfo);
    return result;
}

static void print_named_string(const char *name, const char *value,
                               int *needs_comma) {
    printf("%s\"%s\":", *needs_comma ? "," : "", name);
    print_json_string(value);
    *needs_comma = 1;
}

int main(int argc, char **argv) {
    static const char *fields[] = {"Pid", "Tgid", "NSpid", "NStgid"};
    const char *proc_root = "/proc";
    pid_t ids[PID_NAMESPACE_MAX_LEVELS];
    size_t count = 0;
    char status_path[PATH_MAX];
    char self_namespace_path[PATH_MAX];
    char init_namespace_path[PATH_MAX];
    char value[PROBE_LINE_MAX];
    int needs_comma = 0;
    size_t i;

    if (argc == 3 && strcmp(argv[1], "--proc-root") == 0) {
        proc_root = argv[2];
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--proc-root PATH]\n", argv[0]);
        return 2;
    }
    if (snprintf(status_path, sizeof(status_path), "%s/self/status",
                 proc_root) >= (int)sizeof(status_path) ||
        snprintf(self_namespace_path, sizeof(self_namespace_path),
                 "%s/self/ns/pid", proc_root) >=
                    (int)sizeof(self_namespace_path) ||
        snprintf(init_namespace_path, sizeof(init_namespace_path),
                 "%s/1/ns/pid", proc_root) >=
                    (int)sizeof(init_namespace_path)) {
        fprintf(stderr, "proc root path is too long\n");
        return 2;
    }

    printf("{\"getpid\":%ld,\"gettid\":%ld", (long)getpid(),
           (long)syscall(SYS_gettid));
    needs_comma = 1;

    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (read_status_field(status_path, fields[i], value,
                              sizeof(value)) == 0) {
            print_named_string(fields[i], value, &needs_comma);
        }
    }
    if (read_link_value(self_namespace_path, value, sizeof(value)) == 0) {
        print_named_string("self_pid_ns", value, &needs_comma);
    }
    if (read_link_value(init_namespace_path, value, sizeof(value)) == 0) {
        print_named_string("init_pid_ns", value, &needs_comma);
    }
    if (read_proc_mount(value, sizeof(value)) == 0) {
        print_named_string("proc_mount", value, &needs_comma);
    }

    if (read_nstgid_file(status_path, ids,
                         PID_NAMESPACE_MAX_LEVELS, &count) == 0) {
        printf(",\"nstgid_values\":[");
        for (i = 0; i < count; i++) {
            printf("%s%ld", i == 0 ? "" : ",", (long)ids[i]);
        }
        printf("],\"nstgid_count\":%zu", count);
    } else {
        printf(",\"nstgid_error\":");
        print_json_string(strerror(errno));
    }
    puts("}");
    return 0;
}
