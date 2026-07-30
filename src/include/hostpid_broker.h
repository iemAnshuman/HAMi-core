#ifndef __HOSTPID_BROKER_H__
#define __HOSTPID_BROKER_H__

#include <sys/types.h>

#define HOSTPID_BROKER_SOCKET_PATH "/tmp/vgpulock/hostpid/broker.sock"

int hostpid_broker_query(const char *socket_path, pid_t *host_pid);
int hostpid_broker_query_trusted(const char *socket_path, pid_t *host_pid);

#endif
