#ifndef GERANIUM_BACKEND_H
#define GERANIUM_BACKEND_H
#include <signal.h>
int backend_supported(void);
const char *backend_image_arch(void);
/* Child calls before setup. Parent returns 1 only after kernel exec event;
 * 0 means child reaped into status; -1 means error (child still owned).
 * No tracing remains after success. */
int backend_exec_prepare(void);
/* ack is the startup pipe writer: POLLERR detects loss of its reader.
 * deadline is absolute CLOCK_MONOTONIC milliseconds. Cancellation/error
 * leaves the direct child owned by init for termination and reaping. */
int backend_exec_wait(int child, int *status, int owner, int ack,
		      long long deadline, const volatile sig_atomic_t *cancel);
int backend_watch_parent(int parent_pid);
int backend_close_inherited(const int *keep, unsigned count);
/* 1 in short lived launcher, 0 in namespace PID 1; errors never execute payload. */
int backend_enter(const char *rootfs);
int backend_pidfd_open(int pid);
int backend_signal(int pidfd, int signal_number);
#endif
