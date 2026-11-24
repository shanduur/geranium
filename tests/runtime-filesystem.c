#define _GNU_SOURCE
#include "../src/backend.h"
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	int entered = backend_enter(argv[1]);
	if (entered == 1) {
		int status;
		if (wait(&status) < 0 || !WIFEXITED(status))
			return 1;
		if (WEXITSTATUS(status)) {
			fprintf(stderr, "runtime filesystem check failed: %d\n",
				WEXITSTATUS(status));
			return 1;
		}
		return 0;
	}
	if (entered != 0)
		return 2;
	FILE *proc = fopen("/proc/self/stat", "r");
	if (!proc)
		return 10;
	fclose(proc);
	struct stat st;
	if (stat("/dev/null", &st) < 0 || !S_ISCHR(st.st_mode))
		return 11;
	int fd = open("/dev/null", O_WRONLY);
	if (fd < 0 || write(fd, "x", 1) != 1)
		return 12;
	close(fd);
	fd = open("/dev/urandom", O_RDONLY);
	char random;
	if (fd < 0 || read(fd, &random, 1) != 1)
		return 13;
	close(fd);
	if (stat("/tmp", &st) < 0 || (st.st_mode & 07777) != 01777)
		return 16;
	fd = open("/tmp/check", O_CREAT | O_WRONLY, 0600);
	if (fd < 0 || write(fd, "x", 1) != 1)
		return 14;
	close(fd);
	if (stat("/dev/fd/0", &st) < 0)
		return 15;
	struct statfs fs;
	if (statfs("/dev/pts", &fs) < 0 || (unsigned long)fs.f_type != 0x1cd1)
		return 17;
	fd = posix_openpt(O_RDWR | O_NOCTTY);
	if (fd < 0 || grantpt(fd) < 0 || unlockpt(fd) < 0)
		return 18;
	close(fd);
	if (statfs("/dev/shm", &fs) < 0 ||
	    (unsigned long)fs.f_type != 0x01021994)
		return 19;
	return 0;
}
