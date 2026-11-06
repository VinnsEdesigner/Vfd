#define _GNU_SOURCE
#include "mi_extract_rows.h"
#include "mi_css.h"
#include "mi_json.h"
#include "mi_util.h"
#include "mi_xpath.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Rule parsing                                                        */
/* ------------------------------------------------------------------ */

static mi_field_from from_name(const char *s) {
    if (!s || !*s) return MI_FIELD_TEXT;
    if (!strcmp(s, "own_text")) return MI_FIELD_OWN_TEXT;
    if (!strcmp(s, "html")) return MI_FIELD_HTML;
    if (!strcmp(s, "attr")) return MI_FIELD_ATTR;
    if (!strcmp(s, "href")) return MI_FIELD_HREF;
    if (!strcmp(s, "src")) return MI_FIELD_SRC;
    if (!strcmp(s, "exists")) return MI_FIELD_EXISTS;
    if (!strcmp(s, "count")) return MI_FIELD_COUNT;
    return MI_FIELD_TEXT;
}

mi_row_rule *mi_row_rule_parse(const char *json, char *err, size_t errcap) {
    if (err && errcap) err[0] = '\0';
    mi_json *doc = json ? mi_json_parse(json) : NULL;
    if (!doc) {
        if (err && errcap) snprintf(err, errcap, "the rule is not valid JSON");
        return NULL;
    }
    mi_row_rule *r = mi_calloc(1, sizeof *r);
    if (!r) { mi_json_value_free(doc); return NULL; }

    const char *container = mi_json_get_string(doc, "container", NULL);
    if (container && *container) r->container = mi_strdup(container);
    r->container_xpath = mi_json_get_bool(doc, "container_xpath", 0);

    const mi_json *fields = mi_json_get(doc, "fields");
    if (!fields || fields->type != MI_JSON_ARRAY || fields->count == 0) {
        if (err && errcap) snprintf(err, errcap, "the rule needs a non-empty 'fields' array");
        mi_json_value_free(doc);
        mi_row_rule_free(r);
        return NULL;
    }
    r->fields = mi_calloc(fields->count, sizeof *r->fields);
    if (!r->fields) { mi_json_value_free(doc); mi_row_rule_free(r); return NULL; }

    for (size_t i = 0; i < fields->count; i++) {
        const mi_json *f = fields->items[i];
        if (!f || f->type != MI_JSON_OBJECT) {
            if (err && errcap) snprintf(err, errcap, "field %zu is not an object", i);
            mi_json_value_free(doc);
            mi_row_rule_free(r);
            return NULL;
        }
        mi_field_rule *fr = &r->fields[r->field_count];
        const char *name = mi_json_get_string(f, "name", NULL);
        const char *sel = mi_json_get_string(f, "selector", NULL);
        const char *xp = mi_json_get_string(f, "xpath", NULL);
        if (!name || !*name) {
            if (err && errcap) snprintf(err, errcap, "field %zu has no 'name'", i);
            mi_json_value_free(doc);
            mi_row_rule_free(r);
            return NULL;
        }
        if ((!sel || !*sel) && (!xp || !*xp)) {
            if (err && errcap) snprintf(err, errcap, "field '%s' has no selector", name);
            mi_json_value_free(doc);
            mi_row_rule_free(r);
            return NULL;
        }
        fr->name = mi_strdup(name);
        if (xp && *xp) { fr->selector = mi_strdup(xp); fr->xpath = 1; }
        else fr->selector = mi_strdup(sel);
        fr->from = from_name(mi_json_get_string(f, "from", NULL));
        const char *attr = mi_json_get_string(f, "attr", NULL);
        if (attr && *attr) fr->attr = mi_strdup(attr);
        const char *dv = mi_json_get_string(f, "default", NULL);
        if (dv) fr->default_value = mi_strdup(dv);

        if (fr->from == MI_FIELD_ATTR && !fr->attr) {
            if (err && errcap) snprintf(err, errcap, "field '%s' reads an attribute but names none", name);
            mi_json_value_free(doc);
            mi_row_rule_free(r);
            return NULL;
        }
        r->field_count++;
    }
    mi_json_value_free(doc);
    return r;
}

void mi_row_rule_free(mi_row_rule *r) {
    if (!r) return;
    free(r->container);
    for (size_t i = 0; i < r->field_count; i++) {
        free(r->fields[i].name);
        free(r->fields[i].selector);
        free(r->fields[i].attr);
        free(r->fields[i].default_value);
    }
    free(r->fields);
    free(r);
}

/* ------------------------------------------------------------------ */
/* Applying a rule                                                     */
/* ------------------------------------------------------------------ */

/* Resolve `ref` against `base`, when ref is relative. A small copy of the
 * browse module's rule, kept local so this module stands alone. */
static char *resolve_url(const char *base, const char *ref) {
    if (!ref || !*ref) return NULL;
    if (strstr(ref, "://") || !strncmp(ref, "//", 2) || !strncmp(ref, "data:", 5))
        return mi_strdup(ref);
    if (!base || !*base) return mi_strdup(ref);
    if (*ref == '#') return mi_strdup(base);
    if (*ref == '/') {
        /* Root-relative: scheme://host + ref. */
        const char *p = strstr(base, "://");
        if (!p) return mi_strdup(ref);
        const char *slash = strchr(p + 3, '/');
        size_t origin = slash ? (size_t)(slash - base) : strlen(base);
        char *out = mi_malloc(origin + strlen(ref) + 1);
        if (!out) return NULL;
        memcpy(out, base, origin);
        strcpy(out + origin, ref);
        return out;
    }
    /* Relative: base up to its last slash, then ref. */
    const char *slash = strrchr(base, '/');
    if (!slash) return mi_strdup(ref);
    size_t dir = (size_t)(slash - base) + 1;
    char *out = mi_malloc(dir + strlen(ref) + 1);
    if (!out) return NULL;
    memcpy(out, base, dir);
    strcpy(out + dir, ref);
    return out;
}

/* Inner HTML of an element, as text. */
static char *inner_html(const mi_html_node *el) {
    mi_json_buf b;
    mi_json_init(&b);
    /* A shallow serialiser: tags and text, enough for a cell's markup. */
    for (const mi_html_node *c = el->first_child; c; c = c->next_sibling) {
        if (c->type == MI_HTML_TEXT) mi_json_raw(&b, c->text ? c->text : "");
        else if (c->type == MI_HTML_ELEMENT) {
            mi_json_raw(&b, "<");
            mi_json_raw(&b, c->name ? c->name : "");
            for (size_t i = 0; i < c->attr_count; i++) {
                mi_json_raw(&b, " ");
                mi_json_raw(&b, c->attr_names[i]);
                mi_json_raw(&b, "=\"");
                mi_json_raw(&b, c->attr_values[i]);
                mi_json_raw(&b, "\"");
            }
            mi_json_raw(&b, ">");
            char *inner = inner_html(c);
            if (inner) { mi_json_raw(&b, inner); free(inner); }
            mi_json_raw(&b, "</");
            mi_json_raw(&b, c->name ? c->name : "");
            mi_json_raw(&b, ">");
        }
    }
    char *out = mi_strdup(mi_json_data(&b));
    mi_json_free(&b);
    return out;
}

/*
 * Read a field whose selector is an XPath *value* selector (`/@href`,
 * `/text()`). The XPath engine returns the containing node, so the value is read
 * from it here — the one place that knows the field asked for a value and not an
 * element.
 */
static char *read_xpath_value(const mi_field_rule *fr, const mi_html_node *scope,
                              const char *page_url) {
    char err[128];
    mi_xpath *x = mi_xpath_compile(fr->selector, err, sizeof err);
    if (!x) return NULL;
    mi_html_node **out = NULL;
    size_t n = 0;
    mi_xpath_select(x, scope, &out, &n);
    char *value = NULL;
    if (n > 0) {
        if (mi_xpath_is_attribute(x)) {
            const char *a = mi_xpath_result_attribute(x);
            const char *v = mi_html_attr(out[0], a);
            if (v && (!strcmp(a, "href") || !strcmp(a, "src"))) {
                value = resolve_url(page_url, v);
            } else {
                value = v ? mi_strdup(v) : NULL;
            }
        } else if (mi_xpath_is_text(x)) {
            value = mi_html_text(out[0]);
        }
    }
    free(out);
    mi_xpath_free(x);
    return value;
}

/* Read one field from one matched element. */
static char *read_field(const mi_field_rule *fr, const mi_html_node *el,
                        const char *page_url) {
    switch (fr->from) {
        case MI_FIELD_TEXT: return mi_html_text(el);
        case MI_FIELD_OWN_TEXT: return mi_html_own_text(el);
        case MI_FIELD_HTML: return inner_html(el);
        case MI_FIELD_ATTR: {
            const char *v = mi_html_attr(el, fr->attr);
            return v ? mi_strdup(v) : NULL;
        }
        case MI_FIELD_HREF: {
            const char *v = mi_html_attr(el, "href");
            return resolve_url(page_url, v);
        }
        case MI_FIELD_SRC: {
            const char *v = mi_html_attr(el, "src");
            if (!v) v = mi_html_attr(el, "data-src");
            return resolve_url(page_url, v);
        }
        default:
            return NULL;
    }
}

/* Evaluate a field's selector against `scope` (a row's container, or the
 * document for the whole-page case), returning the first match or NULL. */
static mi_html_node *first_match(const mi_field_rule *fr, const mi_html_node *scope) {
    if (fr->xpath) {
        char err[128];
        mi_xpath *x = mi_xpath_compile(fr->selector, err, sizeof err);
        if (!x) return NULL;
        mi_html_node **out = NULL;
        size_t n = 0;
        mi_xpath_select(x, scope, &out, &n);
        mi_html_node *first = n ? out[0] : NULL;
        free(out);
        mi_xpath_free(x);
        return first;
    }
    char err[128];
    mi_css_selector *s = mi_css_compile(fr->selector, err, sizeof err);
    if (!s) return NULL;
    mi_html_node **out = NULL;
    size_t n = 0;
    mi_css_select(s, scope, &out, &n);
    mi_html_node *first = n ? out[0] : NULL;
    free(out);
    mi_css_free(s);
    return first;
}

static size_t count_matches(const mi_field_rule *fr, const mi_html_node *scope) {
    if (fr->xpath) {
        char err[128];
        mi_xpath *x = mi_xpath_compile(fr->selector, err, sizeof err);
        if (!x) return 0;
        mi_html_node **out = NULL;
        size_t n = 0;
        mi_xpath_select(x, scope, &out, &n);
        free(out);
        mi_xpath_free(x);
        return n;
    }
    char err[128];
    mi_css_selector *s = mi_css_compile(fr->selector, err, sizeof err);
    if (!s) return 0;
    mi_html_node **out = NULL;
    size_t n = 0;
    mi_css_select(s, scope, &out, &n);
    free(out);
    mi_css_free(s);
    return n;
}

int mi_extract_rows(const mi_row_rule *rule, const char *html, size_t len,
                    const char *page_url, mi_rows *out, char *err, size_t errcap) {
    if (err && errcap) err[0] = '\0';
    memset(out, 0, sizeof *out);
    if (!rule) { if (err && errcap) snprintf(err, errcap, "no rule"); return -1; }

    mi_html_node *doc = mi_html_parse(html, len);
    if (!doc) { if (err && errcap) snprintf(err, errcap, "could not parse the page"); return -1; }

    /* The field names, in order. */
    out->names = mi_calloc(rule->field_count, sizeof *out->names);
    for (size_t i = 0; i < rule->field_count; i++) out->names[i] = mi_strdup(rule->fields[i].name);
    out->name_count = rule->field_count;

    /* The scopes: one per container match, or the document itself. */
    mi_html_node **scopes = NULL;
    size_t scope_n = 0;
    if (rule->container && *rule->container) {
        mi_field_rule tmp = {.name = (char *)"", .selector = rule->container,
                             .xpath = rule->container_xpath, .from = MI_FIELD_TEXT};
        if (rule->container_xpath) {
            char xerr[128];
            mi_xpath *x = mi_xpath_compile(rule->container, xerr, sizeof xerr);
            if (!x) {
                if (err && errcap) snprintf(err, errcap, "container: %s", xerr);
                mi_rows_free(out);
                mi_html_free(doc);
                return -1;
            }
            mi_xpath_select(x, doc, &scopes, &scope_n);
            mi_xpath_free(x);
        } else {
            char cerr[128];
            mi_css_selector *s = mi_css_compile(rule->container, cerr, sizeof cerr);
            if (!s) {
                if (err && errcap) snprintf(err, errcap, "container: %s", cerr);
                mi_rows_free(out);
                mi_html_free(doc);
                return -1;
            }
            mi_css_select(s, doc, &scopes, &scope_n);
            mi_css_free(s);
        }
        (void)tmp;
    } else {
        scopes = mi_malloc(sizeof *scopes);
        scopes[0] = doc;
        scope_n = 1;
    }

    if (scope_n > 0) {
        out->rows = mi_calloc(scope_n, sizeof *out->rows);
        for (size_t i = 0; i < scope_n; i++) {
            mi_row *row = &out->rows[i];
            row->count = rule->field_count;
            row->values = mi_calloc(rule->field_count, sizeof *row->values);
            for (size_t j = 0; j < rule->field_count; j++) {
                const mi_field_rule *fr = &rule->fields[j];
                char *v = NULL;
                if (fr->from == MI_FIELD_EXISTS) {
                    v = mi_strdup(count_matches(fr, scopes[i]) > 0 ? "1" : "");
                } else if (fr->from == MI_FIELD_COUNT) {
                    v = mi_asprintf("%zu", count_matches(fr, scopes[i]));
                } else if (fr->xpath) {
                    /* An XPath field may be a value selector (`/@href`,
                     * `/text()`) or a plain node path; try the value first and
                     * fall back to reading the matched element. */
                    char xerr[128];
                    mi_xpath *probe = mi_xpath_compile(fr->selector, xerr, sizeof xerr);
                    int value_sel = probe && (mi_xpath_is_attribute(probe) ||
                                              mi_xpath_is_text(probe));
                    mi_xpath_free(probe);
                    if (value_sel) {
                        v = read_xpath_value(fr, scopes[i], page_url);
                    } else {
                        mi_html_node *el = first_match(fr, scopes[i]);
                        if (el) v = read_field(fr, el, page_url);
                    }
                } else {
                    mi_html_node *el = first_match(fr, scopes[i]);
                    if (el) v = read_field(fr, el, page_url);
                }
                if (!v && fr->default_value) v = mi_strdup(fr->default_value);
                row->values[j] = v ? v : mi_strdup("");
            }
        }
        out->row_count = scope_n;
    }
    free(scopes);
    mi_html_free(doc);
    return 0;
}

void mi_rows_free(mi_rows *r) {
    if (!r) return;
    for (size_t i = 0; i < r->row_count; i++) {
        for (size_t j = 0; j < r->rows[i].count; j++) free(r->rows[i].values[j]);
        free(r->rows[i].values);
    }
    free(r->rows);
    for (size_t i = 0; i < r->name_count; i++) free(r->names[i]);
    free(r->names);
    memset(r, 0, sizeof *r);
}

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

/* Whether a CSV field needs quoting. */
static int csv_needs_quote(const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') return 1;
    }
    return 0;
}

static void csv_field(mi_json_buf *b, const char *s) {
    if (!csv_needs_quote(s)) { mi_json_raw(b, s); return; }
    mi_json_raw(b, "\"");
    for (const char *p = s; *p; p++) {
        if (*p == '"') mi_json_raw(b, "\"\"");
        else { char c[2] = {*p, 0}; mi_json_raw(b, c); }
    }
    mi_json_raw(b, "\"");
}

char *mi_rows_to_csv(const mi_rows *rows) {
    mi_json_buf b;
    mi_json_init(&b);
    for (size_t i = 0; i < rows->name_count; i++) {
        if (i) mi_json_raw(&b, ",");
        csv_field(&b, rows->names[i]);
    }
    mi_json_raw(&b, "\r\n");
    for (size_t r = 0; r < rows->row_count; r++) {
        for (size_t c = 0; c < rows->rows[r].count; c++) {
            if (c) mi_json_raw(&b, ",");
            csv_field(&b, rows->rows[r].values[c]);
        }
        mi_json_raw(&b, "\r\n");
    }
    char *out = mi_strdup(mi_json_data(&b));
    mi_json_free(&b);
    return out;
}

char *mi_rows_to_json(const mi_rows *rows) {
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "[");
    for (size_t r = 0; r < rows->row_count; r++) {
        if (r) mi_json_raw(&b, ",");
        mi_json_raw(&b, "{");
        for (size_t c = 0; c < rows->rows[r].count; c++) {
            if (c) mi_json_raw(&b, ",");
            mi_json_key(&b, rows->names[c]);
            mi_json_string(&b, rows->rows[r].values[c]);
        }
        mi_json_raw(&b, "}");
    }
    mi_json_raw(&b, "]");
    char *out = mi_strdup(mi_json_data(&b));
    mi_json_free(&b);
    return out;
}
