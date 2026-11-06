#include "mi_json.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mi_util.h"

/* ------------------------------------------------------------------ */
/* Writer                                                              */
/* ------------------------------------------------------------------ */

void mi_json_init(mi_json_buf *b) {
    b->cap = 256;
    b->len = 0;
    b->data = mi_malloc(b->cap);
    if (b->data) b->data[0] = '\0';
}

void mi_json_free(mi_json_buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

const char *mi_json_data(const mi_json_buf *b) { return b->data ? b->data : ""; }

static void json_reserve(mi_json_buf *b, size_t extra) {
    if (!b->data) return;
    if (b->len + extra + 1 <= b->cap) return;
    size_t want = b->cap * 2;
    while (want < b->len + extra + 1) want *= 2;
    char *grown = mi_realloc(b->data, want);
    if (!grown) return;
    b->data = grown;
    b->cap = want;
}

void mi_json_raw(mi_json_buf *b, const char *text) {
    if (!text) return;
    size_t n = strlen(text);
    json_reserve(b, n);
    memcpy(b->data + b->len, text, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void mi_json_bytes(mi_json_buf *b, const void *data, size_t len) {
    if (!data || len == 0) return;
    json_reserve(b, len);
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = '\0';
}

void mi_json_string(mi_json_buf *b, const char *s) {
    if (!s) {
        mi_json_raw(b, "null");
        return;
    }
    json_reserve(b, strlen(s) * 6 + 2);
    b->data[b->len++] = '"';
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"': mi_json_raw(b, "\\\""); break;
            case '\\': mi_json_raw(b, "\\\\"); break;
            case '\n': mi_json_raw(b, "\\n"); break;
            case '\r': mi_json_raw(b, "\\r"); break;
            case '\t': mi_json_raw(b, "\\t"); break;
            case '\b': mi_json_raw(b, "\\b"); break;
            case '\f': mi_json_raw(b, "\\f"); break;
            default:
                if (*p < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof esc, "\\u%04x", *p);
                    mi_json_raw(b, esc);
                } else {
                    json_reserve(b, 1);
                    b->data[b->len++] = (char)*p;
                }
        }
    }
    json_reserve(b, 1);
    b->data[b->len++] = '"';
    b->data[b->len] = '\0';
}

void mi_json_int(mi_json_buf *b, int64_t v) {
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%lld", (long long)v);
    mi_json_raw(b, tmp);
}

void mi_json_double(mi_json_buf *b, double v) {
    if (!isfinite(v)) {
        mi_json_raw(b, "null");
        return;
    }
    char tmp[40];
    snprintf(tmp, sizeof tmp, "%.6g", v);
    mi_json_raw(b, tmp);
}

void mi_json_bool(mi_json_buf *b, int v) { mi_json_raw(b, v ? "true" : "false"); }
void mi_json_null(mi_json_buf *b) { mi_json_raw(b, "null"); }

void mi_json_key(mi_json_buf *b, const char *key) {
    mi_json_string(b, key);
    mi_json_raw(b, ":");
}

void mi_json_emit_value(mi_json_buf *b, const mi_json *v) {
    if (!v) {
        mi_json_raw(b, "null");
        return;
    }
    switch (v->type) {
        case MI_JSON_NULL:
            mi_json_raw(b, "null");
            break;
        case MI_JSON_BOOL:
            mi_json_bool(b, v->boolean);
            break;
        case MI_JSON_NUMBER:
            mi_json_double(b, v->number);
            break;
        case MI_JSON_STRING:
            mi_json_string(b, v->string);
            break;
        case MI_JSON_ARRAY:
            mi_json_raw(b, "[");
            for (size_t i = 0; i < v->count; i++) {
                if (i) mi_json_raw(b, ",");
                mi_json_emit_value(b, v->items[i]);
            }
            mi_json_raw(b, "]");
            break;
        case MI_JSON_OBJECT:
            mi_json_raw(b, "{");
            for (size_t i = 0; i < v->member_count; i++) {
                if (i) mi_json_raw(b, ",");
                mi_json_key(b, v->keys[i]);
                mi_json_emit_value(b, v->values[i]);
            }
            mi_json_raw(b, "}");
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Reader                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *p;
    int depth;
} parser;

#define JSON_MAX_DEPTH 64

static void skip_ws(parser *ps) {
    while (*ps->p && isspace((unsigned char)*ps->p)) ps->p++;
}

static mi_json *parse_value(parser *ps);

static mi_json *value_new(mi_json_type t) {
    mi_json *v = mi_calloc(1, sizeof *v);
    if (v) v->type = t;
    return v;
}

void mi_json_value_free(mi_json *v) {
    if (!v) return;
    free(v->string);
    if (v->items) {
        for (size_t i = 0; i < v->count; i++) mi_json_value_free(v->items[i]);
        free(v->items);
    }
    if (v->values) {
        for (size_t i = 0; i < v->member_count; i++) mi_json_value_free(v->values[i]);
        free(v->values);
    }
    if (v->keys) {
        for (size_t i = 0; i < v->member_count; i++) free(v->keys[i]);
        free(v->keys);
    }
    free(v);
}

static char *parse_string_raw(parser *ps) {
    if (*ps->p != '"') return NULL;
    ps->p++;
    mi_json_buf b;
    mi_json_init(&b);
    while (*ps->p && *ps->p != '"') {
        if (*ps->p == '\\') {
            ps->p++;
            switch (*ps->p) {
                case '"': mi_json_raw(&b, "\""); break;
                case '\\': mi_json_raw(&b, "\\"); break;
                case '/': mi_json_raw(&b, "/"); break;
                case 'n': mi_json_raw(&b, "\n"); break;
                case 't': mi_json_raw(&b, "\t"); break;
                case 'r': mi_json_raw(&b, "\r"); break;
                case 'b': mi_json_raw(&b, "\b"); break;
                case 'f': mi_json_raw(&b, "\f"); break;
                case 'u': {
                    /* Decode a BMP code point as UTF-8. */
                    char hex[5] = {0};
                    for (int i = 0; i < 4 && ps->p[1 + i]; i++) hex[i] = ps->p[1 + i];
                    unsigned cp = (unsigned)strtoul(hex, NULL, 16);
                    ps->p += 4;
                    char utf8[4];
                    int n = 0;
                    if (cp < 0x80) {
                        utf8[n++] = (char)cp;
                    } else if (cp < 0x800) {
                        utf8[n++] = (char)(0xC0 | (cp >> 6));
                        utf8[n++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        utf8[n++] = (char)(0xE0 | (cp >> 12));
                        utf8[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        utf8[n++] = (char)(0x80 | (cp & 0x3F));
                    }
                    utf8[n] = '\0';
                    mi_json_raw(&b, utf8);
                    break;
                }
                default: mi_json_raw(&b, ""); break;
            }
            if (*ps->p) ps->p++;
        } else {
            json_reserve(&b, 1);
            b.data[b.len++] = *ps->p++;
            b.data[b.len] = '\0';
        }
    }
    if (*ps->p != '"') {
        mi_json_free(&b);
        return NULL;
    }
    ps->p++;
    return b.data;
}

static mi_json *parse_array(parser *ps) {
    mi_json *arr = value_new(MI_JSON_ARRAY);
    if (!arr) return NULL;
    ps->p++; /* [ */
    skip_ws(ps);
    if (*ps->p == ']') {
        ps->p++;
        return arr;
    }
    for (;;) {
        mi_json *item = parse_value(ps);
        if (!item) {
            mi_json_value_free(arr);
            return NULL;
        }
        mi_json **grown = mi_realloc(arr->items, (arr->count + 1) * sizeof(mi_json *));
        if (!grown) {
            mi_json_value_free(item);
            mi_json_value_free(arr);
            return NULL;
        }
        arr->items = grown;
        arr->items[arr->count++] = item;
        skip_ws(ps);
        if (*ps->p == ',') {
            ps->p++;
            skip_ws(ps);
            continue;
        }
        if (*ps->p == ']') {
            ps->p++;
            return arr;
        }
        mi_json_value_free(arr);
        return NULL;
    }
}

static mi_json *parse_object(parser *ps) {
    mi_json *obj = value_new(MI_JSON_OBJECT);
    if (!obj) return NULL;
    ps->p++; /* { */
    skip_ws(ps);
    if (*ps->p == '}') {
        ps->p++;
        return obj;
    }
    for (;;) {
        skip_ws(ps);
        char *key = parse_string_raw(ps);
        if (!key) {
            mi_json_value_free(obj);
            return NULL;
        }
        skip_ws(ps);
        if (*ps->p != ':') {
            free(key);
            mi_json_value_free(obj);
            return NULL;
        }
        ps->p++;
        mi_json *val = parse_value(ps);
        if (!val) {
            free(key);
            mi_json_value_free(obj);
            return NULL;
        }
        char **keys = mi_realloc(obj->keys, (obj->member_count + 1) * sizeof(char *));
        mi_json **vals = mi_realloc(obj->values, (obj->member_count + 1) * sizeof(mi_json *));
        if (!keys || !vals) {
            free(key);
            mi_json_value_free(val);
            mi_json_value_free(obj);
            return NULL;
        }
        obj->keys = keys;
        obj->values = vals;
        obj->keys[obj->member_count] = key;
        obj->values[obj->member_count] = val;
        obj->member_count++;
        skip_ws(ps);
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == '}') {
            ps->p++;
            return obj;
        }
        mi_json_value_free(obj);
        return NULL;
    }
}

static mi_json *parse_value(parser *ps) {
    if (ps->depth++ > JSON_MAX_DEPTH) {
        ps->depth--;
        return NULL;
    }
    skip_ws(ps);
    mi_json *result = NULL;
    if (*ps->p == '{') {
        result = parse_object(ps);
    } else if (*ps->p == '[') {
        result = parse_array(ps);
    } else if (*ps->p == '"') {
        char *s = parse_string_raw(ps);
        if (s) {
            result = value_new(MI_JSON_STRING);
            if (result) result->string = s;
            else free(s);
        }
    } else if (!strncmp(ps->p, "true", 4)) {
        ps->p += 4;
        result = value_new(MI_JSON_BOOL);
        if (result) result->boolean = 1;
    } else if (!strncmp(ps->p, "false", 5)) {
        ps->p += 5;
        result = value_new(MI_JSON_BOOL);
    } else if (!strncmp(ps->p, "null", 4)) {
        ps->p += 4;
        result = value_new(MI_JSON_NULL);
    } else {
        char *end = NULL;
        double d = strtod(ps->p, &end);
        if (end && end != ps->p) {
            ps->p = end;
            result = value_new(MI_JSON_NUMBER);
            if (result) result->number = d;
        }
    }
    ps->depth--;
    return result;
}

mi_json *mi_json_parse(const char *text) {
    if (!text) return NULL;
    parser ps = {.p = text, .depth = 0};
    mi_json *v = parse_value(&ps);
    if (!v) return NULL;
    skip_ws(&ps);
    if (*ps.p != '\0') {
        mi_json_value_free(v);
        return NULL;
    }
    return v;
}

const mi_json *mi_json_get(const mi_json *obj, const char *key) {
    if (!obj || obj->type != MI_JSON_OBJECT || !key) return NULL;
    for (size_t i = 0; i < obj->member_count; i++) {
        if (!strcmp(obj->keys[i], key)) return obj->values[i];
    }
    return NULL;
}

const char *mi_json_get_string(const mi_json *obj, const char *key,
                               const char *fallback) {
    const mi_json *v = mi_json_get(obj, key);
    if (v && v->type == MI_JSON_STRING && v->string) return v->string;
    return fallback;
}

double mi_json_get_number(const mi_json *obj, const char *key, double fallback) {
    const mi_json *v = mi_json_get(obj, key);
    if (v && v->type == MI_JSON_NUMBER) return v->number;
    return fallback;
}

int64_t mi_json_get_int(const mi_json *obj, const char *key, int64_t fallback) {
    const mi_json *v = mi_json_get(obj, key);
    if (v && v->type == MI_JSON_NUMBER) return (int64_t)v->number;
    return fallback;
}

int mi_json_get_bool(const mi_json *obj, const char *key, int fallback) {
    const mi_json *v = mi_json_get(obj, key);
    if (v && v->type == MI_JSON_BOOL) return v->boolean;
    return fallback;
}
