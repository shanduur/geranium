#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct dirent *__real_readdir(DIR *);
/* Return status first, then interrupt only after production unlink removed it. */
struct dirent *__wrap_readdir(DIR *dir)
{
	static int phase;
	static struct dirent status;
	const char *mode = getenv("REMOVE_FAULT");
	if (mode && !phase && faccessat(dirfd(dir), "status", F_OK, 0) == 0) {
		phase = 1;
		strcpy(status.d_name, "status");
		return &status;
	}
	if (mode && phase == 1) {
		if (faccessat(dirfd(dir), "status", F_OK, 0) == 0)
			_exit(99);
		phase = 2;
		strcpy(status.d_name, "command");
		return &status;
	}
	if (mode && phase == 2) {
		phase = 3;
		if (faccessat(dirfd(dir), "status", F_OK, 0) == 0)
			_exit(99);
		if (!strcmp(mode, "eio")) {
			errno = EIO;
			return NULL;
		}
		int fd = open(getenv("REMOVE_READY"),
			      O_CREAT | O_WRONLY | O_EXCL, 0600);
		if (fd < 0 || close(fd) < 0)
			_exit(99);
		for (;;)
			pause();
	}
	return __real_readdir(dir);
}
#include <sys/stat.h>
#include <stdio.h>
static int fault(const char *name)
{
	const char *f = getenv("REMOVE_OP");
	return f && !strcmp(f, name);
}
int __real_renameat(int, const char *, int, const char *);
int __wrap_renameat(int a, const char *b, int c, const char *d)
{
	if (fault("rename")) {
		errno = EIO;
		return -1;
	}
	return __real_renameat(a, b, c, d);
}
int __real_fsync(int);
int __wrap_fsync(int fd)
{
	static int count;
	++count;
	if ((fault("sync-unpublish") && count == 1) ||
	    (fault("sync-remove") && count == 2)) {
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}
static int selected(int fd)
{
	struct stat a, b;
	const char *path = getenv("REMOVE_STAGE");
	return path && fstat(fd, &a) == 0 && stat(path, &b) == 0 &&
	       a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
int __real_close(int);
int __wrap_close(int fd)
{
	int hit = fault("close") && selected(fd), rc = __real_close(fd);
	if (hit) {
		errno = EIO;
		return -1;
	}
	return rc;
}
int __real_closedir(DIR *);
int __wrap_closedir(DIR *dir)
{
	int hit = fault("closedir") && selected(dirfd(dir)),
	    rc = __real_closedir(dir);
	if (hit) {
		errno = EIO;
		return -1;
	}
	return rc;
}
int __real_unlinkat(int, const char *, int);
int __wrap_unlinkat(int fd, const char *name, int flags)
{
	if (fault("unlink-stage") && flags == AT_REMOVEDIR &&
	    !strncmp(name, ".pending-", 9)) {
		errno = EIO;
		return -1;
	}
	return __real_unlinkat(fd, name, flags);
}
