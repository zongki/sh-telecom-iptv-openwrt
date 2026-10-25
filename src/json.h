#ifndef IPTV_JSON_H
#define IPTV_JSON_H

/* Minimal JSON DOM parser, enough for the epg7 AJAX responses (and the
 * 0.5 VOD endpoints later): objects, arrays, strings, numbers, bools.
 * Numbers/bools are kept as their literal text; everything lives in
 * malloc'd nodes freed with j_free().  Lenient by design: tolerates
 * trailing content, caps nesting depth. */

enum jtype { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

struct jval {
	enum jtype t;
	char *str;              /* J_STR (unescaped) / J_NUM / J_BOOL */
	struct jval **items;    /* J_ARR elements / J_OBJ values */
	char **keys;            /* J_OBJ only, parallel to items */
	int n;
	int cap;                /* allocated slots in items/keys */
};

struct jval *json_parse(const char *text);
void j_free(struct jval *v);

/* object member lookup, NULL if absent or v is not an object */
struct jval *j_get(struct jval *obj, const char *key);
/* string value of any scalar node (numbers/bools included) */
const char *j_str(struct jval *v, const char *deflt);

/* serialize a subtree back to compact JSON text (malloc'd, NULL on
 * OOM); scalars are re-emitted, strings re-escaped */
char *j_serialize(struct jval *v);

#endif
