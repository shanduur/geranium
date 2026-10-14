#define _POSIX_C_SOURCE 200809L
#include "json.h"
#include "fatal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static void error(void)
{
	fprintf(stderr, "geranium image: invalid or excessive JSON\n");
	geranium_fatal_exit(1);
}

static void string_check(yyjson_val *value)
{
	if (memchr(yyjson_get_str(value), 0, yyjson_get_len(value)))
		error();
}

/* yyjson owns syntax/UTF-8 decoding; this walk enforces Geranium's bounds. */
static void validate(yyjson_val *value, unsigned depth, unsigned *nodes)
{
	if (depth > 32 || ++*nodes > 32768)
		error();
	if (yyjson_is_str(value))
		string_check(value);
	else if (yyjson_is_raw(value)) {
		if (yyjson_get_len(value) > 64)
			error();
	} else if (yyjson_is_arr(value)) {
		size_t index, count;
		yyjson_val *child;
		yyjson_arr_foreach(value, index, count, child)
			validate(child, depth + 1, nodes);
	} else if (yyjson_is_obj(value)) {
		size_t index, count;
		yyjson_val *key, *child;
		yyjson_obj_foreach(value, index, count, key, child)
		{
			string_check(key);
			/* Lookup returns the first decoded occurrence of this key. */
			if (yyjson_obj_getn(value, yyjson_get_str(key),
					    yyjson_get_len(key)) != child)
				error();
			/* Object keys do not consume the value budget. */
			validate(child, depth + 1, nodes);
		}
	}
}

yyjson_doc *json_parse(const void *data, size_t n)
{
	if (n > 1024 * 1024)
		error();
	/* Raw numbers preserve the original decimal lexeme without rounding. */
	yyjson_doc *doc = yyjson_read(data, n, YYJSON_READ_NUMBER_AS_RAW);
	if (!doc)
		error();
	unsigned nodes = 0;
	validate(yyjson_doc_get_root(doc), 0, &nodes);
	return doc;
}

yyjson_val *json_get(yyjson_val *value, const char *key)
{
	if (!yyjson_is_obj(value))
		error();
	return yyjson_obj_get(value, key);
}

const char *json_string(yyjson_val *value)
{
	if (!yyjson_is_str(value))
		error();
	return yyjson_get_str(value);
}

unsigned long long json_uint(yyjson_val *value)
{
	const char *raw = yyjson_get_raw(value);
	if (!raw || !*raw)
		error();
	for (const char *p = raw; *p; p++)
		if (*p < '0' || *p > '9')
			error();
	errno = 0;
	unsigned long long result = strtoull(raw, NULL, 10);
	if (errno)
		error();
	return result;
}
