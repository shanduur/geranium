/* Verify the hard body deadline while reads continue to make progress. */
#include "../src/registry.c"

int main(void)
{
	int fd[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd) < 0 ||
	    fcntl(fd[0], F_SETFL, O_NONBLOCK) < 0)
		return 2;
	char payload[1000];
	memset(payload, 'x', sizeof payload);
	if (write(fd[1], payload, sizeof payload) != (ssize_t)sizeof payload)
		return 2;
	struct connection c;
	memset(&c, 0, sizeof c);
	c.fd = fd[0];
	c.end = now() + 120;
	c.idle = now() + 10000;
	for (size_t i = 0; i < sizeof payload; i++) {
		char byte;
		if (transfer(&c, &byte, 1, 0) != 1)
			return 2;
		if (!i)
			puts("positive I/O progress");
		struct timespec delay = { 0, 5000000 };
		while (nanosleep(&delay, &delay) < 0)
			if (errno != EINTR)
				return 2;
	}
	close(fd[0]);
	close(fd[1]);
	return 0;
}
