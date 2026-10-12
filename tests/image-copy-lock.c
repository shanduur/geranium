#define _POSIX_C_SOURCE 200809L
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
ssize_t __real_read(int, void *, size_t);
ssize_t __wrap_read(int fd, void *buffer, size_t n)
{
	static int paused;
	const char *target = getenv("IMAGE_COPY_SOURCE"),
		   *ready = getenv("IMAGE_COPY_READY"),
		   *release = getenv("IMAGE_COPY_RELEASE");
	struct stat actual, wanted;
	if (!paused && target && ready && release && fstat(fd, &actual) == 0 &&
	    stat(target, &wanted) == 0 && actual.st_dev == wanted.st_dev &&
	    actual.st_ino == wanted.st_ino) {
		paused = 1;
		int marker = open(ready, O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (marker < 0)
			_exit(90);
		close(marker);
		struct timespec tick = { 0, 10000000 };
		for (int i = 0; i < 1000; i++) {
			if (access(release, F_OK) == 0)
				return __real_read(fd, buffer, n);
			nanosleep(&tick, NULL);
		}
		_exit(91);
	}
	return __real_read(fd, buffer, n);
}
