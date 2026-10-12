#define _POSIX_C_SOURCE 200809L
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
/* Scheduling only: all storage, locks, identity and workload behavior is real. */
static void marker(const char *variable)
{
	const char *path = getenv(variable);
	if (!path)
		return;
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0 && errno != EEXIST) {
		perror("fixture marker");
		_exit(99);
	}
	if (fd >= 0)
		close(fd);
}
static void gate(void)
{
	marker("STALL_READY");
	const char *path = getenv("STALL_RELEASE");
	if (!path)
		_exit(99);
	while (access(path, F_OK) < 0) {
		struct timespec delay = { 0, 1000000 };
		nanosleep(&delay, NULL);
	}
}
ssize_t __real_read(int, void *, size_t);
ssize_t __wrap_read(int fd, void *buffer, size_t size)
{
	const char *path = getenv("STALL_COPY");
	struct stat a, b;
	static int stalled;
	if (!stalled && path && fstat(fd, &a) == 0 && stat(path, &b) == 0 &&
	    a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
		stalled = 1;
		gate();
	}
	return __real_read(fd, buffer, size);
}
int __real_unlinkat(int, const char *, int);
int __wrap_unlinkat(int dir, const char *name, int flags)
{
	if (getenv("STALL_RM") && flags == AT_REMOVEDIR &&
	    !strcmp(name, "rootfs"))
		gate();
	return __real_unlinkat(dir, name, flags);
}
int __real_setpgid(pid_t, pid_t);
int __wrap_setpgid(pid_t p, pid_t g)
{
	if (getenv("STALL_START"))
		gate();
	return __real_setpgid(p, g);
}
int __real_fcntl(int, int, ...);
static int stop_observations;
int __wrap_fcntl(int fd, int command, ...)
{
	va_list args;
	va_start(args, command);
	int result;
	if (command == F_SETLK || command == F_SETLKW || command == F_GETLK) {
		struct flock *lock = va_arg(args, struct flock *);
		if (command == F_SETLKW && getenv("LOCK_READY")) {
			struct flock observed = *lock;
			if (__real_fcntl(fd, F_GETLK, &observed) < 0 ||
			    observed.l_type == F_UNLCK) {
				fprintf(stderr,
					"fixture waiter did not observe a real lock owner\n");
				_exit(99);
			}
			marker("LOCK_READY");
		}
		if (command == F_GETLK && getenv("STALL_STOP") &&
		    ++stop_observations == 3)
			gate();
		result = __real_fcntl(fd, command, lock);
	} else if (command == F_SETFD || command == F_SETFL ||
		   command == F_DUPFD || command == F_DUPFD_CLOEXEC) {
		result = __real_fcntl(fd, command, va_arg(args, int));
	} else
		result = __real_fcntl(fd, command);
	va_end(args);
	return result;
}
