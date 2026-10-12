#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	if (argc < 4)
		return 1;
	signal(SIGCHLD, SIG_DFL);
	pid_t child = fork();
	if (child < 0)
		return 1;
	if (!child) {
		if (setpgid(0, 0) < 0)
			_exit(122);
		execv(argv[2], argv + 2);
		_exit(123);
	}
	siginfo_t info;
	if (waitid(P_PID, (id_t)child, &info, WEXITED | WNOWAIT) < 0)
		return 1;
	if (info.si_code != CLD_EXITED || info.si_status != 0)
		return 1;
	/* An unreaped child pins this group ID while the test signals it. */
	(void)kill(-child, SIGTERM);
	struct timespec delay = { 1, 0 };
	nanosleep(&delay, NULL);
	int status;
	if (waitpid(child, &status, 0) != child)
		return 1;
	char path[4096], state[32];
	if (snprintf(path, sizeof path, "%s/demo/status", argv[1]) >=
	    (int)sizeof path)
		return 1;
	FILE *file = fopen(path, "r");
	if (!file)
		return 1;
	char *value = fgets(state, sizeof state, file);
	fclose(file);
	if (!value || strcmp(state, "running\n")) {
		fputs("detached container received caller group signal\n",
		      stderr);
		return 1;
	}
	return 0;
}
