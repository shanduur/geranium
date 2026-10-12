#ifndef GERANIUM_H
#define GERANIUM_H

#include <stdbool.h>

/* These calls retain CLI process lifetime: failures may exit, and operations
 * may fork or change process state. This is not a reentrant embedding SDK. */
#define GERANIUM_COMMAND_ARGS_MAX 64
#define GERANIUM_COMMAND_BYTES_MAX 8192

enum geranium_source {
	GERANIUM_SOURCE_ROOTFS,
	GERANIUM_SOURCE_IMAGE,
};

/* Opens and validates a state root. The caller owns the returned directory FD. */
int geranium_open_state_root(const char *root);

/* Count includes the executable; no trailing NULL is required. Zero selects
 * image defaults, but is invalid for rootfs. Explicit executables are absolute.
 * Argument bytes, including terminators, must fit GERANIUM_COMMAND_BYTES_MAX. */
int geranium_create(const char *root, enum geranium_source source_type,
		    const char *source, const char *name, int command_count,
		    const char *const command[]);
int geranium_run(const char *root, enum geranium_source source_type,
		 const char *source, const char *name, int command_count,
		 const char *const command[], bool detached);
int geranium_start(const char *root, const char *name, bool detached);
int geranium_stop(const char *root, const char *name);
int geranium_inspect(const char *root, const char *name);
int geranium_logs(const char *root, const char *name);
int geranium_rm(const char *root, const char *name);
int geranium_pull(const char *root, const char *reference, bool http_loopback);
int geranium_rmi(const char *root, const char *reference);

#endif
