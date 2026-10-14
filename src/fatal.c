#include "fatal.h"
#include <stdlib.h>

static void (*active_cleanup)(void);

void geranium_fatal_cleanup(void (*cleanup)(void))
{
	active_cleanup = cleanup;
}

void geranium_fatal_exit(int status)
{
	if (active_cleanup)
		active_cleanup();
	exit(status);
}
