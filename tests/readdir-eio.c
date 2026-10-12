#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct dirent *__real_readdir(DIR *dir);
/* Fail only the selected directory, after one real non-dot entry. */
struct dirent *__wrap_readdir(DIR *dir)
{
	const char *path = getenv("GERANIUM_READDIR_EIO");
	struct stat target, actual;
	static int seen;
	if (path && stat(path, &target) == 0 &&
	    fstat(dirfd(dir), &actual) == 0 && target.st_dev == actual.st_dev &&
	    target.st_ino == actual.st_ino) {
		if (seen) {
			errno = EIO;
			return NULL;
		}
		struct dirent *ent = __real_readdir(dir);
		if (ent && strcmp(ent->d_name, ".") &&
		    strcmp(ent->d_name, ".."))
			seen = 1;
		return ent;
	}
	return __real_readdir(dir);
}
