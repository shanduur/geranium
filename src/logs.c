#include "geranium.h"
#include "cli.h"
int main(int argc, char **argv)
{
	cli_stdio();
	const char *arguments = "NAME";
	if (argc == 2 && cli_help(argv[1])) {
		cli_usage(stdout, "logs", arguments);
		return 0;
	}
	if (argc < 2) {
		cli_usage(stderr, "logs", arguments);
		return 1;
	}
	const char *root;
	char fallback[PATH_MAX];
	int i = 1;
	root = NULL;
	if (!strcmp(argv[i], "--root")) {
		if (++i >= argc)
			cli_fail("--root requires path");
		root = argv[i++];
	}
	if (i + 1 == argc && cli_help(argv[i]) && !strcmp(argv[1], "--root")) {
		cli_usage(stdout, "logs", arguments);
		return 0;
	}
	if (!root) {
		const char *home = getenv("HOME");
		if (!home)
			cli_fail("HOME missing; specify --root");
		if (snprintf(fallback, sizeof fallback,
			     "%s/.local/state/geranium",
			     home) >= (int)sizeof fallback)
			cli_fail("HOME too long");
		root = fallback;
	}
	if (i >= argc)
		cli_fail("missing arguments");
	if (i + 1 != argc || !cli_name(argv[i]))
		cli_operation_fail(root, "logs NAME");
	return geranium_logs(root, argv[i]);
}
