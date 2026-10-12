#include "geranium.h"
#include "cli.h"
int main(int argc, char **argv)
{
	cli_stdio();
	const char *arguments =
		"[-d] (--rootfs PATH NAME COMMAND [ARG ...] | --image REFERENCE NAME [COMMAND ARG ...])";
	if (argc == 2 && cli_help(argv[1])) {
		cli_usage(stdout, "run", arguments);
		return 0;
	}
	if (argc < 2) {
		cli_usage(stderr, "run", arguments);
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
		cli_usage(stdout, "run", arguments);
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
	bool detached = false;
	if (!strcmp(argv[i], "-d")) {
		detached = true;
		i++;
	}
	if (i + 3 > argc ||
	    (strcmp(argv[i], "--rootfs") && strcmp(argv[i], "--image")))
		cli_operation_fail(
			root,
			"create|run [-d] (--rootfs PATH | --image REFERENCE) NAME [COMMAND ARG ...]");
	enum geranium_source source_type = !strcmp(argv[i], "--image") ?
						   GERANIUM_SOURCE_IMAGE :
						   GERANIUM_SOURCE_ROOTFS;
	const char *source = argv[++i], *name = argv[++i];
	if (!cli_name(name))
		cli_operation_fail(root, "invalid container name");
	i++;
	if (argc - i > GERANIUM_COMMAND_ARGS_MAX)
		cli_operation_fail(root, "too many arguments");
	return geranium_run(root, source_type, source, name, argc - i,
			    (const char *const *)(argv + i), detached);
}
