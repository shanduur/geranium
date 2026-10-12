#define _POSIX_C_SOURCE 200809L
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
ssize_t __real_read(int, void *, size_t);
ssize_t __wrap_read(int fd, void *buf, size_t size)
{
	static int saw_eof, exited;
	char path[64], target[4096];
	snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
	ssize_t n = readlink(path, target, sizeof target - 1);
	int islog = 0, isstatus = 0;
	if (n > 0) {
		target[n] = 0;
		islog = n >= 4 && !strcmp(target + n - 4, "/log");
		isstatus = n >= 7 && !strcmp(target + n - 7, "/status");
	}
	if (islog && saw_eof && exited && getenv("FAIL_TAIL")) {
		const char msg[] =
			"INJECT tail read EIO with final log data pending\n";
		ssize_t ignored = write(2, msg, sizeof msg - 1);
		(void)ignored;
		errno = EIO;
		return -1;
	}
	ssize_t r = __real_read(fd, buf, size);
	if (islog && r == 0 && !saw_eof) {
		saw_eof = 1;
		strcpy(target + n - 4, "/status");
		/* Only schedule the client: let real init append the tail and publish exit. */
		for (int i = 0; i < 400; i++) {
			char state[16] = { 0 };
			int s = open(target, O_RDONLY);
			if (s >= 0) {
				ssize_t got =
					__real_read(s, state, sizeof state);
				close(s);
				if (got == 7 && !memcmp(state, "exited\n", 7)) {
					char runtime[4096];
					strcpy(runtime, target);
					strcpy(runtime + strlen(runtime) - 7,
					       "/runtime");
					int lease = open(runtime, O_RDWR);
					struct flock l = { 0 };
					l.l_type = F_WRLCK;
					l.l_whence = SEEK_SET;
					int released = lease >= 0 &&
						       fcntl(lease, F_GETLK,
							     &l) == 0 &&
						       l.l_type == F_UNLCK;
					if (lease >= 0)
						close(lease);
					if (released)
						break;
				}
			}
			struct timespec delay = { 0, 10000000 };
			nanosleep(&delay, NULL);
		}
	}
	if (isstatus && r == 7 && !memcmp(buf, "exited\n", 7))
		exited = 1;
	return r;
}
