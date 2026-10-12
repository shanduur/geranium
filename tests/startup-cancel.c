#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long long now(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t))
		exit(1);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int owner(int fd)
{
	struct flock l = { 0 };
	l.l_type = F_WRLCK;
	l.l_whence = SEEK_SET;
	if (fcntl(fd, F_GETLK, &l))
		exit(1);
	return l.l_type == F_UNLCK ? 0 : (int)l.l_pid;
}
int main(int argc, char **argv)
{
	if (argc != 5)
		return 1;
	int detached = strcmp(argv[4], "foreground") != 0;
	int timeout = !strcmp(argv[4], "timeout");
	int marker[2];
	if (pipe(marker))
		return 1;
	long long start = now();
	pid_t client = fork();
	if (client < 0)
		return 1;
	if (!client) {
		close(marker[0]);
		if (dup2(marker[1], 2) < 0)
			_exit(1);
		close(marker[1]);
		if (detached)
			execl(argv[1], argv[1], "--root", argv[2], "-d",
			      "--rootfs", argv[3], argv[4], "/bin/busybox",
			      "sleep", "30", (char *)0);
		else
			execl(argv[1], argv[1], "--root", argv[2], "--rootfs",
			      argv[3], argv[4], "/bin/busybox", "sleep", "30",
			      (char *)0);
		_exit(1);
	}
	close(marker[1]);
	char buf[256] = { 0 };
	char path[4096];
	snprintf(path, sizeof path, "%s/%s/log", argv[2], argv[4]);
	int marked = 0;
	while (now() - start < 3000) {
		FILE *f = fopen(path, "r");
		if (f) {
			if (fgets(buf, sizeof buf, f) &&
			    strstr(buf, "PREEXEC_DELAY"))
				marked = 1;
			fclose(f);
		}
		if (marked)
			break;
		poll(NULL, 0, 20);
	}
	snprintf(path, sizeof path, "%s/%s/runtime", argv[2], argv[4]);
	int lock = open(path, O_RDWR);
	if (lock < 0)
		return 1;
	int init = owner(lock);
	int handle = (int)syscall(SYS_pidfd_open, init, 0);
	if (!init || handle < 0)
		return 1;
	printf("%s: pre-exec marker, runtime owner %d\n", argv[4], init);
	fflush(stdout);
	if (!timeout)
		kill(client,
		     SIGKILL); /* Unreaped direct child: identity safe. */
	int status = 0;
	long long limit = now() + (timeout ? 11500 : 3000);
	int reaped = 0;
	struct pollfd event = { .fd = handle, .events = POLLIN };
	while (now() < limit) {
		if (!reaped && waitpid(client, &status, WNOHANG) == client)
			reaped = 1;
		if (!owner(lock) && poll(&event, 1, 0) > 0 && reaped)
			break;
		poll(NULL, 0, 20);
	}
	int ok = marked && !owner(lock) && poll(&event, 1, 0) > 0 && reaped;
	if (timeout)
		ok = ok && WIFEXITED(status) && WEXITSTATUS(status) == 125 &&
		     now() - start < 12000;
	if (!ok) {
		fprintf(stderr,
			"%s: FAIL runtime owner=%d, init alive=%d, elapsed=%lldms\n",
			argv[4], owner(lock), poll(&event, 1, 0) == 0,
			now() - start);
		/* Only this fixture's pinned init and unreaped client are cleaned up. */
		syscall(SYS_pidfd_send_signal, handle, SIGKILL, NULL, 0);
		if (!reaped) {
			kill(client, SIGKILL);
			waitpid(client, NULL, 0);
		}
		close(handle);
		close(lock);
		close(marker[0]);
		return 1;
	}
	snprintf(path, sizeof path, "%s/%s/status", argv[2], argv[4]);
	FILE *f = fopen(path, "r");
	if (!f || !fgets(buf, sizeof buf, f))
		return 1;
	fclose(f);
	if (strcmp(buf, "exited\n"))
		return 1;
	snprintf(path, sizeof path, "%s/%s/exitcode", argv[2], argv[4]);
	f = fopen(path, "r");
	if (!f || !fgets(buf, sizeof buf, f))
		return 1;
	fclose(f);
	if (strcmp(buf, "137\n"))
		return 1;
	printf("%s: init gone, runtime unlocked, completed failure, elapsed=%lldms: PASS\n",
	       argv[4], now() - start);
	close(handle);
	close(lock);
	close(marker[0]);
	return 0;
}
