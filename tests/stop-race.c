#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
int __real_backend_signal(int handle, int signum);
int __wrap_backend_signal(int handle, int signum)
{
#ifdef KILL_RACE
	if (signum == SIGTERM)
		return 0;
	const int trigger = SIGKILL;
#else
	const int trigger = SIGTERM;
#endif
	if (signum == trigger) {
		if (__real_backend_signal(handle, SIGKILL) < 0)
			return -1;
		/* Reaping makes signal 0 report ESRCH, including on Linux 5.3,
         * whose pidfd poll reports readability but no hangup. */
		struct timespec start, now;
		if (clock_gettime(CLOCK_MONOTONIC, &start) < 0)
			return -1;
		for (;;) {
			if (__real_backend_signal(handle, 0) < 0) {
				if (errno == ESRCH)
					break;
				return -1;
			}
			if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
				return -1;
			if ((now.tv_sec - start.tv_sec) * 1000 +
				    (now.tv_nsec - start.tv_nsec) / 1000000 >=
			    3000) {
				errno = ETIMEDOUT;
				return -1;
			}
			if (poll(NULL, 0, 20) < 0 && errno != EINTR)
				return -1;
		}
	}
	int result = __real_backend_signal(handle, signum);
	if (signum == trigger && result < 0 && errno == ESRCH) {
#ifdef KILL_RACE
		const char marker[] = "wrapped KILL observed ESRCH\n";
#else
		const char marker[] = "wrapped TERM observed ESRCH\n";
#endif
		if (write(2, marker, sizeof marker - 1) !=
		    (ssize_t)(sizeof marker - 1))
			_exit(98);
		errno = ESRCH;
	}
	return result;
}
