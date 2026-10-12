#include "backend.h"
#include <errno.h>
int backend_supported(void)
{
	return 0;
}
int backend_watch_parent(int parent_pid)
{
	(void)parent_pid;
	errno = ENOSYS;
	return -1;
}
int backend_enter(const char *rootfs)
{
	(void)rootfs;
	errno = ENOSYS;
	return -1;
}
int backend_pidfd_open(int pid)
{
	(void)pid;
	errno = ENOSYS;
	return -1;
}
int backend_signal(int fd, int sig)
{
	(void)fd;
	(void)sig;
	errno = ENOSYS;
	return -1;
}
int backend_close_inherited(const int *keep, unsigned count)
{
	(void)keep;
	(void)count;
	errno = ENOSYS;
	return -1;
}
int backend_exec_prepare(void)
{
	errno = ENOSYS;
	return -1;
}
int backend_exec_wait(int child, int *status, int owner, int ack,
		      long long deadline, const volatile sig_atomic_t *cancel)
{
	(void)child;
	(void)status;
	(void)owner;
	(void)ack;
	(void)deadline;
	(void)cancel;
	errno = ENOSYS;
	return -1;
}

const char *backend_image_arch(void)
{
	return 0;
}
