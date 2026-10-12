#include <unistd.h>
/* Link-time instrumentation: fail on the 129th close in any manager process.
 * No syscall result is changed within the budget. The fixture inherits few FDs. */
int __real_close(int fd);
int __wrap_close(int fd)
{
	static unsigned calls;
	if (++calls > 128) {
		const char message[] = "close work exceeded 128 calls\n";
		ssize_t written = write(2, message, sizeof message - 1);
		(void)written;
		_exit(93);
	}
	return __real_close(fd);
}
