#include <errno.h>
/* Deterministic fault injection, not a claim about this kernel's permissions. */
int __wrap_chdir(const char *path)
{
	(void)path;
	errno = EACCES;
	return -1;
}
