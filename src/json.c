#include <stdlib.h>
#include <string.h>

#include "json.h"

#define J_DEPTH_MAX 24

struct jctx {
	const char *p;
	int depth;
};

static struct jval *j_value(struct jctx *c);

static void j_skip_ws(struct jctx *c)
{
	while (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')
		c->p++;
}

static struct jval *j_new(enum jtype t)
{
	struct jval *v = calloc(1, sizeof(*v));
	if (v)
		v->t = t;
	return v;
}

void j_free(struct jval *v)
{
	if (!v)
		return;
	for (int i = 0; i < v->n; i++) {
		j_free(v->items ? v->items[i] : NULL);
		if (v->keys)
			free(v->keys[i]);
	}
	free(v->items);
	free(v->keys);
	free(v->str);
	free(v);
}

static int utf8_emit(char **o, unsigned cp)
{
	char *out = *o;
	if (cp < 0x80) {
		*out++ = (char)cp;
	} else if (cp < 0x800) {
		*out++ = (char)(0xc0 | (cp >> 6));
		*out++ = (char)(0x80 | (cp & 0x3f));
	} else if (cp < 0x10000) {
		*out++ = (char)(0xe0 | (cp >> 12));
		*out++ = (char)(0x80 | ((cp >> 6) & 0x3f));
		*out++ = (char)(0x80 | (cp & 0x3f));
	} else {
		*out++ = (char)(0xf0 | (cp >> 18));
		*out++ = (char)(0x80 | ((cp >> 12) & 0x3f));
		*out++ = (char)(0x80 | ((cp >> 6) & 0x3f));
		*out++ = (char)(0x80 | (cp & 0x3f));
	}
	*o = out;
	return 0;
}

static int hex4(const char *p, unsigned *out)
{
	unsigned v = 0;
	for (int i = 0; i < 4; i++) {
		char ch = p[i];
		v <<= 4;
		if (ch >= '0' && ch <= '9')
			v |= (unsigned)(ch - '0');
		else if (ch >= 'a' && ch <= 'f')
			v |= (unsigned)(ch - 'a' + 10);
		else if (ch >= 'A' && ch <= 'F')
			v |= (unsigned)(ch - 'A' + 10);
		else
			return -1;
	}
	*out = v;
	return 0;
}

/* parse a JSON string at c->p (which must point at '"') */
static char *j_string_raw(struct jctx *c)
{
	c->p++;                             /* opening quote */
	/* upper bound: raw length; escapes only shrink */
	size_t raw = 0;
	while (c->p[raw] && c->p[raw] != '"') {
		if (c->p[raw] == '\\' && c->p[raw + 1])
			raw++;
		raw++;
	}
	char *out = malloc(raw + 1);
	if (!out)
		return NULL;
	char *o = out;
	while (*c->p && *c->p != '"') {
		if (*c->p != '\\') {
			*o++ = *c->p++;
			continue;
		}
		c->p++;
		switch (*c->p) {
		case '"':  *o++ = '"';  c->p++; break;
		case '\\': *o++ = '\\'; c->p++; break;
		case '/':  *o++ = '/';  c->p++; break;
		case 'b':  *o++ = '\b'; c->p++; break;
		case 'f':  *o++ = '\f'; c->p++; break;
		case 'n':  *o++ = '\n'; c->p++; break;
		case 'r':  *o++ = '\r'; c->p++; break;
		case 't':  *o++ = '\t'; c->p++; break;
		case 'u': {
			unsigned cp;
			if (hex4(c->p + 1, &cp) != 0) {
				free(out);
				return NULL;
			}
			c->p += 5;
			if (cp >= 0xd800 && cp <= 0xdbff && c->p[0] == '\\' &&
			    c->p[1] == 'u') {
				unsigned lo;
				if (hex4(c->p + 2, &lo) == 0 &&
				    lo >= 0xdc00 && lo <= 0xdfff) {
					cp = 0x10000 + ((cp - 0xd800) << 10) +
					     (lo - 0xdc00);
					c->p += 6;
				}
			}
			utf8_emit(&o, cp);
			break;
		}
		default:
			free(out);
			return NULL;
		}
	}
	if (*c->p != '"') {
		free(out);
		return NULL;
	}
	c->p++;
	*o = '\0';
	return out;
}

/* geometric growth: epg7 bodies carry arrays of hundreds of entries
 * (channelArray, VOD media lists) - a per-element realloc is O(n^2) */
static int j_grow(struct jval *v)
{
	if (v->n < v->cap)
		return 0;
	int ncap = v->cap ? v->cap * 2 : 8;
	struct jval **ni = realloc(v->items, sizeof(*ni) * (size_t)ncap);
	if (!ni)
		return -1;
	v->items = ni;
	if (v->t == J_OBJ) {
		char **nk = realloc(v->keys, sizeof(*nk) * (size_t)ncap);
		if (!nk)
			return -1;
		v->keys = nk;
	}
	v->cap = ncap;
	return 0;
}

static int j_push(struct jval *v, char *key, struct jval *child)
{
	if (j_grow(v) != 0)
		return -1;
	if (v->t == J_OBJ)
		v->keys[v->n] = key;
	v->items[v->n] = child;
	v->n++;
	return 0;
}

static struct jval *j_value(struct jctx *c)
{
	struct jval *v = NULL;
	j_skip_ws(c);
	if (c->depth > J_DEPTH_MAX)
		return NULL;
	if (*c->p == '{') {
		v = j_new(J_OBJ);
		if (!v)
			return NULL;
		c->p++;
		c->depth++;
		j_skip_ws(c);
		if (*c->p == '}') {
			c->p++;
			c->depth--;
			return v;
		}
		for (;;) {
			j_skip_ws(c);
			if (*c->p != '"')
				goto fail;
			char *key = j_string_raw(c);
			if (!key)
				goto fail;
			j_skip_ws(c);
			if (*c->p != ':') {
				free(key);
				goto fail;
			}
			c->p++;
			struct jval *child = j_value(c);
			if (!child || j_push(v, key, child) != 0) {
				free(key);
				j_free(child);
				goto fail;
			}
			j_skip_ws(c);
			if (*c->p == ',') {
				c->p++;
				continue;
			}
			if (*c->p == '}') {
				c->p++;
				c->depth--;
				return v;
			}
			goto fail;
		}
	}
	if (*c->p == '[') {
		v = j_new(J_ARR);
		if (!v)
			return NULL;
		c->p++;
		c->depth++;
		j_skip_ws(c);
		if (*c->p == ']') {
			c->p++;
			c->depth--;
			return v;
		}
		for (;;) {
			struct jval *child = j_value(c);
			if (!child || j_push(v, NULL, child) != 0) {
				j_free(child);
				goto fail;
			}
			j_skip_ws(c);
			if (*c->p == ',') {
				c->p++;
				continue;
			}
			if (*c->p == ']') {
				c->p++;
				c->depth--;
				return v;
			}
			goto fail;
		}
	}
	if (*c->p == '"') {
		char *s = j_string_raw(c);
		if (!s)
			return NULL;
		v = j_new(J_STR);
		if (!v) {
			free(s);
			return NULL;
		}
		v->str = s;
		return v;
	}
	/* number / bool / null: copy the literal */
	const char *start = c->p;
	if (!strncmp(c->p, "true", 4) || !strncmp(c->p, "null", 4))
		c->p += 4;
	else if (!strncmp(c->p, "false", 5))
		c->p += 5;
	else {
		if (*c->p == '-')
			c->p++;
		while ((*c->p >= '0' && *c->p <= '9') || *c->p == '.' ||
		       *c->p == 'e' || *c->p == 'E' || *c->p == '+' || *c->p == '-')
			c->p++;
	}
	if (c->p == start)
		return NULL;
	if (*start == 'n') {           /* null literal */
		v = j_new(J_NULL);
		return v;
	}
	v = j_new(J_NUM);
	if (!v)
		return NULL;
	v->str = malloc((size_t)(c->p - start) + 1);
	if (!v->str) {
		free(v);
		return NULL;
	}
	memcpy(v->str, start, (size_t)(c->p - start));
	v->str[c->p - start] = '\0';
	v->t = (*start == 't' || *start == 'f') ? J_BOOL : J_NUM;
	return v;

fail:
	j_free(v);
	return NULL;
}

struct jval *json_parse(const char *text)
{
	struct jctx c = { .p = text, .depth = 0 };
	struct jval *v = j_value(&c);
	if (!v)
		return NULL;
	return v;
}

struct jval *j_get(struct jval *obj, const char *key)
{
	if (!obj || obj->t != J_OBJ)
		return NULL;
	for (int i = 0; i < obj->n; i++)
		if (strcmp(obj->keys[i], key) == 0)
			return obj->items[i];
	return NULL;
}

const char *j_str(struct jval *v, const char *deflt)
{
	if (!v || !v->str)
		return deflt;
	return v->str;
}

/* -------- serializer -------- */

#include "util.h"

static int j_ser(struct sbuf *sb, struct jval *v)
{
	if (!v)
		return sb_printf(sb, "null");
	switch (v->t) {
	case J_NULL:
		return sb_printf(sb, "null");
	case J_BOOL:
	case J_NUM:
		return sb_printf(sb, "%s", v->str ? v->str : "null");
	case J_STR:
		return sb_json_str(sb, v->str ? v->str : "");
	case J_ARR:
		if (sb_printf(sb, "[") != 0)
			return -1;
		for (int i = 0; i < v->n; i++)
			if (j_ser(sb, v->items[i]) != 0 ||
			    (i + 1 < v->n && sb_printf(sb, ",") != 0))
				return -1;
		return sb_printf(sb, "]");
	case J_OBJ:
		if (sb_printf(sb, "{") != 0)
			return -1;
		for (int i = 0; i < v->n; i++)
			if (sb_json_str(sb, v->keys[i]) != 0 ||
			    sb_printf(sb, ":") != 0 ||
			    j_ser(sb, v->items[i]) != 0 ||
			    (i + 1 < v->n && sb_printf(sb, ",") != 0))
				return -1;
		return sb_printf(sb, "}");
	}
	return -1;
}

char *j_serialize(struct jval *v)
{
	struct sbuf sb;
	if (sb_init(&sb, 256) != 0)
		return NULL;
	if (j_ser(&sb, v) != 0) {
		sb_free(&sb);
		return NULL;
	}
	return sb.b;
}
