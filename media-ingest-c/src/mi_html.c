#define _GNU_SOURCE
#include "mi_html.h"
#include "mi_json.h"
#include "mi_util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Nodes                                                               */
/* ------------------------------------------------------------------ */

static mi_html_node *node_new(mi_html_type type) {
    mi_html_node *n = mi_calloc(1, sizeof *n);
    if (n) n->type = type;
    return n;
}

static void append_child(mi_html_node *parent, mi_html_node *child) {
    child->parent = parent;
    child->prev_sibling = parent->last_child;
    if (parent->last_child) parent->last_child->next_sibling = child;
    else parent->first_child = child;
    parent->last_child = child;
}

void mi_html_free(mi_html_node *node) {
    if (!node) return;
    mi_html_node *c = node->first_child;
    while (c) {
        mi_html_node *next = c->next_sibling;
        mi_html_free(c);
        c = next;
    }
    free(node->name);
    free(node->text);
    for (size_t i = 0; i < node->attr_count; i++) {
        free(node->attr_names[i]);
        free(node->attr_values[i]);
    }
    free(node->attr_names);
    free(node->attr_values);
    free(node);
}

const char *mi_html_attr(const mi_html_node *el, const char *name) {
    if (!el || el->type != MI_HTML_ELEMENT || !name) return NULL;
    for (size_t i = 0; i < el->attr_count; i++) {
        if (!strcasecmp(el->attr_names[i], name)) return el->attr_values[i];
    }
    return NULL;
}

int mi_html_is(const mi_html_node *n, const char *tag) {
    return n && n->type == MI_HTML_ELEMENT && n->name && !strcasecmp(n->name, tag);
}

static void set_attr(mi_html_node *el, const char *name, size_t nlen, const char *val,
                     size_t vlen) {
    char *n = mi_strndup(name, nlen);
    char *v = mi_strndup(val, vlen);
    if (!n || !v) { free(n); free(v); return; }
    /* A duplicate attribute is dropped; a browser keeps the first. */
    for (size_t i = 0; i < el->attr_count; i++) {
        if (!strcasecmp(el->attr_names[i], n)) { free(n); free(v); return; }
    }
    char **gn = mi_realloc(el->attr_names, (el->attr_count + 1) * sizeof *gn);
    char **gv = mi_realloc(el->attr_values, (el->attr_count + 1) * sizeof *gv);
    if (!gn || !gv) { free(n); free(v); free(gn); free(gv); return; }
    el->attr_names = gn;
    el->attr_values = gv;
    el->attr_names[el->attr_count] = n;
    el->attr_values[el->attr_count] = v;
    el->attr_count++;
}

/* ------------------------------------------------------------------ */
/* Entities                                                            */
/* ------------------------------------------------------------------ */

static int named_entity(const char *s, size_t n, unsigned codepoint) {
    struct { const char *name; unsigned cp; } TABLE[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
        {"nbsp", 0xA0}, {"copy", 0xA9}, {"reg", 0xAE}, {"hellip", 0x2026},
        {"mdash", 0x2014}, {"ndash", 0x2013}, {"rsquo", 0x2019}, {"lsquo", 0x2018},
        {"rdquo", 0x201D}, {"ldquo", 0x201C}, {"middot", 0xB7}, {"bull", 0x2022},
        {"euro", 0x20AC}, {"pound", 0xA3}, {"trade", 0x2122},
    };
    (void)codepoint;
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) {
        if (strlen(TABLE[i].name) == n && !strncasecmp(TABLE[i].name, s, n))
            return (int)TABLE[i].cp;
    }
    return -1;
}

/* Append `cp` as UTF-8. */
static void append_utf8(char *out, size_t *len, unsigned cp) {
    if (cp < 0x80) {
        out[(*len)++] = (char)cp;
    } else if (cp < 0x800) {
        out[(*len)++] = (char)(0xC0 | (cp >> 6));
        out[(*len)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out[(*len)++] = (char)(0xE0 | (cp >> 12));
        out[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*len)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[(*len)++] = (char)(0xF0 | (cp >> 18));
        out[(*len)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*len)++] = (char)(0x80 | (cp & 0x3F));
    }
}

/* Decode character references in `s` (length `n`) into a new string. */
static char *decode_entities(const char *s, size_t n) {
    char *out = mi_malloc(n * 4 + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') { out[o++] = s[i]; continue; }
        const char *semi = memchr(s + i, ';', n - i);
        if (!semi || (size_t)(semi - (s + i)) > 10) { out[o++] = s[i]; continue; }
        size_t elen = (size_t)(semi - (s + i + 1));
        unsigned cp = 0;
        int ok = 0;
        if (elen > 1 && s[i + 1] == '#') {
            if (s[i + 2] == 'x' || s[i + 2] == 'X') {
                char tmp[16]; size_t k = 0;
                for (size_t j = i + 3; j < (size_t)(semi - s) && k < 15; j++) tmp[k++] = s[j];
                tmp[k] = 0;
                if (k) { cp = (unsigned)strtoul(tmp, NULL, 16); ok = 1; }
            } else {
                char tmp[16]; size_t k = 0;
                for (size_t j = i + 2; j < (size_t)(semi - s) && k < 15; j++) tmp[k++] = s[j];
                tmp[k] = 0;
                if (k) { cp = (unsigned)strtoul(tmp, NULL, 10); ok = 1; }
            }
        } else {
            int e = named_entity(s + i + 1, elen, 0);
            if (e >= 0) { cp = (unsigned)e; ok = 1; }
        }
        if (ok && cp) {
            append_utf8(out, &o, cp);
            i = (size_t)(semi - s);
        } else {
            out[o++] = s[i];
        }
    }
    out[o] = 0;
    return out;
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

/* Elements that never have children. */
static int is_void(const char *name) {
    static const char *VOID[] = {"area", "base", "br", "col", "embed", "hr", "img",
                                 "input", "link", "meta", "param", "source", "track",
                                 "wbr", NULL};
    for (size_t i = 0; VOID[i]; i++) if (!strcasecmp(name, VOID[i])) return 1;
    return 0;
}

/* Elements whose content is raw text, not markup. */
static int is_raw_text(const char *name) {
    return !strcasecmp(name, "script") || !strcasecmp(name, "style") ||
           !strcasecmp(name, "textarea") || !strcasecmp(name, "title");
}

/* Block-level elements that implicitly close an open <p>. */
static int closes_p(const char *name) {
    static const char *BLOCK[] = {"address", "article", "aside", "blockquote",
        "details", "div", "dl", "fieldset", "figcaption", "figure", "footer",
        "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hr", "main",
        "nav", "ol", "p", "pre", "section", "table", "ul", NULL};
    for (size_t i = 0; BLOCK[i]; i++) if (!strcasecmp(name, BLOCK[i])) return 1;
    return 0;
}

/* Whether opening `child` implies closing an open `open` of the same family.
 * This is what lets a page with no end tags still nest correctly. */
static int implicitly_closes(const char *open, const char *child) {
    if (!open || !child) return 0;
    if (!strcasecmp(child, "li") && !strcasecmp(open, "li")) return 1;
    if (!strcasecmp(child, "p") && !strcasecmp(open, "p")) return 1;
    if ((!strcasecmp(child, "td") || !strcasecmp(child, "th")) &&
        (!strcasecmp(open, "td") || !strcasecmp(open, "th"))) return 1;
    if (!strcasecmp(child, "tr") && !strcasecmp(open, "tr")) return 1;
    if ((!strcasecmp(child, "option") && !strcasecmp(open, "option"))) return 1;
    if (!strcasecmp(child, "dd") && !strcasecmp(open, "dd")) return 1;
    if (!strcasecmp(child, "dt") && !strcasecmp(open, "dt")) return 1;
    if (closes_p(child) && !strcasecmp(open, "p")) return 1;
    return 0;
}

/* Skip whitespace. */
static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* Read a tag name, lowercased. */
static char *read_name(const char **pp, const char *end) {
    const char *p = *pp;
    const char *start = p;
    while (p < end && (isalnum((unsigned char)*p) || *p == '-' || *p == ':' ||
                       *p == '_' || *p == '.')) p++;
    char *name = mi_strndup(start, (size_t)(p - start));
    *pp = p;
    if (name) for (char *q = name; *q; q++) *q = (char)tolower((unsigned char)*q);
    return name;
}

mi_html_node *mi_html_parse(const char *html, size_t len) {
    mi_html_node *doc = node_new(MI_HTML_DOCUMENT);
    if (!doc) return NULL;

    const char *p = html;
    const char *end = html + len;
    mi_html_node *cur = doc;

    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt) {
            /* Trailing text. */
            const char *t = skip_ws(p, end);
            if (t < end) {
                mi_html_node *n = node_new(MI_HTML_TEXT);
                n->text = decode_entities(p, (size_t)(end - p));
                append_child(cur, n);
            }
            break;
        }
        /* Text before the tag. */
        if (lt > p) {
            mi_html_node *n = node_new(MI_HTML_TEXT);
            n->text = decode_entities(p, (size_t)(lt - p));
            append_child(cur, n);
        }

        if (lt + 1 < end && lt[1] == '!') {
            /* Comment or doctype. */
            const char *gt = memchr(lt, '>', (size_t)(end - lt));
            if (!gt) break;
            mi_html_node *n;
            if (lt + 2 < end && lt[2] == '-' && lt + 3 < end && lt[3] == '-') {
                n = node_new(MI_HTML_COMMENT);
                n->text = mi_strndup(lt + 4, (size_t)(gt - (lt + 4)));
            } else {
                n = node_new(MI_HTML_DOCTYPE);
                n->text = mi_strndup(lt + 2, (size_t)(gt - (lt + 2)));
            }
            append_child(cur, n);
            p = gt + 1;
            continue;
        }
        if (lt + 1 < end && lt[1] == '?') {
            /* Processing instruction; skip to '>'. */
            const char *gt = memchr(lt, '>', (size_t)(end - lt));
            p = gt ? gt + 1 : end;
            continue;
        }

        /* A close tag. */
        if (lt + 1 < end && lt[1] == '/') {
            const char *q = lt + 2;
            char *name = read_name(&q, end);
            const char *gt = memchr(q, '>', (size_t)(end - q));
            /* Pop until the matching open, tolerating an unmatched close. */
            mi_html_node *t = cur;
            while (t && t->type == MI_HTML_ELEMENT &&
                   (!name || strcasecmp(t->name, name))) {
                t = t->parent;
            }
            if (t && t->type == MI_HTML_ELEMENT) cur = t->parent;
            free(name);
            p = gt ? gt + 1 : end;
            continue;
        }

        /* An open tag. */
        const char *q = lt + 1;
        char *name = read_name(&q, end);
        if (!name || !*name) { free(name); p = lt + 1; continue; }

        mi_html_node *el = node_new(MI_HTML_ELEMENT);
        el->name = name;

        /* Attributes. */
        int self_close = 0;
        const char *gt = NULL;
        while (q < end) {
            q = skip_ws(q, end);
            if (q >= end) break;
            if (*q == '>') { gt = q; break; }
            if (*q == '/' && q + 1 < end && q[1] == '>') { self_close = 1; gt = q + 1; break; }
            const char *an = q;
            while (q < end && !isspace((unsigned char)*q) && *q != '=' && *q != '>' && *q != '/') q++;
            size_t alen = (size_t)(q - an);
            q = skip_ws(q, end);
            if (q < end && *q == '=') {
                q++;
                q = skip_ws(q, end);
                if (q < end && (*q == '"' || *q == '\'')) {
                    char quote = *q++;
                    const char *vs = q;
                    while (q < end && *q != quote) q++;
                    set_attr(el, an, alen, vs, (size_t)(q - vs));
                    if (q < end) q++;
                } else {
                    const char *vs = q;
                    while (q < end && !isspace((unsigned char)*q) && *q != '>') q++;
                    set_attr(el, an, alen, vs, (size_t)(q - vs));
                }
            } else if (alen > 0) {
                /* A valueless attribute. */
                set_attr(el, an, alen, "", 0);
            }
        }
        if (!gt) gt = end;

        /* Implicit close. */
        while (cur->type == MI_HTML_ELEMENT && implicitly_closes(cur->name, name))
            cur = cur->parent;

        append_child(cur, el);

        if (is_raw_text(name) && !self_close && !is_void(name)) {
            /* The content runs to the matching close tag, with no markup inside. */
            char close[64];
            snprintf(close, sizeof close, "</%s", name);
            const char *cs = gt + 1;
            const char *found = NULL;
            for (const char *r = cs; r + strlen(close) <= end; r++) {
                if (strncasecmp(r, close, strlen(close)) == 0) { found = r; break; }
            }
            const char *ce = found ? found : end;
            if (ce > cs) {
                mi_html_node *tn = node_new(MI_HTML_TEXT);
                tn->text = mi_strndup(cs, (size_t)(ce - cs));
                append_child(el, tn);
            }
            if (found) {
                const char *cg = memchr(found, '>', (size_t)(end - found));
                p = cg ? cg + 1 : end;
            } else {
                p = end;
            }
            continue;
        }

        if (!self_close && !is_void(name)) cur = el;
        p = gt + 1;
    }

    return doc;
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

static void append_text(mi_json_buf *b, const char *s) {
    if (s) mi_json_raw(b, s);
}

/* Collapse whitespace and trim, into a buffer. */
static void collapse(mi_json_buf *b) {
    char *s = b->data;
    if (!s) return;
    size_t o = 0;
    int pending_space = 0;
    for (size_t i = 0; s[i]; i++) {
        if (isspace((unsigned char)s[i])) {
            if (o > 0) pending_space = 1;
            continue;
        }
        if (pending_space) { s[o++] = ' '; pending_space = 0; }
        s[o++] = s[i];
    }
    s[o] = 0;
}

static void collect_text(const mi_html_node *n, mi_json_buf *b) {
    for (const mi_html_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->type == MI_HTML_TEXT) append_text(b, c->text);
        else if (c->type == MI_HTML_ELEMENT) collect_text(c, b);
    }
}

char *mi_html_text(const mi_html_node *el) {
    if (!el) return NULL;
    mi_json_buf b;
    mi_json_init(&b);
    if (el->type == MI_HTML_TEXT) append_text(&b, el->text);
    else collect_text(el, &b);
    collapse(&b);
    char *out = mi_strdup(mi_json_data(&b));
    mi_json_free(&b);
    return out;
}

char *mi_html_own_text(const mi_html_node *el) {
    if (!el) return NULL;
    mi_json_buf b;
    mi_json_init(&b);
    for (const mi_html_node *c = el->first_child; c; c = c->next_sibling) {
        if (c->type == MI_HTML_TEXT) append_text(&b, c->text);
    }
    collapse(&b);
    char *out = mi_strdup(mi_json_data(&b));
    mi_json_free(&b);
    return out;
}

/* ------------------------------------------------------------------ */
/* Iteration                                                           */
/* ------------------------------------------------------------------ */

mi_html_node *mi_html_next(const mi_html_node *root, const mi_html_node *n) {
    if (!n) return NULL;
    if (n->first_child) return n->first_child;
    const mi_html_node *cur = n;
    while (cur && cur != root) {
        if (cur->next_sibling) return cur->next_sibling;
        cur = cur->parent;
    }
    return NULL;
}
