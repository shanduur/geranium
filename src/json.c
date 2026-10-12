#define _POSIX_C_SOURCE 200809L
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
struct parser {
	const unsigned char *p, *end;
	unsigned nodes;
};
static void error(void)
{
	fprintf(stderr, "geranium image: invalid or excessive JSON\n");
	exit(1);
}
static void space(struct parser *p)
{
	while (p->p < p->end && strchr(" \t\r\n", *p->p) && *p->p)
		p->p++;
}
static unsigned hex(struct parser *p)
{
	unsigned n = 0;
	for (int i = 0; i < 4; i++) {
		if (p->p == p->end)
			error();
		unsigned c = *p->p++;
		n *= 16;
		if (c >= '0' && c <= '9')
			n += c - '0';
		else if (c >= 'a' && c <= 'f')
			n += c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			n += c - 'A' + 10;
		else
			error();
	}
	return n;
}
static char *string(struct parser *p)
{
	size_t n = 0;
	if (p->p == p->end || *p->p++ != '"')
		error();
	const unsigned char *end = p->p;
	int escaped = 0;
	for (; end < p->end; end++) {
		if (!escaped && *end == '"')
			break;
		if (!escaped && *end == '\\')
			escaped = 1;
		else
			escaped = 0;
	}
	if (end == p->end)
		error();
	/* Raw string spans are disjoint and sum to at most the document budget. */
	size_t cap = (size_t)(end - p->p) + 1;
	char *data = malloc(cap);
	if (!data)
		error();
	while (p->p < p->end && *p->p != '"') {
		unsigned c = *p->p++;
		if (c < 32)
			error();
		if (c == '\\') {
			if (p->p == p->end)
				error();
			c = *p->p++;
			switch (c) {
			case '"':
			case '\\':
			case '/':
				break;
			case 'b':
				c = '\b';
				break;
			case 'f':
				c = '\f';
				break;
			case 'n':
				c = '\n';
				break;
			case 'r':
				c = '\r';
				break;
			case 't':
				c = '\t';
				break;
			case 'u':
				c = hex(p);
				if (c >= 0xd800 && c <= 0xdbff) {
					if (p->end - p->p < 2 ||
					    *p->p++ != '\\' || *p->p++ != 'u')
						error();
					unsigned lo = hex(p);
					if (lo < 0xdc00 || lo > 0xdfff)
						error();
					c = 0x10000 + ((c - 0xd800) << 10) +
					    (lo - 0xdc00);
				} else if (c >= 0xdc00 && c <= 0xdfff)
					error();
				if (!c)
					error();
				if (n + 4 > cap - 1)
					error();
				if (c >= 0x10000) {
					data[n++] = (char)(0xf0 | (c >> 18));
					data[n++] =
						(char)(0x80 | ((c >> 12) & 63));
					data[n++] =
						(char)(0x80 | ((c >> 6) & 63));
					c = 0x80 | (c & 63);
				} else if (c >= 0x800) {
					data[n++] = (char)(0xe0 | (c >> 12));
					data[n++] =
						(char)(0x80 | ((c >> 6) & 63));
					c = 0x80 | (c & 63);
				} else if (c >= 0x80) {
					data[n++] = (char)(0xc0 | (c >> 6));
					c = 0x80 | (c & 63);
				}
				break;
			default:
				error();
			}
		} else if (c >= 128) {
			unsigned count, min, v;
			if (c >= 0xc2 && c <= 0xdf) {
				count = 1;
				min = 128;
				v = c & 31;
			} else if (c >= 0xe0 && c <= 0xef) {
				count = 2;
				min = 2048;
				v = c & 15;
			} else if (c >= 0xf0 && c <= 0xf4) {
				count = 3;
				min = 65536;
				v = c & 7;
			} else {
				error();
				count = min = v = 0;
			}
			if (n + count + 1 > cap - 1)
				error();
			data[n++] = (char)c;
			for (unsigned j = 0; j < count; j++) {
				if (p->p == p->end)
					error();
				c = *p->p++;
				if ((c & 0xc0) != 0x80)
					error();
				v = (v << 6) | (c & 63);
				if (j + 1 < count)
					data[n++] = (char)c;
			}
			if (v < min || v > 0x10ffff ||
			    (v >= 0xd800 && v <= 0xdfff))
				error();
		}
		if (n >= cap - 1)
			error();
		data[n++] = (char)c;
	}
	if (p->p == p->end || *p->p++ != '"')
		error();
	data[n] = 0;
	return data;
}
static struct json *value(struct parser *p, unsigned depth)
{
	space(p);
	if (depth > 32 || ++p->nodes > 32768 || p->p == p->end)
		error();
	struct json *j = calloc(1, sizeof *j);
	if (!j)
		error();
	j->type = (char)*p->p;
	if (j->type == '"')
		j->text = string(p);
	else if (j->type == '{' || j->type == '[') {
		p->p++;
		space(p);
		unsigned char end = j->type == '{' ? '}' : ']';
		struct json **tail = &j->child;
		if (p->p < p->end && *p->p == end) {
			p->p++;
			return j;
		}
		for (;;) {
			if (j->type == '{') {
				char *key = string(p);
				for (struct json *q = j->child; q;
				     q = q->next->next)
					if (!strcmp(q->text, key))
						error();
				struct json *k = calloc(1, sizeof *k);
				if (!k)
					error();
				k->type = '"';
				k->text = key;
				*tail = k;
				tail = &k->next;
				space(p);
				if (p->p == p->end || *p->p++ != ':')
					error();
			}
			*tail = value(p, depth + 1);
			tail = &(*tail)->next;
			space(p);
			if (p->p == p->end)
				error();
			unsigned char c = *p->p++;
			if (c == end)
				break;
			if (c != ',')
				error();
			space(p);
		}
	} else {
		const unsigned char *begin = p->p;
		while (p->p < p->end && !strchr(" \t\r\n,}]", *p->p) && *p->p)
			p->p++;
		size_t n = (size_t)(p->p - begin);
		if (!n || n > 64)
			error();
		j->text = malloc(n + 1);
		if (!j->text)
			error();
		memcpy(j->text, begin, n);
		j->text[n] = 0;
		if (strcmp(j->text, "null") && strcmp(j->text, "true") &&
		    strcmp(j->text, "false")) {
			const char *s = j->text;
			if (*s == '-')
				s++;
			if (*s == '0')
				s++;
			else {
				if (*s < '1' || *s > '9')
					error();
				while (*s >= '0' && *s <= '9')
					s++;
			}
			if (*s == '.') {
				s++;
				if (*s < '0' || *s > '9')
					error();
				while (*s >= '0' && *s <= '9')
					s++;
			}
			if (*s == 'e' || *s == 'E') {
				s++;
				if (*s == '+' || *s == '-')
					s++;
				if (*s < '0' || *s > '9')
					error();
				while (*s >= '0' && *s <= '9')
					s++;
			}
			if (*s)
				error();
			j->type = '0';
		}
	}
	return j;
}
struct json *json_parse(const void *data, size_t n)
{
	if (n > 1024 * 1024)
		error();
	struct parser p = { (const unsigned char *)data,
			    (const unsigned char *)data + n, 0 };
	struct json *j = value(&p, 0);
	space(&p);
	if (p.p != p.end)
		error();
	return j;
}
struct json *json_get(struct json *j, const char *s)
{
	if (!j || j->type != '{')
		error();
	for (struct json *p = j->child; p; p = p->next->next)
		if (!strcmp(p->text, s))
			return p->next;
	return NULL;
}
const char *json_string(struct json *j)
{
	if (!j || j->type != '"')
		error();
	return j->text;
}
unsigned long long json_uint(struct json *j)
{
	if (!j || j->type != '0' || !j->text || !*j->text)
		error();
	for (const char *p = j->text; *p; p++)
		if (*p < '0' || *p > '9')
			error();
	errno = 0;
	unsigned long long v = strtoull(j->text, NULL, 10);
	if (errno)
		error();
	return v;
}
void json_free(struct json *j)
{
	if (j) {
		json_free(j->child);
		json_free(j->next);
		free(j->text);
		free(j);
	}
}
