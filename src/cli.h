#ifndef GERANIUM_CLI_H
#define GERANIUM_CLI_H

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Only common CLI help, name-token and standard-descriptor plumbing lives here.
 * No helper symbol name overlaps the private library implementation. */
static void cli_fail(const char *message)
{
	fprintf(stderr, "geranium: %s\n", message);
	exit(1);
}
static void cli_stdio(void)
{
	for (int fd = 0; fd < 3; fd++) {
		if (fcntl(fd, F_GETFD) >= 0)
			continue;
		if (errno != EBADF) {
			perror("check standard descriptor");
			exit(1);
		}
		int null = open("/dev/null", O_RDWR);
		if (null < 0) {
			perror("open standard descriptor");
			exit(1);
		}
		if (null != fd) {
			if (dup2(null, fd) < 0) {
				perror("restore standard descriptor");
				exit(1);
			}
			close(null);
		}
	}
}
static inline int cli_name(const char *name)
{
	size_t n = strlen(name);
	return n && n <= 63 && name[0] != '-' &&
	       strspn(name,
		      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") ==
		       n;
}
/* Preserve state-root preparation before operation syntax failures. The
 * process exits below, just as the old CLI did with its state FD still open. */
static inline void cli_operation_fail(const char *root, const char *message)
{
	(void)geranium_open_state_root(root);
	cli_fail(message);
}

static int cli_help(const char *s)
{
	return !strcmp(s, "--help") || !strcmp(s, "-h");
}
static void cli_usage(FILE *out, const char *verb, const char *arguments)
{
	if (fprintf(out, "usage: gu-%s [--root PATH] %s\n", verb, arguments) <
		    0 ||
	    fflush(out) == EOF) {
		perror("write usage");
		exit(1);
	}
}
#endif
