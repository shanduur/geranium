#include "geranium.h"
#include "cli.h"
#include <getopt.h>

int main(int argc, char **argv)
{
	const char *arguments = "[-d] NAME";
	static const struct option options[] = {
		{ "root", required_argument, NULL, 'r' },
		{ "help", no_argument, NULL, 'h' },
		{ "detached", no_argument, NULL, 'd' },
		{ NULL, 0, NULL, 0 }
	};
	const char *root = NULL;
	const char *problem = NULL;
	char fallback[PATH_MAX];
	char diagnostic[256];
	bool detached = false;
	int ch;

	cli_stdio();

	if (argc == 3 && !strcmp(argv[1], "--root") && cli_help(argv[2])) {
		cli_usage(stdout, "start", arguments);
		return 0;
	}
	if (argc == 1) {
		cli_usage(stderr, "start", arguments);
		return 1;
	}

	opterr = 0;
	optind = 1;
	while (1) {
		int before = optind, index = -1;

		ch = getopt_long(argc, argv, "+:hd", options, &index);
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
			cli_usage(stdout, "start", arguments);
			return 0;
		case 'd':
			detached = true;
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
		if (detached)
			cli_operation_fail(root, "missing arguments");
		cli_fail("missing arguments");
	}
	if (optind + 1 != argc || !cli_name(argv[optind]))
		cli_operation_fail(root, "unexpected arguments; expected NAME");

	return geranium_start(root, argv[optind], detached);
}
