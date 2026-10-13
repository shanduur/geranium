#ifndef GERANIUM_JSON_H
#define GERANIUM_JSON_H
#include <stddef.h>
#include <yyjson.h>

/* Values and strings are borrowed until their owning document is freed. */
yyjson_doc *json_parse(const void *, size_t);
yyjson_val *json_get(yyjson_val *, const char *);
const char *json_string(yyjson_val *);
unsigned long long json_uint(yyjson_val *);
#endif
