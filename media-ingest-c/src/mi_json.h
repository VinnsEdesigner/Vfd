/* mi_json.h - minimal JSON writer (builder) and reader (DOM parser).
 *
 * Enough JSON for an HTTP API: no streaming, no comments, but correct escaping,
 * UTF-8 passthrough, and both serialisation and parsing.
 */
#ifndef MI_JSON_H
#define MI_JSON_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Writer                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} mi_json_buf;

void mi_json_init(mi_json_buf *b);
void mi_json_free(mi_json_buf *b);
const char *mi_json_data(const mi_json_buf *b); /* never NULL once inited */

void mi_json_raw(mi_json_buf *b, const char *text);
/*
 * Append raw bytes, which may include NUL and anything else.
 *
 * `mi_json_raw` is strlen-based and so cannot carry a binary payload; this is for
 * building a buffer that holds a serialized WebSocket frame rather than JSON.
 */
void mi_json_bytes(mi_json_buf *b, const void *data, size_t len);
void mi_json_string(mi_json_buf *b, const char *s); /* NULL -> null */
void mi_json_int(mi_json_buf *b, int64_t v);
void mi_json_double(mi_json_buf *b, double v);
void mi_json_bool(mi_json_buf *b, int v);
void mi_json_null(mi_json_buf *b);
/* Writes "key": (escaped). */
void mi_json_key(mi_json_buf *b, const char *key);


/* ------------------------------------------------------------------ */
/* Reader                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    MI_JSON_NULL,
    MI_JSON_BOOL,
    MI_JSON_NUMBER,
    MI_JSON_STRING,
    MI_JSON_ARRAY,
    MI_JSON_OBJECT
} mi_json_type;

typedef struct mi_json {
    mi_json_type type;
    int boolean;
    double number;
    char *string;      /* MI_JSON_STRING */
    struct mi_json **items;
    size_t count;      /* array length */
    char **keys;       /* object member names */
    struct mi_json **values;
    size_t member_count;
} mi_json;

/* Parse a NUL-terminated JSON document. Returns NULL on syntax error. */
mi_json *mi_json_parse(const char *text);
void mi_json_value_free(mi_json *v);

const mi_json *mi_json_get(const mi_json *obj, const char *key);
const char *mi_json_get_string(const mi_json *obj, const char *key,
                               const char *fallback);
double mi_json_get_number(const mi_json *obj, const char *key, double fallback);
int64_t mi_json_get_int(const mi_json *obj, const char *key, int64_t fallback);
int mi_json_get_bool(const mi_json *obj, const char *key, int fallback);

/*
 * Re-serialise a parsed value into a buffer.
 *
 * The reader has no "copy this subtree" primitive, so a caller that wants a
 * parsed node on its own — the rows out of a database response, say — emits it
 * here and parses the result.
 */
void mi_json_emit_value(mi_json_buf *b, const mi_json *v);

#endif /* MI_JSON_H */
