#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "json.h"

struct json_kv {
	char *key;
	struct json_value *val;
};

struct json_value {
	enum json_type type;
	union {
		bool b;
		double num;
		char *str;
		struct { struct json_kv *items; size_t count; } obj;
		struct { struct json_value **items; size_t count; } arr;
	} u;
};

struct parser {
	char *p;
	char *errbuf;
	size_t errbuf_len;
	bool failed;
};

static void
fail(struct parser *ps, const char *msg)
{
	if (!ps->failed)
		snprintf(ps->errbuf, ps->errbuf_len, "%s (near: %.30s)", msg, ps->p);
	ps->failed = true;
}

static void
skip_ws(struct parser *ps)
{
	while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
		ps->p++;
}

static struct json_value *
new_value(enum json_type type)
{
	struct json_value *v = calloc(1, sizeof(*v));
	if (v != NULL)
		v->type = type;
	return v;
}

static struct json_value *parse_value(struct parser *ps);

/* In-place unescape of a JSON string body (between the quotes, ps->p
 * left just past the closing quote on return). Handles the escapes
 * this schema actually needs (\" \\ \/ \n \t) plus \uXXXX for the BMP
 * (no surrogate-pair handling - none of graph_config.c's fields need
 * anything outside the BMP, and this stays a scoped reader, not a
 * general one). */
static char *
parse_string_body(struct parser *ps)
{
	char *start = ps->p;
	char *out = ps->p; /* unescape shrinks in place, dst <= src always */

	while (*ps->p != '"') {
		if (*ps->p == '\0') {
			fail(ps, "unterminated string");
			return NULL;
		}
		if (*ps->p == '\\') {
			ps->p++;
			switch (*ps->p) {
			case '"': *out++ = '"'; break;
			case '\\': *out++ = '\\'; break;
			case '/': *out++ = '/'; break;
			case 'n': *out++ = '\n'; break;
			case 't': *out++ = '\t'; break;
			case 'r': *out++ = '\r'; break;
			case 'u': {
				unsigned int cp;
				if (sscanf(ps->p + 1, "%4x", &cp) != 1) {
					fail(ps, "bad \\u escape");
					return NULL;
				}
				/* ASCII-range only - good enough for this
				 * schema's field values (IPs, MACs, names). */
				*out++ = (cp < 0x80) ? (char)cp : '?';
				ps->p += 4;
				break;
			}
			default:
				fail(ps, "unknown escape sequence");
				return NULL;
			}
			ps->p++;
		} else {
			*out++ = *ps->p++;
		}
	}
	*out = '\0';
	ps->p++; /* past closing quote */
	return start;
}

static struct json_value *
parse_string(struct parser *ps)
{
	ps->p++; /* opening quote */
	char *s = parse_string_body(ps);
	if (ps->failed)
		return NULL;
	struct json_value *v = new_value(JSON_STRING);
	if (v == NULL) {
		fail(ps, "out of memory");
		return NULL;
	}
	v->u.str = s;
	return v;
}

static struct json_value *
parse_number(struct parser *ps)
{
	char *end;
	double n = strtod(ps->p, &end);
	if (end == ps->p) {
		fail(ps, "invalid number");
		return NULL;
	}
	ps->p = end;
	struct json_value *v = new_value(JSON_NUMBER);
	if (v == NULL) {
		fail(ps, "out of memory");
		return NULL;
	}
	v->u.num = n;
	return v;
}

static struct json_value *
parse_literal(struct parser *ps)
{
	if (strncmp(ps->p, "true", 4) == 0) {
		ps->p += 4;
		struct json_value *v = new_value(JSON_BOOL);
		if (v != NULL)
			v->u.b = true;
		return v;
	}
	if (strncmp(ps->p, "false", 5) == 0) {
		ps->p += 5;
		struct json_value *v = new_value(JSON_BOOL);
		if (v != NULL)
			v->u.b = false;
		return v;
	}
	if (strncmp(ps->p, "null", 4) == 0) {
		ps->p += 4;
		return new_value(JSON_NULL);
	}
	fail(ps, "unexpected token");
	return NULL;
}

static struct json_value *
parse_array(struct parser *ps)
{
	ps->p++; /* '[' */
	struct json_value *v = new_value(JSON_ARRAY);
	if (v == NULL) {
		fail(ps, "out of memory");
		return NULL;
	}

	size_t cap = 0;
	skip_ws(ps);
	if (*ps->p == ']') {
		ps->p++;
		return v;
	}

	for (;;) {
		skip_ws(ps);
		struct json_value *item = parse_value(ps);
		if (ps->failed)
			return NULL;

		if (v->u.arr.count == cap) {
			cap = cap ? cap * 2 : 4;
			struct json_value **grown =
				realloc(v->u.arr.items, cap * sizeof(*grown));
			if (grown == NULL) {
				fail(ps, "out of memory");
				return NULL;
			}
			v->u.arr.items = grown;
		}
		v->u.arr.items[v->u.arr.count++] = item;

		skip_ws(ps);
		if (*ps->p == ',') {
			ps->p++;
			continue;
		}
		if (*ps->p == ']') {
			ps->p++;
			return v;
		}
		fail(ps, "expected ',' or ']' in array");
		return NULL;
	}
}

static struct json_value *
parse_object(struct parser *ps)
{
	ps->p++; /* '{' */
	struct json_value *v = new_value(JSON_OBJECT);
	if (v == NULL) {
		fail(ps, "out of memory");
		return NULL;
	}

	size_t cap = 0;
	skip_ws(ps);
	if (*ps->p == '}') {
		ps->p++;
		return v;
	}

	for (;;) {
		skip_ws(ps);
		if (*ps->p != '"') {
			fail(ps, "expected string key in object");
			return NULL;
		}
		ps->p++;
		char *key = parse_string_body(ps);
		if (ps->failed)
			return NULL;

		skip_ws(ps);
		if (*ps->p != ':') {
			fail(ps, "expected ':' after object key");
			return NULL;
		}
		ps->p++;
		skip_ws(ps);

		struct json_value *val = parse_value(ps);
		if (ps->failed)
			return NULL;

		if (v->u.obj.count == cap) {
			cap = cap ? cap * 2 : 4;
			struct json_kv *grown = realloc(v->u.obj.items, cap * sizeof(*grown));
			if (grown == NULL) {
				fail(ps, "out of memory");
				return NULL;
			}
			v->u.obj.items = grown;
		}
		v->u.obj.items[v->u.obj.count].key = key;
		v->u.obj.items[v->u.obj.count].val = val;
		v->u.obj.count++;

		skip_ws(ps);
		if (*ps->p == ',') {
			ps->p++;
			continue;
		}
		if (*ps->p == '}') {
			ps->p++;
			return v;
		}
		fail(ps, "expected ',' or '}' in object");
		return NULL;
	}
}

static struct json_value *
parse_value(struct parser *ps)
{
	skip_ws(ps);
	switch (*ps->p) {
	case '{': return parse_object(ps);
	case '[': return parse_array(ps);
	case '"': return parse_string(ps);
	case 't': case 'f': case 'n': return parse_literal(ps);
	default:
		if (*ps->p == '-' || isdigit((unsigned char)*ps->p))
			return parse_number(ps);
		fail(ps, "unexpected character");
		return NULL;
	}
}

struct json_value *
json_parse(char *text, char *errbuf, size_t errbuf_len)
{
	struct parser ps = { .p = text, .errbuf = errbuf, .errbuf_len = errbuf_len };
	if (errbuf_len > 0)
		errbuf[0] = '\0';

	struct json_value *v = parse_value(&ps);
	if (ps.failed) {
		json_free(v);
		return NULL;
	}
	skip_ws(&ps);
	if (*ps.p != '\0') {
		fail(&ps, "trailing content after top-level value");
		json_free(v);
		return NULL;
	}
	return v;
}

void
json_free(struct json_value *v)
{
	if (v == NULL)
		return;
	switch (v->type) {
	case JSON_OBJECT:
		for (size_t i = 0; i < v->u.obj.count; i++)
			json_free(v->u.obj.items[i].val);
		free(v->u.obj.items);
		break;
	case JSON_ARRAY:
		for (size_t i = 0; i < v->u.arr.count; i++)
			json_free(v->u.arr.items[i]);
		free(v->u.arr.items);
		break;
	default:
		break;
	}
	free(v);
}

enum json_type
json_get_type(const struct json_value *v)
{
	return v == NULL ? JSON_NULL : v->type;
}

const struct json_value *
json_object_get(const struct json_value *v, const char *key)
{
	if (v == NULL || v->type != JSON_OBJECT)
		return NULL;
	for (size_t i = 0; i < v->u.obj.count; i++)
		if (strcmp(v->u.obj.items[i].key, key) == 0)
			return v->u.obj.items[i].val;
	return NULL;
}

size_t
json_array_size(const struct json_value *v)
{
	return (v != NULL && v->type == JSON_ARRAY) ? v->u.arr.count : 0;
}

const struct json_value *
json_array_get(const struct json_value *v, size_t idx)
{
	if (v == NULL || v->type != JSON_ARRAY || idx >= v->u.arr.count)
		return NULL;
	return v->u.arr.items[idx];
}

bool
json_as_bool(const struct json_value *v, bool default_val)
{
	return (v != NULL && v->type == JSON_BOOL) ? v->u.b : default_val;
}

double
json_as_number(const struct json_value *v, double default_val)
{
	return (v != NULL && v->type == JSON_NUMBER) ? v->u.num : default_val;
}

const char *
json_as_string(const struct json_value *v, const char *default_val)
{
	return (v != NULL && v->type == JSON_STRING) ? v->u.str : default_val;
}
