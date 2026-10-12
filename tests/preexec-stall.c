#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#include <time.h>
int __real_setpgid(pid_t, pid_t);
int __wrap_setpgid(pid_t p, pid_t g)
{
	const char marker[] = "PREEXEC_DELAY\n";
	ssize_t n = write(2, marker, sizeof marker - 1);
	(void)n;
#ifdef PREEXEC_DELAY_MS
	struct timespec delay = { PREEXEC_DELAY_MS / 1000,
				  (PREEXEC_DELAY_MS % 1000) * 1000000L };
	while (nanosleep(&delay, &delay) < 0) {
	}
#else
	for (;;)
		pause();
#endif
	return __real_setpgid(p, g);
}
