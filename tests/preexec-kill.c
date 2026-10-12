#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <unistd.h>
/* Explicit fault injection: kill only this payload, before exec. */
int __wrap_setpgid(pid_t pid, pid_t group)
{
	(void)pid;
	(void)group;
	kill(getpid(), SIGKILL);
	_exit(99);
}
