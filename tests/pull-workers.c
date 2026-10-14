#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

static pid_t owner;
static unsigned forks;
static int fault(const char *name)
{
	const char *value = getenv("GERANIUM_PULL_FAULT");
	return value && !strcmp(value, name);
}
pid_t __real_fork(void);
pid_t __wrap_fork(void)
{
	if (!owner)
		owner = getpid();
	if ((++forks == 2 && fault("fork2")) || fault("fork1")) {
		errno = EAGAIN;
		return -1;
	}
	return __real_fork();
}
int __real_fsync(int);
int __wrap_fsync(int fd)
{
	if (owner && getpid() != owner && fault("worker-sync")) {
		errno = ENOSPC;
		return -1;
	}
	return __real_fsync(fd);
}
void *__real_mmap(void *, size_t, int, int, int, off_t);
void *__wrap_mmap(void *address, size_t size, int prot, int flags, int fd,
		  off_t offset)
{
	if (owner == getpid() && fault("map")) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	return __real_mmap(address, size, prot, flags, fd, offset);
}
int __real_openat(int, const char *, int, ...);
int __wrap_openat(int fd, const char *path, int flags, ...)
{
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list args;
		va_start(args, flags);
		mode = va_arg(args, int);
		va_end(args);
	}
	if (fault("stage2") && !strcmp(path, ".layer-1")) {
		errno = ENOSPC;
		return -1;
	}
	return __real_openat(fd, path, flags, mode);
}
int __real_sched_getaffinity(pid_t, size_t, cpu_set_t *);
int __wrap_sched_getaffinity(pid_t pid, size_t size, cpu_set_t *mask)
{
	if (getenv("GERANIUM_PULL_CPUS")) {
		errno = EINVAL;
		return -1;
	}
	return __real_sched_getaffinity(pid, size, mask);
}
long __real_sysconf(int);
long __wrap_sysconf(int name)
{
	const char *value = getenv("GERANIUM_PULL_CPUS");
	if (name == _SC_NPROCESSORS_ONLN && value)
		return strtol(value, NULL, 10);
	return __real_sysconf(name);
}
int __real_backend_watch_parent(int);
int __wrap_backend_watch_parent(int pid)
{
	if (fault("worker-start")) {
		errno = EPERM;
		return -1;
	}
	return __real_backend_watch_parent(pid);
}
int __real_atexit(void (*)(void));
int __wrap_atexit(void (*callback)(void))
{
	if (owner && getpid() != owner && fault("worker-guard"))
		return -1;
	return __real_atexit(callback);
}
