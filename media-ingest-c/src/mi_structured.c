#define _GNU_SOURCE
#include "mi_structured.h"
#include "mi_json.h"
#include "mi_util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* The list                                                            */
/* ------------------------------------------------------------------ */

void mi_props_free(mi_props *p) {
    if (!p) return;
    for (size_t i = 0; i < p->count; i++) {
        free(p->names[i]);
        free(p->values[i]);
    }
    free(p->names);
    free(p->values);
    memset(p, 0, sizeof *p);
}

static void add(mi_props *p, const char *name, const char *value) {
    if (!name || !*name) return;
    if (!value) value = "";
    size_t n = p->count + 1;
    char **gn = mi_realloc(p->names, n * sizeof *gn);
    char **gv = mi_realloc(p->values, n * sizeof *gv);
    if (!gn || !gv) { free(gn); free(gv); return; }
    p->names = gn;
    p->values = gv;
    p->names[p->count] = mi_strdup(name);
    p->values[p->count] = mi_strdup(value);
    p->count++;
}

const char *mi_props_get(const mi_props *p, const char *name) {
    if (!p || !name) return NULL;
    for (size_t i = 0; i < p->count; i++)
        if (!strcasecmp(p->names[i], name)) return p->values[i];
    return NULL;
}

size_t mi_props_count(const mi_props *p, const char *name) {
    size_t n = 0;
    if (!p || !name) return 0;
    for (size_t i = 0; i < p->count; i++)
        if (!strcasecmp(p->names[i], name)) n++;
    return n;
}

const char *mi_props_get_n(const mi_props *p, const char *name, size_t n) {
    if (!p || !name) return NULL;
    size_t seen = 0;
    for (size_t i = 0; i < p->count; i++) {
        if (!strcasecmp(p->names[i], name)) {
            if (seen == n) return p->values[i];
            seen++;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* JSON-LD                                                             */
/* ------------------------------------------------------------------ */

/* A scalar JSON value as text. A number keeps its form; a boolean is
 * true/false; an object or array is skipped (the caller recurses). */
static char *json_scalar(const mi_json *v) {
    if (!v) return NULL;
    switch (v->type) {
        case MI_JSON_STRING: return mi_strdup(v->string ? v->string : "");
        case MI_JSON_NUMBER: {
            char buf[40];
            if (v->number == (double)(long long)v->number)
                snprintf(buf, sizeof buf, "%lld", (long long)v->number);
            else
                snprintf(buf, sizeof buf, "%.6g", v->number);
            return mi_strdup(buf);
        }
        case MI_JSON_BOOL: return mi_strdup(v->boolean ? "true" : "false");
        case MI_JSON_NULL: return mi_strdup("");
        default: return NULL;
    }
}

/*
 * Flatten a JSON-LD value into name=value pairs, with dotted names for nesting.
 *
 * A JSON-LD document is a graph, and a value may be a scalar, an object, or an
 * array of either. The shape a caller wants is a flat list, so nesting becomes a
 * dotted name and an array becomes repeated names.
 */
static void flatten_json(const mi_json *v, const char *prefix, mi_props *out) {
    if (!v) return;
    if (v->type == MI_JSON_OBJECT) {
        for (size_t i = 0; i < v->member_count; i++) {
            const char *key = v->keys[i];
            char *name = (prefix && *prefix) ? mi_asprintf("%s.%s", prefix, key)
                                             : mi_strdup(key);
            flatten_json(v->values[i], name, out);
            free(name);
        }
        return;
    }
    if (v->type == MI_JSON_ARRAY) {
        for (size_t i = 0; i < v->count; i++) flatten_json(v->items[i], prefix, out);
        return;
    }
    char *s = json_scalar(v);
    if (s) {
        add(out, prefix ? prefix : "", s);
        free(s);
    }
}

/* Find every <script type="application/ld+json"> and parse its content. */
static void extract_json_ld(const mi_html_node *doc, mi_props *out) {
    for (const mi_html_node *n = mi_html_next(doc, doc); n; n = mi_html_next(doc, n)) {
        if (!mi_html_is(n, "script")) continue;
        const char *type = mi_html_attr(n, "type");
        if (!type || strcasecmp(type, "application/ld+json") != 0) continue;
        /* The script's text is its child text node. */
        const mi_html_node *text = n->first_child;
        if (!text || text->type != MI_HTML_TEXT || !text->text) continue;
        mi_json *doc_json = mi_json_parse(text->text);
        if (!doc_json) continue;
        flatten_json(doc_json, NULL, out);
        mi_json_value_free(doc_json);
    }
}

/* ------------------------------------------------------------------ */
/* microdata / RDFa                                                    */
/* ------------------------------------------------------------------ */

/* The itemtype/itemprop (microdata) or typeof/property (RDFa) attribute on a
 * node, whichever it has. */
static const char *item_attr(const mi_html_node *n, const char *a1, const char *a2) {
    const char *v = mi_html_attr(n, a1);
    if (v) return v;
    return a2 ? mi_html_attr(n, a2) : NULL;
}

/*
 * Walk the tree carrying the current itemprop prefix, so a nested itemscope's
 * properties are dotted (`offers.price`), matching the JSON-LD shape.
 */
static void walk_items(const mi_html_node *n, const char *prefix, mi_props *out) {
    for (; n; n = n->next_sibling) {
        if (n->type != MI_HTML_ELEMENT) continue;
        /*
         * A <meta> is OpenGraph/Twitter, read by extract_meta. It also carries
         * `property=`, which is the *same attribute name* RDFa uses, so without
         * this a `og:title` meta would be read twice — once as a meta, once as
         * an RDFa property.
         */
        if ((mi_html_is(n, "meta") || mi_html_is(n, "link")) &&
            !mi_html_attr(n, "itemprop"))
            continue;

        const char *scope = mi_html_attr(n, "itemscope");
        const char *type = item_attr(n, "itemtype", "typeof");
        const char *prop = item_attr(n, "itemprop", "property");
        const char *content = mi_html_attr(n, "content"); /* a meta carries the value here */
        const char *href = mi_html_attr(n, "href");
        const char *src = mi_html_attr(n, "src");

        if (type && !prop) add(out, "@type", type);

        /* The prefix for this element's children. It is `prefix` unless a
         * nested itemscope names a new one, in which case we allocated it and
         * must free it — so ownership is tracked explicitly. */
        const char *child_prefix = prefix;
        char *allocated = NULL;

        if (prop) {
            char *full = (prefix && *prefix) ? mi_asprintf("%s.%s", prefix, prop)
                                             : mi_strdup(prop);
            const char *value = content ? content : (href ? href : src);
            if (value) {
                add(out, full, value);
            } else if (scope) {
                /* A nested item: its properties are dotted under this name. */
                if (type) add(out, full, type);
                child_prefix = allocated = full;
            } else {
                char *text = mi_html_text(n);
                if (text) {
                    add(out, full, text);
                    free(text);
                }
            }
            if (allocated != full) free(full);
        }

        if (n->first_child) walk_items(n->first_child, child_prefix, out);
        free(allocated);
    }
}

/* ------------------------------------------------------------------ */
/* meta tags                                                           */
/* ------------------------------------------------------------------ */

static void extract_meta(const mi_html_node *doc, mi_props *out) {
    for (const mi_html_node *n = mi_html_next(doc, doc); n; n = mi_html_next(doc, n)) {
        if (!mi_html_is(n, "meta")) continue;
        const char *prop = mi_html_attr(n, "property");
        const char *name = mi_html_attr(n, "name");
        const char *content = mi_html_attr(n, "content");
        const char *key = prop ? prop : name;
        if (!key || !content) continue;
        /* Keep the meta tags that carry structured data: OpenGraph (og:*),
         * Twitter cards (twitter:*), and the schema-ish article/product ones. */
        if (!strncasecmp(key, "og:", 3) || !strncasecmp(key, "twitter:", 8) ||
            !strncasecmp(key, "article:", 8) || !strncasecmp(key, "product:", 8) ||
            !strncasecmp(key, "profile:", 8) || !strncasecmp(key, "al:", 3)) {
            add(out, key, content);
        }
    }
}

int mi_structured_extract(const mi_html_node *doc, mi_props *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!doc) return 0;
    extract_json_ld(doc, out);
    extract_meta(doc, out);
    walk_items(doc->first_child, NULL, out);
    return 0;
}

/* ------------------------------------------------------------------ */
/* JSON output                                                         */
/* ------------------------------------------------------------------ */

char *mi_props_to_json(const mi_props *p) {
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{");
    int first = 1;
    for (size_t i = 0; i < p->count; i++) {
        /* A name already written is skipped here; it is emitted once, as an
         * array, by the branch below. */
        int seen = 0;
        for (size_t j = 0; j < i; j++)
            if (!strcasecmp(p->names[j], p->names[i])) { seen = 1; break; }
        if (seen) continue;
        if (!first) mi_json_raw(&b, ",");
        first = 0;
        mi_json_key(&b, p->names[i]);
        size_t n = mi_props_count(p, p->names[i]);
        if (n == 1) {
            mi_json_string(&b, mi_props_get(p, p->names[i]));
        } else {
            mi_json_raw(&b, "[");
            for (size_t k = 0; k < n; k++) {
                if (k) mi_json_raw(&b, ",");
                mi_json_string(&b, mi_props_get_n(p, p->names[i], k));
            }
            mi_json_raw(&b, "]");
        }
    }
    mi_json_raw(&b, "}");
    char *out = mi_strdup(mi_json_data(&b));
    mi_json_free(&b);
    return out;
}
