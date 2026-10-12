#ifndef GERANIUM_JSON_H
#define GERANIUM_JSON_H
#include <stddef.h>
struct json {
	char type;
	char *text;
	struct json *child, *next;
};
struct json *json_parse(const void *, size_t);
struct json *json_get(struct json *, const char *);
const char *json_string(struct json *);
unsigned long long json_uint(struct json *);
void json_free(struct json *);
#endif
