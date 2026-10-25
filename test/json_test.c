/* Host-side unit test for src/json.c.
 *
 *   gcc -std=gnu99 -Wall -Wextra -Isrc -o /tmp/json_test \
 *       test/json_test.c src/json.c src/util.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

static int fails;

#define CHECK(name, cond) do { \
	int ok_ = (cond); \
	printf("%-32s %s\n", name, ok_ ? "PASS" : "FAIL"); \
	if (!ok_) fails++; \
} while (0)

int main(void)
{
	const char *src = "{\"a\":[1, 2, {\"b\":\"x\\ny\"}],\"c\":true,"
			  "\"d\":null,\"e\":\"中文\",\"f\":\"\\u4e2d\\u6587\"}";
	struct jval *r = json_parse(src);
	CHECK("parse object", r && r->t == J_OBJ && r->n == 5);

	struct jval *a = j_get(r, "a");
	CHECK("array member", a && a->t == J_ARR && a->n == 3);
	CHECK("number literal", a->items[0] && strcmp(j_str(a->items[0], ""), "1") == 0);
	struct jval *nested = a->items[2];
	CHECK("nested object", nested && nested->t == J_OBJ);
	struct jval *b = j_get(nested, "b");
	CHECK("escape unescapes", b && strcmp(j_str(b, ""), "x\ny") == 0);

	struct jval *c = j_get(r, "c");
	CHECK("bool literal", c && c->t == J_BOOL && strcmp(j_str(c, ""), "true") == 0);
	struct jval *d = j_get(r, "d");
	CHECK("null type", d && d->t == J_NULL);
	CHECK("utf8 passthrough", strcmp(j_str(j_get(r, "e"), ""), "中文") == 0);
	CHECK("\\u escape decodes", strcmp(j_str(j_get(r, "f"), ""), "中文") == 0);
	CHECK("missing key", j_get(r, "zzz") == NULL);

	char *ser = j_serialize(r);
	CHECK("serialize", ser != NULL);
	struct jval *r2 = json_parse(ser);
	struct jval *a2 = j_get(r2, "a");
	CHECK("roundtrip", r2 && a2 && a2->t == J_ARR &&
	      strcmp(j_str(j_get(r2, "e"), ""), "中文") == 0 &&
	      a2->n == 3 && strcmp(j_str(a2->items[0], ""), "1") == 0);
	j_free(r2);
	free(ser);
	j_free(r);

	CHECK("malformed rejected", json_parse("{\"a\": [1, 2") == NULL);
	CHECK("empty rejected", json_parse("") == NULL);
	CHECK("bare number", ({ struct jval *v = json_parse("42"); int ok = v && strcmp(v->str, "42") == 0; j_free(v); ok; }));

	printf("%s (%d failure(s))\n", fails ? "TESTS FAILED" : "all tests passed", fails);
	return fails ? 1 : 0;
}
