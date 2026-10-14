#define _POSIX_C_SOURCE 200809L
#include "../src/geranium.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *marker, *gate;
static int blocking;

static void callback(void)
{
	char byte;
	/* Simulate an embedding caller closing every descriptor it owns,
	 * including the image lock, before entering a blocking callback. */
	for (int fd = 3; fd < 1024; fd++)
		close(fd);
	int fd = open(marker, O_WRONLY);
	if (fd < 0 || write(fd, blocking ? "b" : "a", 1) != 1)
		_exit(25);
	close(fd);
	if (!blocking)
		return;
	fd = open(gate, O_RDONLY);
	if (fd < 0 || read(fd, &byte, 1) != 1)
		_exit(26);
	close(fd);
}

static void first_callback(void)
{
	int saved = blocking;
	blocking = 0;
	callback();
	blocking = saved;
}

int main(int argc, char **argv)
{
	if (argc != 7)
		return 2;
	marker = argv[5];
	gate = argv[6];
	if (geranium_pull(argv[1], argv[2], 1))
		return 3;
	if (atexit(first_callback))
		return 4;
	if (geranium_pull(argv[1], argv[3], 1))
		return 5;
	blocking = 1;
	if (atexit(callback))
		return 6;
	if (!strcmp(argv[4], "success"))
		return 0;
	return geranium_pull(argv[1], argv[4], 1);
}
