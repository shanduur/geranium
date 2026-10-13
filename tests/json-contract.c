#include "../src/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	char *data = malloc(1024 * 1024 + 1);
	if (!data)
		return 2;
	size_t n = fread(data, 1, 1024 * 1024 + 1, stdin);
	if (ferror(stdin))
		return 2;
	yyjson_doc *doc = json_parse(data, n);
	if (argc == 2 && !strcmp(argv[1], "uint"))
		printf("%llu\n", json_uint(yyjson_doc_get_root(doc)));
	yyjson_doc_free(doc);
	free(data);
	return 0;
}
