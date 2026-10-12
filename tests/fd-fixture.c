#define _POSIX_C_SOURCE 200809L
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "kill-init")) {
		int fd = open(argv[2], O_RDWR);
		struct flock lock = { 0 };
		lock.l_type = F_WRLCK;
		lock.l_whence = SEEK_SET;
		if (fd < 0 || fcntl(fd, F_GETLK, &lock) < 0 ||
		    lock.l_type == F_UNLCK)
			return 1;
		return kill(lock.l_pid, SIGKILL) < 0;
	}
	if (argc == 2 && !strcmp(argv[1], "check")) {
		if (fcntl(2048, F_GETFD) >= 0) {
			puts("host descriptor leaked");
			return 1;
		}
		puts("descriptors isolated");
		return 0;
	}
	if (argc < 3)
		return 1;
	struct rlimit r;
	if (getrlimit(RLIMIT_NOFILE, &r) < 0)
		return 1;
	if (r.rlim_cur <= 2048) {
		if (r.rlim_max <= 2048) {
			fputs("descriptor fixture requires a hard limit above 2048\n",
			      stderr);
			return 1;
		}
		r.rlim_cur = 2048 + 1;
		if (setrlimit(RLIMIT_NOFILE, &r) < 0)
			return 1;
	}
	int fd = open("/", O_RDONLY);
	if (fd < 0 || dup2(fd, 2048) < 0)
		return 1;
	r.rlim_cur = 1024;
	if (setrlimit(RLIMIT_NOFILE, &r) < 0)
		return 1;
	execv(argv[1], argv + 1);
	perror("exec launcher");
	return 1;
}
