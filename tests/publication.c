#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdarg.h>
static int test_fcntl(int fd, int command, ...);
#define fcntl test_fcntl
#include "../src/geranium.c"
#undef fcntl

static int publish_during_observation;
static int observed_container;

static int test_fcntl(int fd, int command, ...)
{
	va_list arguments;
	va_start(arguments, command);
	int result;
	if (command == F_GETLK || command == F_SETLK || command == F_SETLKW) {
		struct flock *lock = va_arg(arguments, struct flock *);
		if (command == F_GETLK && publish_during_observation) {
			publish_during_observation = 0;
			code_write(observed_container, 42);
			status_write(observed_container, "exited\n");
		}
		result = fcntl(fd, command, lock);
	} else if (command == F_SETFD || command == F_SETFL) {
		result = fcntl(fd, command, va_arg(arguments, int));
	} else {
		result = fcntl(fd, command);
	}
	va_end(arguments);
	return result;
}

int main(void)
{
	char path[] = "/tmp/geranium-publication-XXXXXX";
	if (!mkdtemp(path))
		return 1;
	int container = open(path, O_RDONLY | O_DIRECTORY);
	if (container < 0)
		return 1;
	status_write(container, "running\n");
	observed_container = container;
	publish_during_observation = 1;
	char status[32];
	refresh(container, status, sizeof status);
	struct stat result;
	int success = !strcmp(status, "exited\n") &&
		      fstatat(container, "exitcode", &result,
			      AT_SYMLINK_NOFOLLOW) == 0 &&
		      code_read(container) == 42;
	remove_tree(container);
	close(container);
	rmdir(path);
	if (!success)
		fprintf(stderr,
			"refresh erased a concurrently published exit result\n");
	return success ? 0 : 1;
}
