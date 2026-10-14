#define _GNU_SOURCE
#include "../src/geranium.h"
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

static pid_t owner, other;
static volatile sig_atomic_t notifications, reaped;
static int wake;
static const char *trace, *gate;
static void handler(int sig)
{
	int saved = errno;
	if (sig == SIGUSR1) {
		if (write(wake, "x", 1) != 1)
			_exit(25);
	} else if (sig == SIGCHLD) {
		int status;
		notifications++;
		if (other > 0 && waitpid(other, &status, WNOHANG) == other)
			reaped = WIFEXITED(status) && WEXITSTATUS(status) == 23;
	}
	errno = saved;
}
static void callback(void)
{
	char line[64];
	int n = snprintf(line, sizeof line, "%ld\n", (long)getpid());
	int fd = open(trace, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (fd >= 0) {
		if (write(fd, line, (size_t)n) != n)
			_exit(26);
		close(fd);
	}
	if (getpid() != owner && gate) {
		fd = open(gate, O_RDONLY);
		if (fd >= 0) {
			if (read(fd, line, 1) < 0)
				_exit(27);
			close(fd);
		}
	}
}
#ifdef __GLIBC__
static void exit_callback(int status, void *arg)
{
	(void)status;
	(void)arg;
	callback();
}
#endif
int main(int argc, char **argv)
{
	if (argc != 5)
		return 1;
	owner = getpid();
	const char *mode = argv[4];
	int masked_signal = !strcmp(mode, "masked-int")	 ? SIGINT :
			    !strcmp(mode, "masked-term") ? SIGTERM :
							   0;
	if (masked_signal) {
		sigset_t mask, pending;
		sigemptyset(&mask);
		sigaddset(&mask, masked_signal);
		if (sigprocmask(SIG_BLOCK, &mask, NULL) || raise(masked_signal))
			return 3;
		if (geranium_pull(argv[1], argv[2], true))
			return 4;
		if (sigpending(&pending) ||
		    !sigismember(&pending, masked_signal))
			return 17;
		if (sigprocmask(SIG_BLOCK, NULL, &mask) ||
		    !sigismember(&mask, masked_signal))
			return 18;
		return 0;
	}
	if (!strncmp(mode, "callback", 8)) {
		static char trace_path[4096], gate_path[4096];
		if (snprintf(trace_path, sizeof trace_path, "%s.trace",
			     argv[3]) >= (int)sizeof trace_path ||
		    snprintf(gate_path, sizeof gate_path, "%s.gate", argv[3]) >=
			    (int)sizeof gate_path)
			return 2;
		trace = trace_path;
		gate = !strcmp(mode, "callback-block") ? gate_path : NULL;
		if (atexit(callback))
			return 3;
#ifdef __GLIBC__
		if (on_exit(exit_callback, NULL))
			return 3;
#endif
		return geranium_pull(argv[1], argv[2], 1);
	}
	int ignore = !strcmp(mode, "ignore");
	int nocldwait = !strcmp(mode, "nocldwait");
	int pending = !strcmp(mode, "pending");
	int during = !strcmp(mode, "during");
	int ignored_signal = !strcmp(mode, "ignore-int")  ? SIGINT :
			     !strcmp(mode, "ignore-term") ? SIGTERM :
							    0;
	int sentinel = open(argv[3], O_RDONLY);
	if (sentinel < 0)
		return 2;
	struct sigaction action;
	memset(&action, 0, sizeof action);
	sigemptyset(&action.sa_mask);
	action.sa_handler = ignore ? SIG_IGN : handler;
	action.sa_flags = SA_RESTART |
			  ((ignore || nocldwait) ? SA_NOCLDWAIT : 0);
	if (sigaction(SIGCHLD, &action, NULL))
		return 3;
	action.sa_handler = ignored_signal == SIGTERM ? SIG_IGN : handler;
	action.sa_flags = SA_RESTART;
	if (sigaction(SIGTERM, &action, NULL))
		return 3;
	action.sa_handler = SIG_IGN;
	if (sigaction(SIGINT, &action, NULL))
		return 4;
	action.sa_handler = handler;
	if (sigaction(SIGUSR1, &action, NULL))
		return 4;
	sigset_t mask;
	sigemptyset(&mask);
	if (!ignored_signal)
		sigaddset(&mask, SIGTERM);
	if (!during)
		sigaddset(&mask, SIGCHLD);
	if (sigprocmask(SIG_SETMASK, &mask, NULL))
		return 5;
	if (!ignored_signal) {
		int ready[2];
		if (pipe(ready))
			return 6;
		wake = ready[1];
		other = fork();
		if (other < 0)
			return 6;
		if (!other) {
			close(ready[1]);
			char byte;
			if ((during || ignore || nocldwait) &&
			    read(ready[0], &byte, 1) != 1)
				_exit(24);
			_exit(23);
		}
		close(ready[0]);
		if (pending) {
			siginfo_t info;
			if (waitid(P_PID, (id_t)other, &info,
				   WEXITED | WNOWAIT) ||
			    sigpending(&mask) || !sigismember(&mask, SIGCHLD))
				return 13;
		}
		printf("other %ld\n", (long)other);
		fflush(stdout);
	}
	if (geranium_pull(argv[1], argv[2], 1))
		return 7;
	if (sigaction(SIGCHLD, NULL, &action) ||
	    action.sa_handler != (ignore ? SIG_IGN : handler) ||
	    !(action.sa_flags & SA_RESTART) ||
	    ((ignore || nocldwait) && !(action.sa_flags & SA_NOCLDWAIT)))
		return 8;
	if (sigaction(SIGTERM, NULL, &action) ||
	    action.sa_handler !=
		    (ignored_signal == SIGTERM ? SIG_IGN : handler))
		return 9;
	if (sigaction(SIGINT, NULL, &action) || action.sa_handler != SIG_IGN)
		return 10;
	if (sigprocmask(SIG_SETMASK, NULL, &mask) ||
	    sigismember(&mask, SIGTERM) != !ignored_signal ||
	    sigismember(&mask, SIGCHLD) != !during ||
	    sigismember(&mask, SIGINT))
		return 11;
	if (pending || during) {
		if (pending) {
			if (sigpending(&mask) || !sigismember(&mask, SIGCHLD))
				return 14;
			sigemptyset(&mask);
			sigaddset(&mask, SIGCHLD);
			if (sigprocmask(SIG_UNBLOCK, &mask, NULL))
				return 15;
		}
		if (!notifications || !reaped)
			return 16;
	} else if (ignore || nocldwait) {
		sigemptyset(&mask);
		sigaddset(&mask, SIGCHLD);
		if (sigprocmask(SIG_UNBLOCK, &mask, NULL))
			return 17;
		if (nocldwait && !notifications)
			return 18;
		if (waitpid(other, NULL, WNOHANG) != -1 || errno != ECHILD)
			return 19;
	} else if (other > 0) {
		int status;
		if (waitpid(other, &status, 0) != other || !WIFEXITED(status) ||
		    WEXITSTATUS(status) != 23)
			return 12;
	}
	if (other > 0)
		close(wake);
	close(sentinel);
	return 0;
}
