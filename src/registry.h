#ifndef GERANIUM_REGISTRY_H
#define GERANIUM_REGISTRY_H
#include <stddef.h>
struct response {
	unsigned char *data;
	size_t size;
	int status;
	char location[4096], challenge[8192], type[256], digest[128];
};
struct response registry_get(const char *url, const char *token, int local,
			     size_t limit);
char *registry_token(const char *challenge, int local);
#endif
