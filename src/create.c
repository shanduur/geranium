#include "geranium.h"
#include "cli.h"
#include <getopt.h>

int main(int argc, char **argv)
{
	const char *arguments =
		"(--rootfs PATH NAME COMMAND [ARG ...] | --image REFERENCE NAME [COMMAND ARG ...])";
	static const struct option options[] = {
		{ "root", required_argument, NULL, 'r' },
		{ "help", no_argument, NULL, 'h' },
		{ "rootfs", required_argument, NULL, 'f' },
		{ "image", required_argument, NULL, 'i' },
		{ NULL, 0, NULL, 0 }
	};
	const char *root = NULL;
	const char *problem = NULL;
	char fallback[PATH_MAX];
	char diagnostic[256];
	enum geranium_source source_type = GERANIUM_SOURCE_ROOTFS;
	const char *source = NULL;
	int ch;

	cli_stdio();
	if (argc == 3 && !strcmp(argv[1], "--root") && cli_help(argv[2])) {
		cli_usage(stdout, "create", arguments);
		return 0;
	}
	if (argc == 1) {
		cli_usage(stderr, "create", arguments);
		return 1;
	}
	opterr = 0;
	optind = 1;
	while (1) {
		int before = optind, index = -1;
		ch = getopt_long(argc, argv, "+:h", options, &index);
		if (ch == -1)
			break;
		if (index >= 0 &&
		    (strcspn(argv[before] + 2, "=") !=
			     strlen(options[index].name) ||
		     strncmp(argv[before] + 2, options[index].name,
			     strlen(options[index].name)))) {
			(void)snprintf(diagnostic, sizeof diagnostic,
				       "unknown option %s", argv[before]);
			problem = diagnostic;
			break;
		}
		switch (ch) {
		case 'r':
			if (root) {
				problem = "--root specified more than once";
				break;
			}
			root = optarg;
			break;
		case 'h':
			if (optind < argc) {
				problem = "unexpected arguments after --help";
				break;
			}
			cli_usage(stdout, "create", arguments);
			return 0;
		case 'f':
		case 'i':
			if (source) {
				problem =
					"source specified more than once; choose one of --rootfs or --image";
				break;
			}
			source = optarg;
			source_type = ch == 'i' ? GERANIUM_SOURCE_IMAGE :
						  GERANIUM_SOURCE_ROOTFS;
			break;
		case ':':
			(void)snprintf(diagnostic, sizeof diagnostic,
				       "option %s requires a value",
				       argv[before]);
			problem = diagnostic;
			break;
		case '?':
			(void)snprintf(diagnostic, sizeof diagnostic,
				       "unknown option %s", argv[before]);
			problem = diagnostic;
			break;
		}
		if (problem)
			break;
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
	if (problem)
		cli_operation_fail(root, problem);
	if (optind >= argc) {
		if (source)
			cli_operation_fail(root, "missing arguments");
		cli_fail("missing arguments");
	}
	if (!source)
		cli_operation_fail(
			root,
			"source required: specify --rootfs PATH or --image REFERENCE");
	if (optind >= argc)
		cli_operation_fail(root, "missing container name");
	const char *name = argv[optind++];
	if (!cli_name(name))
		cli_operation_fail(root, "invalid container name");
	if (argc - optind > GERANIUM_COMMAND_ARGS_MAX)
		cli_operation_fail(root, "too many arguments");
	return geranium_create(root, source_type, source, name, argc - optind,
			       (const char *const *)(argv + optind));
}
