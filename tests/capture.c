#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long long milliseconds(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		exit(1);
	return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static pid_t spawn(char **arguments, int writer)
{
	pid_t child = fork();
	if (child < 0)
		exit(1);
	if (!child) {
		if (dup2(writer, 1) < 0 || dup2(writer, 2) < 0)
			_exit(125);
		if (writer > 2)
			close(writer);
		execv(arguments[0], arguments);
		_exit(125);
	}
	return child;
}

static int capture(char **arguments)
{
	int output[2];
	if (pipe(output) < 0)
		return 1;
	pid_t child = spawn(arguments, output[1]);
	close(output[1]);
	long long deadline = milliseconds() + 6000;
	int ended = 0;
	while (milliseconds() < deadline) {
		struct pollfd observed = { output[0], POLLIN | POLLHUP, 0 };
		int result = poll(&observed, 1, 100);
		if (result < 0 && errno == EINTR)
			continue;
		if (result < 0)
			break;
		if (observed.revents & (POLLIN | POLLHUP)) {
			char buffer[4096];
			ssize_t size = read(output[0], buffer, sizeof buffer);
			if (!size) {
				ended = 1;
				break;
			}
			if (size < 0 && errno != EINTR)
				break;
		}
	}
	close(output[0]);
	if (!ended) {
		fprintf(stderr,
			"captured CLI did not close output within 6 seconds\n");
		kill(child, SIGKILL);
	}
	int status;
	while (waitpid(child, &status, 0) < 0) {
		if (errno != EINTR)
			return 1;
	}
	return ended && WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : 1;
}

static int blocked_logs(const char *directory, char *root)
{
	char log_program[4096], inspect_program[4096], stop_program[4096],
		log_path[4096];
	if (snprintf(log_program, sizeof log_program, "%s/gu-logs",
		     directory) >= (int)sizeof log_program ||
	    snprintf(inspect_program, sizeof inspect_program, "%s/gu-inspect",
		     directory) >= (int)sizeof inspect_program ||
	    snprintf(stop_program, sizeof stop_program, "%s/gu-stop",
		     directory) >= (int)sizeof stop_program ||
	    snprintf(log_path, sizeof log_path, "%s/demo/log", root) >=
		    (int)sizeof log_path)
		return 1;
	long long deadline = milliseconds() + 6000;
	struct stat info;
	while (stat(log_path, &info) < 0 || info.st_size < 262144) {
		if (milliseconds() >= deadline)
			return 1;
		struct timespec interval = { 0, 10000000 };
		nanosleep(&interval, NULL);
	}
	int output[2];
	if (pipe(output) < 0)
		return 1;
#ifdef F_SETPIPE_SZ
	if (fcntl(output[1], F_SETPIPE_SZ, 4096) < 0)
		return 1;
#endif
	char *logs[] = { log_program, "--root", root, "demo", NULL };
	pid_t reader = spawn(logs, output[1]);
	close(output[1]);
	struct pollfd observed = { output[0], POLLIN, 0 };
	if (poll(&observed, 1, 3000) <= 0) {
		kill(reader, SIGKILL);
		waitpid(reader, NULL, 0);
		close(output[0]);
		return 1;
	}
	struct timespec interval = { 0, 100000000 };
	nanosleep(&interval, NULL);
	char *inspect[] = { inspect_program, "--root", root, "demo", NULL };
	char *stop[] = { stop_program, "--root", root, "demo", NULL };
	int failed = capture(inspect) || capture(stop);
	kill(reader, SIGKILL);
	waitpid(reader, NULL, 0);
	close(output[0]);
	return failed;
}

int main(int argc, char **argv)
{
	if (argc >= 3 && !strcmp(argv[1], "--host-signals")) {
		sigset_t blocked;
		sigemptyset(&blocked);
		sigaddset(&blocked, SIGCHLD);
		sigaddset(&blocked, SIGTERM);
		if (signal(SIGCHLD, SIG_IGN) == SIG_ERR ||
		    sigprocmask(SIG_BLOCK, &blocked, NULL) < 0)
			return 1;
		execv(argv[2], argv + 2);
		return 1;
	}
	if (argc == 4 && !strcmp(argv[1], "--blocked-logs"))
		return blocked_logs(argv[2], argv[3]);
	if (argc < 2)
		return 1;
	return capture(argv + 1);
}
