#include <geranium.h>
#include <string.h>
#include <unistd.h>

/* Each invocation has CLI lifetime; errors may terminate the process. */
int main(int argc, char **argv)
{
	if (argc < 3)
		return 99;
	const char *root = argv[2];
	const char *command[] = { "/bin/true" };
	if (!strcmp(argv[1], "prepare-root")) {
		int fd = geranium_open_state_root(root);
		return close(fd) < 0;
	}
	if (!strcmp(argv[1], "null-prepare-root"))
		return geranium_open_state_root(NULL);
	if (!strcmp(argv[1], "create") && argc == 4)
		return geranium_create(root, GERANIUM_SOURCE_ROOTFS, argv[3],
				       "consumer", 1, command);
	if (!strcmp(argv[1], "inspect"))
		return geranium_inspect(root, "consumer");
	if (!strcmp(argv[1], "rm"))
		return geranium_rm(root, "consumer");
	if (!strcmp(argv[1], "bad-enum"))
		return geranium_create(root, (enum geranium_source)42, root,
				       "consumer", 1, command);
	if (!strcmp(argv[1], "null-root"))
		return geranium_inspect(NULL, "consumer");
	if (!strcmp(argv[1], "null-name"))
		return geranium_inspect(root, NULL);
	if (!strcmp(argv[1], "bad-name"))
		return geranium_inspect(root, "../unsafe");
	if (!strcmp(argv[1], "negative-count"))
		return geranium_create(root, GERANIUM_SOURCE_ROOTFS, root,
				       "consumer", -1, command);
	if (!strcmp(argv[1], "oversize-count"))
		return geranium_create(root, GERANIUM_SOURCE_ROOTFS, root,
				       "consumer", 65, command);
	if (!strcmp(argv[1], "null-vector"))
		return geranium_create(root, GERANIUM_SOURCE_ROOTFS, root,
				       "consumer", 1, NULL);
	if (!strcmp(argv[1], "null-element")) {
		const char *null_command[] = { NULL };
		return geranium_create(root, GERANIUM_SOURCE_ROOTFS, root,
				       "consumer", 1, null_command);
	}
	if (!strcmp(argv[1], "null-source"))
		return geranium_create(root, GERANIUM_SOURCE_ROOTFS, NULL,
				       "consumer", 1, command);
	if (!strcmp(argv[1], "null-reference"))
		return geranium_pull(root, NULL, false);
	return 99;
}
