#include <errno.h>
/* Explicit denial injection, equivalent to rejected TRACEME. */
int __wrap_backend_exec_prepare(void)
{
	errno = EPERM;
	return -1;
}
