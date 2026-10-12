#define _POSIX_C_SOURCE 200809L
#include "../src/backend.h"
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
	int identity[2], ready[2];
	if (pipe(identity) || pipe(ready))
		return 1;
	pid_t caller = fork();
	if (caller < 0)
		return 1;
	if (!caller) {
		close(identity[0]);
		close(ready[0]);
		int owner = (int)getpid();
		pid_t launcher = fork();
		if (launcher < 0)
			_exit(1);
		if (!launcher) {
			close(identity[1]);
			if (backend_watch_parent(owner))
				_exit(1);
			if (write(ready[1], "r", 1) != 1)
				_exit(1);
			close(ready[1]);
			for (;;)
				pause();
		}
		close(ready[1]);
		if (write(identity[1], &launcher, sizeof launcher) !=
		    sizeof launcher)
			_exit(1);
		close(identity[1]);
		for (;;)
			pause();
	}
	close(identity[1]);
	close(ready[1]);
	pid_t launcher;
	if (read(identity[0], &launcher, sizeof launcher) != sizeof launcher) {
		kill(caller, SIGKILL);
		waitpid(caller, NULL, 0);
		return 1;
	}
	close(identity[0]);
	struct pollfd readiness = { ready[0], POLLIN, 0 };
	char byte;
	if (poll(&readiness, 1, 3000) <= 0 || read(ready[0], &byte, 1) != 1 ||
	    byte != 'r') {
		kill(caller, SIGKILL);
		waitpid(caller, NULL, 0);
		return 1;
	}
	close(ready[0]);
	int handle = backend_pidfd_open((int)launcher);
	kill(caller, SIGKILL);
	waitpid(caller, NULL, 0);
	if (handle < 0)
		return 1;
	struct pollfd death = { handle, POLLIN, 0 };
	int dead = poll(&death, 1, 3000) > 0 && (death.revents & POLLIN);
	if (!dead) {
		fprintf(stderr, "bootstrap process survived its caller\n");
		backend_signal(handle, SIGKILL);
	}
	close(handle);
	return dead ? 0 : 1;
}
