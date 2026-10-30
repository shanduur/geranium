#include "../src/backend.h"
#include <stdio.h>

int main(void)
{
	const char *arch = backend_image_arch();

	puts(arch ? arch : "unknown");
	return 0;
}
