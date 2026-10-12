#define _POSIX_C_SOURCE 200809L
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
static int status_fd = -1, terminal, result_dir = -1;
static int active(const char *s)
{
	const char *m = getenv("COMPLETION_FAULT");
	return m && !strcmp(m, s);
}
int __real_openat(int, const char *, int, ...);
int __wrap_openat(int dir, const char *path, int flags, ...)
{
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	int fd = __real_openat(dir, path, flags, mode);
	if (fd >= 0 && !strcmp(path, "status.tmp")) {
		status_fd = fd;
		terminal = 0;
	}
	return fd;
}
ssize_t __real_write(int, const void *, size_t);
ssize_t __wrap_write(int fd, const void *buf, size_t size)
{
	if (fd == status_fd && size == 7 && !memcmp(buf, "exited\n", 7))
		terminal = 1;
	return __real_write(fd, buf, size);
}
int __real_fsync(int);
int __wrap_fsync(int fd)
{
	if (getpid() == 1 &&
	    ((active("status-sync") && fd == status_fd && terminal) ||
	     (active("directory-sync") && fd == result_dir))) {
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}
int __real_renameat(int, const char *, int, const char *);
int __wrap_renameat(int a, const char *b, int c, const char *d)
{
	int r = __real_renameat(a, b, c, d);
	if (r == 0 && getpid() == 1 && !strcmp(d, "exitcode") &&
	    active("kill")) {
		int fd = openat(c, "result-window", O_CREAT | O_WRONLY, 0600);
		if (fd < 0 || close(fd) < 0)
			_exit(99);
		for (;;)
			pause();
	}
	if (r == 0 && getpid() == 1 && !strcmp(d, "status") && terminal)
		result_dir = c;
	return r;
}
