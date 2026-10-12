/* Exercise production requests/redirects on unprivileged fixture ports.
 * Only the socket destination is remapped; URL parsing, Host construction,
 * origin comparison and TLS verification run unchanged. */
#define _POSIX_C_SOURCE 200809L
#include <netdb.h>
static const char *fixture_port;
static int fixture_getaddrinfo(const char *, const char *,
			       const struct addrinfo *, struct addrinfo **);
#define getaddrinfo fixture_getaddrinfo
#include "../src/registry.c"
#undef getaddrinfo

static int fixture_getaddrinfo(const char *host, const char *port,
			       const struct addrinfo *hints,
			       struct addrinfo **result)
{
	printf("connection identity %s:%s\n", host, port);
	return getaddrinfo(host, fixture_port, hints, result);
}
int main(int argc, char **argv)
{
	if (argc != 3)
		return 2;
	fixture_port = argv[2];
	struct response r = registry_get(argv[1], "fixture-token", 1, 1024);
	int status = r.status;
	free(r.data);
	return status == 200 ? 0 : 1;
}
