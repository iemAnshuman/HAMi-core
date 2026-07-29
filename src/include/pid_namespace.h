#ifndef SRC_INCLUDE_PID_NAMESPACE_H_
#define SRC_INCLUDE_PID_NAMESPACE_H_

#include <stddef.h>
#include <sys/types.h>

#define PID_NAMESPACE_MAX_LEVELS 32

/*
 * Parse the values after an NStgid field, for example:
 *
 *     NStgid:  4217  1
 *
 * Linux reports the IDs from the PID namespace associated with the procfs
 * mount toward successively nested namespaces. The first value is therefore
 * not necessarily an ID in the initial (host) PID namespace.
 */
int parse_nstgid_line(const char *line, pid_t *ids, size_t capacity,
                      size_t *count);

/* Read and parse the NStgid field from a proc status file. */
int read_nstgid_file(const char *path, pid_t *ids, size_t capacity,
                     size_t *count);

#endif  // SRC_INCLUDE_PID_NAMESPACE_H_
