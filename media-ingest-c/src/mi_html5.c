#define _GNU_SOURCE
#include "mi_html5.h"
#include "mi_json.h"
#include "mi_util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/*
 * A pragmatic, spec-shaped implementation of the HTML5 tree construction
 * algorithm. It follows the Standard's structure — tokenizer states, insertion
 * modes, the stack of open elements, the list of active formatting elements, the
 * adoption agency algorithm, and foster parenting — over the cases real pages
 * exercise, rather than every corner of the spec. The cases that are implemented
 * are implemented the way the spec says, which is what makes a mis-nested table
 * or an unclosed <b> come out the way a browser nests it.
 */

/* ------------------------------------------------------------------ */
/* Tokenizer                                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    TOK_CHAR,     /* one character */
    TOK_START,    /* <name attrs> */
    TOK_END,      /* </name> */
    TOK_COMMENT,
    TOK_DOCTYPE,
    TOK_EOF,
} tok_type;

typedef struct {
    tok_type type;
    char *name;         /* lowercased */
    char *data;         /* comment/doctype data, or a character */
    /* Attributes, lowercased names. */
    char **attr_names;
    char **attr_values;
    size_t attr_count;
    int self_closing;
    /* For TOK_DOCTYPE: the name and whether it is quirks. */
    char *doctype_name;
    int force_quirks;
} token;

static void token_free(token *t) {
    free(t->name);
    free(t->data);
    free(t->doctype_name);
    for (size_t i = 0; i < t->attr_count; i++) {
        free(t->attr_names[i]);
        free(t->attr_values[i]);
    }
    free(t->attr_names);
    free(t->attr_values);
    memset(t, 0, sizeof *t);
}

typedef struct {
    const char *p;
    const char *end;
    token *pending; /* a queued token (for a character run) */
    int has_pending;
} tokenizer;

/* Named character references, the ones that appear in real text. */
static int named_ref(const char *s, size_t n, unsigned *cp) {
    struct { const char *name; unsigned cp; } T[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
        {"nbsp", 0xA0}, {"copy", 0xA9}, {"reg", 0xAE}, {"hellip", 0x2026},
        {"mdash", 0x2014}, {"ndash", 0x2013}, {"rsquo", 0x2019}, {"lsquo", 0x2018},
        {"rdquo", 0x201D}, {"ldquo", 0x201C}, {"middot", 0xB7}, {"bull", 0x2022},
        {"euro", 0x20AC}, {"pound", 0xA3}, {"trade", 0x2122}, {"times", 0xD7},
        {"divide", 0xF7}, {"deg", 0xB0}, {"plusmn", 0xB1}, {"frac12", 0xBD},
        {"szlig", 0xDF}, {"agrave", 0xE0}, {"eacute", 0xE9}, {"auml", 0xE4},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        if (strlen(T[i].name) == n && !strncasecmp(T[i].name, s, n)) {
            *cp = T[i].cp;
            return 1;
        }
    }
    return 0;
}

static void append_utf8(char *out, size_t *len, unsigned cp) {
    if (cp < 0x80) out[(*len)++] = (char)cp;
    else if (cp < 0x800) {
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

/* Decode a character reference starting at `p` (which points at '&').
 * Advances *p past the reference. Returns the codepoint, or 0 if it is not a
 * reference (and *p is left at the '&'). */
static unsigned consume_char_ref(const char **pp, const char *end) {
    const char *p = *pp;
    if (p + 1 >= end) return 0;
    if (p[1] == '#') {
        const char *q = p + 2;
        unsigned cp = 0;
        int hex = 0;
        if (q < end && (*q == 'x' || *q == 'X')) { hex = 1; q++; }
        const char *start = q;
        while (q < end && q - start < 8 &&
               (hex ? isxdigit((unsigned char)*q) : isdigit((unsigned char)*q))) {
            cp = cp * (hex ? 16 : 10) +
                 (unsigned)(hex ? (isdigit((unsigned char)*q) ? *q - '0'
                                                              : tolower((unsigned char)*q) - 'a' + 10)
                                : *q - '0');
            q++;
        }
        if (q == start || q >= end || *q != ';') return 0;
        *pp = q + 1;
        return cp;
    }
    /* Named. Find the ';'. */
    const char *q = p + 1;
    while (q < end && q - (p + 1) < 32 && *q != ';' && *q != '&' && *q != '<') q++;
    if (q >= end || *q != ';') return 0;
    unsigned cp = 0;
    if (named_ref(p + 1, (size_t)(q - (p + 1)), &cp)) {
        *pp = q + 1;
        return cp;
    }
    return 0;
}

/* Read a tag name / attribute name (anything that is not whitespace or a
 * delimiter), lowercased. */
static char *read_tag_name(const char **pp, const char *end) {
    const char *p = *pp;
    const char *start = p;
    while (p < end && !isspace((unsigned char)*p) && *p != '>' && *p != '/' &&
           *p != '=') p++;
    char *s = mi_strndup(start, (size_t)(p - start));
    *pp = p;
    if (s) for (char *q = s; *q; q++) *q = (char)tolower((unsigned char)*q);
    return s;
}

/* RCDATA/RAWTEXT: read until the matching close tag, decoding references in
 * RCDATA only. */
static char *read_raw_text(const char **pp, const char *end, const char *name,
                           int decode) {
    char close[64];
    snprintf(close, sizeof close, "</%s", name);
    size_t clen = strlen(close);
    const char *p = *pp;
    const char *found = NULL;
    for (const char *r = p; r + clen <= end; r++) {
        if (strncasecmp(r, close, clen) == 0 &&
            (r + clen == end || r[clen] == '>' || isspace((unsigned char)r[clen]) ||
             r[clen] == '/')) {
            found = r;
            break;
        }
    }
    const char *e = found ? found : end;
    char *out = mi_malloc((size_t)(e - p) * 4 + 1);
    if (!out) return NULL;
    size_t o = 0;
    if (decode) {
        const char *q = p;
        while (q < e) {
            if (*q == '&') {
                const char *save = q;
                unsigned cp = consume_char_ref(&q, e);
                if (cp) { append_utf8(out, &o, cp); continue; }
                q = save;
            }
            out[o++] = *q++;
        }
    } else {
        memcpy(out, p, (size_t)(e - p));
        o = (size_t)(e - p);
    }
    out[o] = 0;
    if (found) {
        const char *gt = memchr(found, '>', (size_t)(end - found));
        *pp = gt ? gt + 1 : end;
    } else {
        *pp = end;
    }
    return out;
}

/* Whether an element's content is RCDATA or RAWTEXT. */
static int is_rawtext(const char *name, int scripting, int *decode) {
    *decode = 0;
    if (!strcasecmp(name, "script") || !strcasecmp(name, "style") ||
        !strcasecmp(name, "xmp") || !strcasecmp(name, "iframe") ||
        !strcasecmp(name, "noembed") || !strcasecmp(name, "noframes") ||
        !strcasecmp(name, "plaintext")) return 1;
    if (!strcasecmp(name, "title") || !strcasecmp(name, "textarea")) {
        *decode = 1;
        return 1;
    }
    if (!strcasecmp(name, "noscript") && scripting) return 1;
    return 0;
}

/* The next token. The tokenizer returns one token at a time; a character run
 * becomes one TOK_CHAR per character, which is what the tree builder expects
 * (it buffers its own runs). */
static void next_token(tokenizer *tz, token *t) {
    memset(t, 0, sizeof *t);
    if (tz->has_pending) {
        *t = *tz->pending;
        tz->has_pending = 0;
        return;
    }
    const char *p = tz->p;
    const char *end = tz->end;
    if (p >= end) { t->type = TOK_EOF; return; }

    if (*p != '<') {
        if (*p == '&') {
            const char *save = p;
            unsigned cp = consume_char_ref(&p, end);
            if (cp) {
                char buf[8];
                size_t n = 0;
                append_utf8(buf, &n, cp);
                buf[n] = 0;
                t->type = TOK_CHAR;
                t->data = mi_strdup(buf);
                tz->p = p;
                return;
            }
            p = save;
        }
        t->type = TOK_CHAR;
        t->data = mi_strndup(p, 1);
        tz->p = p + 1;
        return;
    }

    /* '<' */
    if (p + 1 >= end) { t->type = TOK_CHAR; t->data = mi_strdup("<"); tz->p = end; return; }
    if (p[1] == '!') {
        if (p + 3 < end && p[2] == '-' && p[3] == '-') {
            const char *close = NULL;
            for (const char *r = p + 4; r + 2 < end; r++) {
                if (r[0] == '-' && r[1] == '-' && r[2] == '>') { close = r; break; }
            }
            const char *ce = close ? close : end;
            t->type = TOK_COMMENT;
            t->data = mi_strndup(p + 4, (size_t)(ce - (p + 4)));
            tz->p = close ? close + 3 : end;
            return;
        }
        if (p + 8 < end && !strncasecmp(p + 2, "doctype", 7)) {
            const char *gt = memchr(p, '>', (size_t)(end - p));
            const char *de = gt ? gt : end;
            t->type = TOK_DOCTYPE;
            const char *s = p + 9;
            while (s < de && isspace((unsigned char)*s)) s++;
            const char *ns = s;
            while (s < de && !isspace((unsigned char)*s) && *s != '>') s++;
            t->doctype_name = mi_strndup(ns, (size_t)(s - ns));
            for (char *q = t->doctype_name; q && *q; q++) *q = (char)tolower((unsigned char)*q);
            tz->p = gt ? gt + 1 : end;
            return;
        }
        /* A bogus comment. */
        const char *gt = memchr(p, '>', (size_t)(end - p));
        const char *ce = gt ? gt : end;
        t->type = TOK_COMMENT;
        t->data = mi_strndup(p + 2, (size_t)(ce - (p + 2)));
        tz->p = gt ? gt + 1 : end;
        return;
    }
    if (p[1] == '?') {
        const char *gt = memchr(p, '>', (size_t)(end - p));
        tz->p = gt ? gt + 1 : end;
        /* A bogus comment, but the tree builder ignores it; emit as comment. */
        t->type = TOK_COMMENT;
        t->data = mi_strndup(p + 2, (size_t)((gt ? gt : end) - (p + 2)));
        return;
    }
    if (p[1] == '/') {
        if (p + 2 >= end || p[2] == '>') {
            /* "</>" is a bogus comment the tree builder drops; it is not EOF,
             * and treating it as EOF truncated the rest of the document. */
            tz->p = p + 2;
            t->type = TOK_COMMENT;
            t->data = mi_strdup("");
            return;
        }
        const char *q = p + 2;
        char *name = read_tag_name(&q, end);
        const char *gt = memchr(q, '>', (size_t)(end - q));
        t->type = TOK_END;
        t->name = name ? name : mi_strdup("");
        tz->p = gt ? gt + 1 : end;
        return;
    }
    /* A start tag. */
    {
        const char *q = p + 1;
        char *name = read_tag_name(&q, end);
        if (!name || !*name) {
            /* "<" followed by a non-letter: a literal '<'. */
            free(name);
            t->type = TOK_CHAR;
            t->data = mi_strdup("<");
            tz->p = p + 1;
            return;
        }
        t->type = TOK_START;
        t->name = name;
        size_t acap = 4;
        t->attr_names = mi_calloc(acap, sizeof *t->attr_names);
        t->attr_values = mi_calloc(acap, sizeof *t->attr_values);
        int self_closing = 0;
        while (q < end) {
            while (q < end && isspace((unsigned char)*q)) q++;
            if (q >= end) break;
            if (*q == '>') { q++; break; }
            if (*q == '/' && q + 1 < end && q[1] == '>') { self_closing = 1; q += 2; break; }
            if (*q == '/') { q++; continue; }
            const char *ans = q;
            while (q < end && !isspace((unsigned char)*q) && *q != '=' && *q != '>' && *q != '/') q++;
            size_t anl = (size_t)(q - ans);
            while (q < end && isspace((unsigned char)*q)) q++;
            char *value = NULL;
            if (q < end && *q == '=') {
                q++;
                while (q < end && isspace((unsigned char)*q)) q++;
                if (q < end && (*q == '"' || *q == '\'')) {
                    char quote = *q++;
                    const char *vs = q;
                    while (q < end && *q != quote) q++;
                    value = mi_strndup(vs, (size_t)(q - vs));
                    if (q < end) q++;
                } else {
                    const char *vs = q;
                    while (q < end && !isspace((unsigned char)*q) && *q != '>') q++;
                    value = mi_strndup(vs, (size_t)(q - vs));
                }
            } else {
                value = mi_strdup("");
            }
            /* Decode references in the value. */
            if (value && strchr(value, '&')) {
                char *out = mi_malloc(strlen(value) * 4 + 1);
                size_t o = 0;
                const char *vp = value;
                while (*vp) {
                    if (*vp == '&') {
                        const char *save = vp;
                        unsigned cp = consume_char_ref(&vp, value + strlen(value));
                        if (cp) { append_utf8(out, &o, cp); continue; }
                        vp = save;
                    }
                    out[o++] = *vp++;
                }
                out[o] = 0;
                free(value);
                value = out;
            }
            /* Duplicate attributes: the first wins. */
            int dup = 0;
            for (size_t i = 0; i < t->attr_count; i++) {
                if (strlen(t->attr_names[i]) == anl && !strncasecmp(t->attr_names[i], ans, anl)) {
                    dup = 1;
                    break;
                }
            }
            if (!dup && anl > 0) {
                if (t->attr_count == acap) {
                    acap *= 2;
                    t->attr_names = mi_realloc(t->attr_names, acap * sizeof *t->attr_names);
                    t->attr_values = mi_realloc(t->attr_values, acap * sizeof *t->attr_values);
                }
                char *an = mi_strndup(ans, anl);
                for (char *z = an; z && *z; z++) *z = (char)tolower((unsigned char)*z);
                t->attr_names[t->attr_count] = an;
                t->attr_values[t->attr_count] = value ? value : mi_strdup("");
                t->attr_count++;
                value = NULL;
            }
            free(value);
        }
        t->self_closing = self_closing;
        tz->p = q;
    }
}

/* ------------------------------------------------------------------ */
/* Tree builder                                                        */
/* ------------------------------------------------------------------ */

/* The insertion modes the builder uses. */
typedef enum {
    IM_INITIAL,
    IM_BEFORE_HTML,
    IM_BEFORE_HEAD,
    IM_IN_HEAD,
    IM_AFTER_HEAD,
    IM_IN_BODY,
    IM_TEXT,
    IM_IN_TABLE,
    IM_IN_TABLE_TEXT,
    IM_IN_CAPTION,
    IM_IN_COLUMN_GROUP,
    IM_IN_TABLE_BODY,
    IM_IN_ROW,
    IM_IN_CELL,
    IM_IN_SELECT,
    IM_IN_SELECT_IN_TABLE,
    IM_AFTER_BODY,
    IM_IN_FRAMESET,
    IM_AFTER_FRAMESET,
    IM_AFTER_AFTER_BODY,
} insertion_mode;

typedef struct {
    mi_html_node *node;
    char *name; /* cached lowercased name */
} open_el;

typedef struct {
    mi_html_node *element;
    token saved; /* the token to re-run if reconstructed */
    int has_saved;
} fmt_el;

typedef struct {
    mi_html_node *doc;
    mi_html_node *html;
    mi_html_node *head;
    mi_html_node *body;
    insertion_mode mode;
    int original_mode_valid;
    insertion_mode original_mode;

    open_el *stack;
    size_t stack_n, stack_cap;

    fmt_el *afe;
    size_t afe_n, afe_cap;

    /* A pending table character run, and where to put it. */
    mi_json_buf table_text;
    int table_text_pending;

    /* The frameset flag, the head element pointer, and the form element
     * pointer, per the spec. */
    int frameset_ok;
    mi_html_node *form;
    int quirks;
    int scripting;
    int max_depth;
} builder;

/* ---- stack helpers ---- */

static const char *stack_name(const open_el *e) { return e->name; }

static void stack_push(builder *b, mi_html_node *node) {
    if ((int)b->stack_n >= b->max_depth) return;
    if (b->stack_n == b->stack_cap) {
        b->stack_cap = b->stack_cap ? b->stack_cap * 2 : 32;
        b->stack = mi_realloc(b->stack, b->stack_cap * sizeof *b->stack);
    }
    b->stack[b->stack_n].node = node;
    b->stack[b->stack_n].name = node->name ? mi_strdup(node->name) : mi_strdup("");
    b->stack_n++;
}

static mi_html_node *current_node(builder *b) {
    return b->stack_n ? b->stack[b->stack_n - 1].node : b->doc;
}


/* Pop until `name` is popped (inclusive), or the stack empties. */
static void pop_until(builder *b, const char *name) {
    while (b->stack_n) {
        open_el e = b->stack[--b->stack_n];
        int match = !strcasecmp(e.name, name);
        free(e.name);
        if (match) break;
    }
}

static void pop_one(builder *b) {
    if (b->stack_n) free(b->stack[--b->stack_n].name);
}

/* ---- node creation ---- */

static mi_html_node *make_element(const token *t) {
    mi_html_node *n = mi_calloc(1, sizeof *n);
    if (!n) return NULL;
    n->type = MI_HTML_ELEMENT;
    n->name = mi_strdup(t->name);
    if (t->attr_count) {
        n->attr_names = mi_calloc(t->attr_count, sizeof *n->attr_names);
        n->attr_values = mi_calloc(t->attr_count, sizeof *n->attr_values);
        for (size_t i = 0; i < t->attr_count; i++) {
            n->attr_names[i] = mi_strdup(t->attr_names[i]);
            n->attr_values[i] = mi_strdup(t->attr_values[i]);
        }
        n->attr_count = t->attr_count;
    }
    return n;
}

static mi_html_node *make_text(const char *s, size_t n) {
    mi_html_node *node = mi_calloc(1, sizeof *node);
    if (!node) return NULL;
    node->type = MI_HTML_TEXT;
    node->text = mi_strndup(s, n);
    return node;
}

static void append_child(mi_html_node *parent, mi_html_node *child) {
    child->parent = parent;
    child->prev_sibling = parent->last_child;
    if (parent->last_child) parent->last_child->next_sibling = child;
    else parent->first_child = child;
    parent->last_child = child;
}

static void insert_at(mi_html_node *parent, mi_html_node *child, mi_html_node *before) {
    if (!before) { append_child(parent, child); return; }
    child->parent = parent;
    child->next_sibling = before;
    child->prev_sibling = before->prev_sibling;
    if (before->prev_sibling) before->prev_sibling->next_sibling = child;
    else parent->first_child = child;
    before->prev_sibling = child;
}

/* ---- the list of active formatting elements ---- */

static void afe_push(builder *b, mi_html_node *el) {
    if (b->afe_n == b->afe_cap) {
        b->afe_cap = b->afe_cap ? b->afe_cap * 2 : 16;
        b->afe = mi_realloc(b->afe, b->afe_cap * sizeof *b->afe);
    }
    memset(&b->afe[b->afe_n], 0, sizeof b->afe[b->afe_n]);
    b->afe[b->afe_n].element = el;
    b->afe_n++;
}

static void afe_clear(builder *b) {
    for (size_t i = 0; i < b->afe_n; i++) token_free(&b->afe[i].saved);
    b->afe_n = 0;
}

/* Remove the entry at `i`, freeing its saved token. */
static void afe_remove(builder *b, size_t i) {
    if (i >= b->afe_n) return;
    token_free(&b->afe[i].saved);
    memmove(&b->afe[i], &b->afe[i + 1], (b->afe_n - i - 1) * sizeof b->afe[0]);
    b->afe_n--;
}

/* The formatting elements that can appear in the active list. */
static int is_formatting(const char *name) {
    static const char *F[] = {"a", "b", "big", "code", "em", "font", "i",
                              "nobr", "s", "small", "strike", "strong", "tt", "u", NULL};
    for (size_t i = 0; F[i]; i++) if (!strcasecmp(name, F[i])) return 1;
    return 0;
}

/* ---- foster parenting ---- */

/* Where a node goes when it appears inside a table but is not table content. */
static void insert_node(builder *b, mi_html_node *node, int foster);

static void foster_parent(builder *b, mi_html_node *node) {
    /*
     * The spec's foster-parenting rule: find the last <table> on the stack, and
     * insert before it. If there is a template or the table is the last element,
     * the node goes to the previous element instead.
     */
    size_t table_idx = (size_t)-1;
    for (size_t i = b->stack_n; i-- > 0;) {
        if (!strcasecmp(b->stack[i].name, "table")) { table_idx = i; break; }
    }
    if (table_idx == (size_t)-1) { append_child(current_node(b), node); return; }
    if (table_idx == 0) { append_child(b->stack[0].node, node); return; }
    mi_html_node *table = b->stack[table_idx].node;
    mi_html_node *parent = table->parent;
    if (!parent) { append_child(b->doc, node); return; }
    insert_at(parent, node, table);
}

static void insert_node(builder *b, mi_html_node *node, int foster) {
    if (foster) { foster_parent(b, node); return; }
    append_child(current_node(b), node);
}

/* Insert an element and push it on the stack. */
static mi_html_node *insert_element(builder *b, const token *t, int foster) {
    mi_html_node *n = make_element(t);
    if (!n) return NULL;
    insert_node(b, n, foster);
    stack_push(b, n);
    return n;
}

/* Insert a character (merging with a trailing text node, as a browser does so
 * that the DOM has one text node per run). */
static void insert_char(builder *b, const char *s, int foster) {
    mi_html_node *parent = foster ? NULL : current_node(b);
    if (foster) {
        /*
         * Foster-parented text: find or make a text node before the table, and
         * *merge with the previous foster-parented run* so a word does not become
         * one text node per character. A per-character node is what the DOM would
         * have if the merge were skipped, and it makes `mi_html_text` return the
         * same string but the tree wrong.
         */
        size_t table_idx = (size_t)-1;
        for (size_t i = b->stack_n; i-- > 0;) {
            if (!strcasecmp(b->stack[i].name, "table")) { table_idx = i; break; }
        }
        mi_html_node *parent2 = table_idx == (size_t)-1 ? current_node(b)
                               : (b->stack[table_idx].node->parent
                                      ? b->stack[table_idx].node->parent : b->doc);
        mi_html_node *before = table_idx == (size_t)-1 ? NULL : b->stack[table_idx].node;
        /* The last text node already placed just before the table, if any. */
        mi_html_node *prev = before ? before->prev_sibling : parent2->last_child;
        if (prev && prev->type == MI_HTML_TEXT) {
            char *merged = mi_asprintf("%s%s", prev->text ? prev->text : "", s);
            free(prev->text);
            prev->text = merged;
            return;
        }
        if (before) insert_at(parent2, make_text(s, strlen(s)), before);
        else append_child(parent2, make_text(s, strlen(s)));
        return;
    }
    mi_html_node *last = parent->last_child;
    if (last && last->type == MI_HTML_TEXT) {
        char *merged = mi_asprintf("%s%s", last->text ? last->text : "", s);
        free(last->text);
        last->text = merged;
        return;
    }
    append_child(parent, make_text(s, strlen(s)));
}

/* ------------------------------------------------------------------ */
/* Scope rules                                                         */
/* ------------------------------------------------------------------ */

static int is_scope_boundary(const char *name, int kind) {
    /* kind 0 = default scope, 1 = list item scope, 2 = button scope,
     * 3 = table scope, 4 = select scope. */
    static const char *DEFAULT[] = {"applet", "caption", "html", "table", "td",
                                    "th", "marquee", "object", "template", NULL};
    static const char *LIST[] = {"ol", "ul", NULL};
    static const char *TABLE[] = {"html", "table", "template", NULL};
    for (size_t i = 0; DEFAULT[i]; i++) if (!strcasecmp(name, DEFAULT[i])) return 1;
    if (kind == 1) for (size_t i = 0; LIST[i]; i++) if (!strcasecmp(name, LIST[i])) return 1;
    if (kind == 2) if (!strcasecmp(name, "button")) return 1;
    if (kind == 3) { for (size_t i = 0; TABLE[i]; i++) if (!strcasecmp(name, TABLE[i])) return 1; return 0; }
    if (kind == 4) {
        if (!strcasecmp(name, "optgroup") || !strcasecmp(name, "option")) return 0;
        return 1;
    }
    return 0;
}

static int in_scope(builder *b, const char *name, int kind) {
    for (size_t i = b->stack_n; i-- > 0;) {
        if (!strcasecmp(b->stack[i].name, name)) return 1;
        if (is_scope_boundary(b->stack[i].name, kind)) return 0;
    }
    return 0;
}

static int in_button_scope(builder *b, const char *name) {
    return in_scope(b, name, 2);
}

/* ---- the adoption agency algorithm ---- */

/* Find the last element in the active formatting list between `start` and the
 * end with this name; -1 if none. */
static int afe_find_last(builder *b, const char *name, size_t start) {
    for (size_t i = b->afe_n; i-- > start;) {
        if (b->afe[i].element && !strcasecmp(b->afe[i].element->name, name)) return (int)i;
    }
    return -1;
}

static int stack_index_of(builder *b, mi_html_node *node) {
    for (size_t i = 0; i < b->stack_n; i++)
        if (b->stack[i].node == node) return (int)i;
    return -1;
}

/*
 * The adoption agency algorithm, the spec's procedure for closing a formatting
 * element that is not the current node — `<b>a<i>b</b>c</i>` being the classic.
 * This is a faithful implementation of the steps that matter; the loop bound
 * (8) and the "if nothing changed, pop" step are the spec's.
 */
/* Detach a node from its parent, leaving its own subtree intact. */
static void detach(mi_html_node *node) {
    if (!node || !node->parent) return;
    mi_html_node *p = node->parent;
    if (node->prev_sibling) node->prev_sibling->next_sibling = node->next_sibling;
    else p->first_child = node->next_sibling;
    if (node->next_sibling) node->next_sibling->prev_sibling = node->prev_sibling;
    else p->last_child = node->prev_sibling;
    node->parent = NULL;
    node->prev_sibling = NULL;
    node->next_sibling = NULL;
}

/* A deep-enough clone of an element: the same name and attributes, no children.
 * The adoption agency needs a fresh element to reopen formatting, and the
 * children are moved into it separately. */
static mi_html_node *clone_element(const mi_html_node *src) {
    mi_html_node *n = mi_calloc(1, sizeof *n);
    if (!n) return NULL;
    n->type = MI_HTML_ELEMENT;
    n->name = mi_strdup(src->name);
    if (src->attr_count) {
        n->attr_names = mi_calloc(src->attr_count, sizeof *n->attr_names);
        n->attr_values = mi_calloc(src->attr_count, sizeof *n->attr_values);
        for (size_t i = 0; i < src->attr_count; i++) {
            n->attr_names[i] = mi_strdup(src->attr_names[i]);
            n->attr_values[i] = mi_strdup(src->attr_values[i]);
        }
        n->attr_count = src->attr_count;
    }
    return n;
}

static void adoption_agency(builder *b, const char *subject) {
    /* A small stack helper: insert a node at a position. */
    for (int outer = 0; outer < 8; outer++) {
        /* Find the last entry in the active list with the subject's name. */
        int fmt_idx = afe_find_last(b, subject, 0);
        if (fmt_idx < 0) {
            /* Not in the active list: act as "any other end tag". */
            for (size_t i = b->stack_n; i-- > 0;) {
                if (!strcasecmp(b->stack[i].name, subject)) { pop_until(b, subject); return; }
                if (is_scope_boundary(b->stack[i].name, 0)) return;
            }
            return;
        }
        mi_html_node *fmt = b->afe[fmt_idx].element;
        int stack_idx = stack_index_of(b, fmt);
        if (stack_idx < 0) { afe_remove(b, (size_t)fmt_idx); return; }
        if (!in_scope(b, subject, 0)) return;

        /* The furthest block: the topmost node below fmt that is special. */
        static const char *SPECIAL[] = {"address", "applet", "area", "article",
            "aside", "base", "basefont", "bgsound", "blockquote", "body", "br",
            "button", "caption", "center", "col", "colgroup", "dd", "details",
            "dir", "div", "dl", "dt", "embed", "fieldset", "figcaption", "figure",
            "footer", "form", "frame", "frameset", "h1", "h2", "h3", "h4", "h5",
            "h6", "head", "header", "hgroup", "hr", "html", "iframe", "img",
            "input", "keygen", "li", "link", "listing", "main", "marquee", "menu",
            "meta", "nav", "noembed", "noframes", "noscript", "object", "ol",
            "p", "param", "plaintext", "pre", "script", "section", "select",
            "source", "style", "summary", "table", "tbody", "td", "template",
            "textarea", "tfoot", "th", "thead", "title", "tr", "track", "ul",
            "wbr", "xmp", NULL};
        int furthest = -1;
        for (size_t i = (size_t)stack_idx + 1; i < b->stack_n; i++) {
            for (size_t j = 0; SPECIAL[j]; j++) {
                if (!strcasecmp(b->stack[i].name, SPECIAL[j])) { furthest = (int)i; break; }
            }
            if (furthest >= 0) break;
        }
        if (furthest < 0) { pop_until(b, subject); afe_remove(b, (size_t)fmt_idx); return; }

        mi_html_node *furthest_block = b->stack[furthest].node;
        mi_html_node *common = b->stack[stack_idx - 1].node;
        int bookmark = fmt_idx;

        /* Clone a node's element, for the inner loop's replace step. */
        #define CLONE_NODE(src) clone_element(src)
        mi_html_node *node = furthest_block;
        mi_html_node *last_node = furthest_block;
        int inner = 0;
        while (1) {
            inner++;
            int node_stack = stack_index_of(b, node);
            if (node_stack <= stack_idx) break; /* reached fmt */
            node = b->stack[node_stack - 1].node;
            if (node == fmt) break;
            int node_afe = -1;
            for (size_t i = 0; i < b->afe_n; i++)
                if (b->afe[i].element == node) { node_afe = (int)i; break; }
            if (inner > 3) {
                if (node_afe >= 0) afe_remove(b, (size_t)node_afe);
                /* Remove from the stack. */
                int ns = stack_index_of(b, node);
                if (ns >= 0) {
                    free(b->stack[ns].name);
                    memmove(&b->stack[ns], &b->stack[ns + 1],
                            (b->stack_n - ns - 1) * sizeof b->stack[0]);
                    b->stack_n--;
                }
                continue;
            }
            if (node_afe < 0) {
                int ns = stack_index_of(b, node);
                if (ns >= 0) {
                    free(b->stack[ns].name);
                    memmove(&b->stack[ns], &b->stack[ns + 1],
                            (b->stack_n - ns - 1) * sizeof b->stack[0]);
                    b->stack_n--;
                }
                continue;
            }
            /* Replace node with a fresh clone, in both the afe and the stack. */
            mi_html_node *fresh = CLONE_NODE(node);
            b->afe[node_afe].element = fresh;
            int ns = stack_index_of(b, node);
            if (ns >= 0) {
                b->stack[ns].node = fresh;
                free(b->stack[ns].name);
                b->stack[ns].name = mi_strdup(fresh->name);
            }
            if (last_node == furthest_block) bookmark = node_afe;
            /* Insert lastNode into node. */
            detach(last_node);
            append_child(fresh, last_node);
            last_node = fresh;
            node = fresh;
        }

        /* Insert lastNode at the appropriate place, using common as the target
         * (which is foster parenting when common is a table element). */
        detach(last_node);
        {
            int foster = !strcasecmp(common->name, "table") ||
                         !strcasecmp(common->name, "tbody") ||
                         !strcasecmp(common->name, "tfoot") ||
                         !strcasecmp(common->name, "thead") ||
                         !strcasecmp(common->name, "tr");
            if (foster) {
                /* Find the table in the stack and insert before it. */
                size_t table_idx = (size_t)-1;
                for (size_t i = b->stack_n; i-- > 0;)
                    if (!strcasecmp(b->stack[i].name, "table")) { table_idx = i; break; }
                if (table_idx != (size_t)-1 && b->stack[table_idx].node->parent) {
                    insert_at(b->stack[table_idx].node->parent, last_node,
                              b->stack[table_idx].node);
                } else {
                    append_child(common, last_node);
                }
            } else {
                append_child(common, last_node);
            }
        }

        /* Create a fresh clone of the formatting element, move the furthest
         * block's children into it, and put it inside the furthest block. */
        mi_html_node *new_fmt = CLONE_NODE(fmt);
        {
            mi_html_node *c = furthest_block->first_child;
            while (c) {
                mi_html_node *next = c->next_sibling;
                detach(c);
                append_child(new_fmt, c);
                c = next;
            }
        }
        append_child(furthest_block, new_fmt);

        /* The stack: remove fmt, insert new_fmt immediately below furthest. */
        int fi = stack_index_of(b, fmt);
        if (fi >= 0) {
            free(b->stack[fi].name);
            memmove(&b->stack[fi], &b->stack[fi + 1],
                    (b->stack_n - fi - 1) * sizeof b->stack[0]);
            b->stack_n--;
        }
        int fbi = stack_index_of(b, furthest_block);
        if (fbi >= 0) {
            /* Insert after fbi. */
            if (b->stack_n == b->stack_cap) {
                b->stack_cap = b->stack_cap ? b->stack_cap * 2 : 32;
                b->stack = mi_realloc(b->stack, b->stack_cap * sizeof *b->stack);
            }
            memmove(&b->stack[fbi + 2], &b->stack[fbi + 1],
                    (b->stack_n - fbi - 1) * sizeof b->stack[0]);
            b->stack[fbi + 1].node = new_fmt;
            b->stack[fbi + 1].name = mi_strdup(new_fmt->name);
            b->stack_n++;
        } else {
            stack_push(b, new_fmt);
        }

        /* The active list: remove fmt, insert new_fmt at the bookmark. */
        afe_remove(b, (size_t)fmt_idx);
        if (bookmark > (int)b->afe_n) bookmark = (int)b->afe_n;
        if (b->afe_n == b->afe_cap) {
            b->afe_cap = b->afe_cap ? b->afe_cap * 2 : 16;
            b->afe = mi_realloc(b->afe, b->afe_cap * sizeof *b->afe);
        }
        memmove(&b->afe[bookmark + 1], &b->afe[bookmark],
                (b->afe_n - (size_t)bookmark) * sizeof b->afe[0]);
        memset(&b->afe[bookmark], 0, sizeof b->afe[bookmark]);
        b->afe[bookmark].element = new_fmt;
        b->afe_n++;
        #undef CLONE_NODE
    }
}

/* ---- character runs in a table ---- */

static void flush_table_text(builder *b) {
    if (!b->table_text_pending) return;
    const char *text = mi_json_data(&b->table_text);
    /* Any non-whitespace makes the run foster-parented. */
    int all_ws = 1;
    for (const char *p = text; *p; p++) if (!isspace((unsigned char)*p)) { all_ws = 0; break; }
    if (!all_ws) {
        insert_node(b, make_text(text, strlen(text)), 1);
    } else if (b->stack_n) {
        mi_html_node *cur = current_node(b);
        if (cur->last_child && cur->last_child->type == MI_HTML_TEXT) {
            char *m = mi_asprintf("%s%s", cur->last_child->text, text);
            free(cur->last_child->text);
            cur->last_child->text = m;
        } else {
            append_child(cur, make_text(text, strlen(text)));
        }
    }
    mi_json_free(&b->table_text);
    mi_json_init(&b->table_text);
    b->table_text_pending = 0;
}

/* ------------------------------------------------------------------ */
/* The dispatch                                                        */
/* ------------------------------------------------------------------ */

/* Forward declaration: the tree builder. */
static void process_token(builder *b, token *t);

/* Insert a character in the current insertion mode's way. */
static void insert_char_for_mode(builder *b, const char *s) {
    if (b->mode == IM_IN_TABLE || b->mode == IM_IN_TABLE_BODY ||
        b->mode == IM_IN_ROW || b->mode == IM_IN_CELL ||
        b->mode == IM_IN_CAPTION || b->mode == IM_IN_COLUMN_GROUP) {
        b->table_text_pending = 1;
        mi_json_raw(&b->table_text, s);
        return;
    }
    insert_char(b, s, 0);
}

/* The implied end tags, then pop until a name is out. */
static void generate_implied_end_tags(builder *b, const char *except) {
    static const char *IMPLIED[] = {"dd", "dt", "li", "optgroup", "option",
                                    "p", "rb", "rp", "rt", "rtc", NULL};
    while (b->stack_n) {
        const char *top = stack_name(&b->stack[b->stack_n - 1]);
        int matched = 0;
        for (size_t i = 0; IMPLIED[i]; i++) {
            if (!strcasecmp(top, IMPLIED[i]) && (!except || strcasecmp(top, except))) {
                matched = 1;
                break;
            }
        }
        if (!matched) break;
        pop_one(b);
    }
}

/* Close a <p> in button scope, inserting one first if there is none (the spec's
 * "close a p element"). */
static void close_p(builder *b) {
    if (in_button_scope(b, "p")) {
        generate_implied_end_tags(b, "p");
        pop_until(b, "p");
    }
}

/* Reset the insertion mode from the stack, as the spec's rule does. */
static void reset_insertion_mode(builder *b) {
    for (size_t i = b->stack_n; i-- > 0;) {
        const char *name = stack_name(&b->stack[i]);
        if (!strcasecmp(name, "select")) {
            b->mode = IM_IN_SELECT;
            return;
        }
        if (!strcasecmp(name, "td") || !strcasecmp(name, "th")) { b->mode = IM_IN_CELL; return; }
        if (!strcasecmp(name, "tr")) { b->mode = IM_IN_ROW; return; }
        if (!strcasecmp(name, "tbody") || !strcasecmp(name, "thead") ||
            !strcasecmp(name, "tfoot")) { b->mode = IM_IN_TABLE_BODY; return; }
        if (!strcasecmp(name, "caption")) { b->mode = IM_IN_CAPTION; return; }
        if (!strcasecmp(name, "colgroup")) { b->mode = IM_IN_COLUMN_GROUP; return; }
        if (!strcasecmp(name, "table")) { b->mode = IM_IN_TABLE; return; }
        if (!strcasecmp(name, "head")) { b->mode = IM_IN_HEAD; return; }
        if (!strcasecmp(name, "body")) { b->mode = IM_IN_BODY; return; }
        if (!strcasecmp(name, "html")) { b->mode = b->head ? IM_AFTER_HEAD : IM_BEFORE_HEAD; return; }
        if (!strcasecmp(name, "frameset")) { b->mode = IM_IN_FRAMESET; return; }
    }
    b->mode = IM_IN_BODY;
}

/* Whether a start tag, seen in "in body", switches the insertion mode. */
static int in_body_start_tag(builder *b, token *t) {
    const char *n = t->name;

    /* Character runs are handled by the caller. */

    if (!strcasecmp(n, "html")) {
        /* Merge the attributes onto the existing <html>, ignoring duplicates. */
        if (b->html) {
            for (size_t i = 0; i < t->attr_count; i++) {
                if (!mi_html_attr(b->html, t->attr_names[i])) {
                    mi_html_node *h = b->html;
                    h->attr_names = mi_realloc(h->attr_names, (h->attr_count + 1) * sizeof *h->attr_names);
                    h->attr_values = mi_realloc(h->attr_values, (h->attr_count + 1) * sizeof *h->attr_values);
                    h->attr_names[h->attr_count] = mi_strdup(t->attr_names[i]);
                    h->attr_values[h->attr_count] = mi_strdup(t->attr_values[i]);
                    h->attr_count++;
                }
            }
        }
        return 1;
    }
    if (!strcasecmp(n, "body")) {
        /* Merge onto the existing <body>. */
        if (b->body && b->stack_n > 1) {
            for (size_t i = 0; i < t->attr_count; i++) {
                if (!mi_html_attr(b->body, t->attr_names[i])) {
                    mi_html_node *h = b->body;
                    h->attr_names = mi_realloc(h->attr_names, (h->attr_count + 1) * sizeof *h->attr_names);
                    h->attr_values = mi_realloc(h->attr_values, (h->attr_count + 1) * sizeof *h->attr_values);
                    h->attr_names[h->attr_count] = mi_strdup(t->attr_names[i]);
                    h->attr_values[h->attr_count] = mi_strdup(t->attr_values[i]);
                    h->attr_count++;
                }
            }
        }
        b->frameset_ok = 0;
        return 1;
    }
    if (!strcasecmp(n, "frameset")) {
        if (!b->frameset_ok) return 1;
        /* Replace the body with a frameset. */
        if (b->body && b->body->parent) {
            mi_html_node *p = b->body->parent;
            mi_html_node *fs = make_element(t);
            if (b->body->prev_sibling) b->body->prev_sibling->next_sibling = fs;
            else p->first_child = fs;
            if (b->body->next_sibling) b->body->next_sibling->prev_sibling = fs;
            else p->last_child = fs;
            fs->parent = p;
            fs->prev_sibling = b->body->prev_sibling;
            fs->next_sibling = b->body->next_sibling;
            mi_html_free(b->body);
            b->body = fs;
        }
        /* Pop to html and push the frameset. */
        while (b->stack_n > 1) pop_one(b);
        stack_push(b, b->body);
        b->mode = IM_IN_FRAMESET;
        return 1;
    }

    /* The block elements that close an open <p>. */
    static const char *P_CLOSERS[] = {"address", "article", "aside", "blockquote",
        "center", "details", "dialog", "dir", "div", "dl", "fieldset",
        "figcaption", "figure", "footer", "header", "hgroup", "main", "menu",
        "nav", "ol", "p", "section", "summary", "ul", NULL};
    for (size_t i = 0; P_CLOSERS[i]; i++) {
        if (!strcasecmp(n, P_CLOSERS[i])) {
            close_p(b);
            insert_element(b, t, 0);
            return 1;
        }
    }
    /* Headings. */
    if ((n[0] == 'h' && n[1] >= '1' && n[1] <= '6' && !n[2])) {
        close_p(b);
        insert_element(b, t, 0);
        return 1;
    }
    if (!strcasecmp(n, "pre") || !strcasecmp(n, "listing")) {
        close_p(b);
        insert_element(b, t, 0);
        return 1;
    }
    if (!strcasecmp(n, "form")) {
        if (b->form) return 1;
        close_p(b);
        mi_html_node *el = insert_element(b, t, 0);
        b->form = el;
        return 1;
    }
    if (!strcasecmp(n, "li")) {
        /* Close an open li in list-item scope. */
        for (size_t i = b->stack_n; i-- > 0;) {
            if (!strcasecmp(b->stack[i].name, "li")) {
                generate_implied_end_tags(b, "li");
                pop_until(b, "li");
                break;
            }
            if (is_scope_boundary(b->stack[i].name, 1) &&
                strcasecmp(b->stack[i].name, "li")) break;
        }
        close_p(b);
        insert_element(b, t, 0);
        return 1;
    }
    if (!strcasecmp(n, "dd") || !strcasecmp(n, "dt")) {
        for (size_t i = b->stack_n; i-- > 0;) {
            if (!strcasecmp(b->stack[i].name, "dd") || !strcasecmp(b->stack[i].name, "dt")) {
                generate_implied_end_tags(b, b->stack[i].name);
                pop_until(b, b->stack[i].name);
                break;
            }
            if (is_scope_boundary(b->stack[i].name, 0) &&
                strcasecmp(b->stack[i].name, "dd") && strcasecmp(b->stack[i].name, "dt")) break;
        }
        close_p(b);
        insert_element(b, t, 0);
        return 1;
    }
    if (!strcasecmp(n, "button")) {
        if (in_scope(b, "button", 0)) {
            generate_implied_end_tags(b, NULL);
            pop_until(b, "button");
        }
        insert_element(b, t, 0);
        return 1;
    }
    if (!strcasecmp(n, "table")) {
        close_p(b);
        insert_element(b, t, 0);
        b->frameset_ok = 0;
        b->mode = IM_IN_TABLE;
        return 1;
    }
    if (!strcasecmp(n, "select")) {
        insert_element(b, t, 0);
        b->frameset_ok = 0;
        b->mode = IM_IN_SELECT;
        return 1;
    }
    if (!strcasecmp(n, "textarea") || !strcasecmp(n, "title")) {
        insert_element(b, t, 0);
        /* The content is RCDATA; the tokenizer handles it via the text mode. */
        b->mode = IM_TEXT;
        return 1;
    }
    if (!strcasecmp(n, "script") || !strcasecmp(n, "style") ||
        !strcasecmp(n, "xmp") || !strcasecmp(n, "iframe") ||
        !strcasecmp(n, "noembed") || !strcasecmp(n, "noframes") ||
        (!strcasecmp(n, "noscript") && b->scripting)) {
        insert_element(b, t, 0);
        b->mode = IM_TEXT;
        return 1;
    }
    if (!strcasecmp(n, "br") || !strcasecmp(n, "img") || !strcasecmp(n, "hr") ||
        !strcasecmp(n, "input") || !strcasecmp(n, "meta") || !strcasecmp(n, "link") ||
        !strcasecmp(n, "area") || !strcasecmp(n, "embed") || !strcasecmp(n, "wbr") ||
        !strcasecmp(n, "source") || !strcasecmp(n, "track") || !strcasecmp(n, "col") ||
        !strcasecmp(n, "param") || !strcasecmp(n, "base") || !strcasecmp(n, "basefont")) {
        if (!strcasecmp(n, "br")) close_p(b);
        mi_html_node *el = make_element(t);
        if (el) append_child(current_node(b), el);
        return 1;
    }

    /* Formatting elements: add to the active list and reconstruct. */
    if (is_formatting(n) || !strcasecmp(n, "nobr")) {
        if (!strcasecmp(n, "nobr") && in_scope(b, "nobr", 0)) {
            adoption_agency(b, "nobr");
            /* Fall through to insert a new one. */
        }
        mi_html_node *el = insert_element(b, t, 0);
        afe_push(b, el);
        return 1;
    }

    /* Any other start tag: reconstruct formatting and insert. */
    insert_element(b, t, 0);
    return 1;
}

/* The end-tag handling in "in body". */
static int in_body_end_tag(builder *b, token *t) {
    const char *n = t->name;
    if (!strcasecmp(n, "body")) {
        if (!in_scope(b, "body", 0)) return 1;
        b->mode = IM_AFTER_BODY;
        return 1;
    }
    if (!strcasecmp(n, "html")) {
        if (!in_scope(b, "body", 0)) return 1;
        b->mode = IM_AFTER_BODY;
        process_token(b, t); /* reprocess in after-body */
        return 1;
    }
    static const char *P_CLOSERS[] = {"address", "article", "aside", "blockquote",
        "button", "center", "details", "dialog", "dir", "div", "dl", "fieldset",
        "figcaption", "figure", "footer", "header", "hgroup", "listing", "main",
        "menu", "nav", "ol", "pre", "section", "summary", "ul", NULL};
    for (size_t i = 0; P_CLOSERS[i]; i++) {
        if (!strcasecmp(n, P_CLOSERS[i])) {
            if (!in_scope(b, n, 0)) return 1;
            generate_implied_end_tags(b, NULL);
            pop_until(b, n);
            return 1;
        }
    }
    if (!strcasecmp(n, "form")) {
        mi_html_node *node = b->form;
        b->form = NULL;
        if (!node || !in_scope(b, "form", 0)) return 1;
        generate_implied_end_tags(b, NULL);
        pop_until(b, "form");
        return 1;
    }
    if (!strcasecmp(n, "p")) {
        if (!in_button_scope(b, "p")) {
            /* Insert an empty p, then close it. */
            token p = {.type = TOK_START, .name = mi_strdup("p")};
            insert_element(b, &p, 0);
            free(p.name);
        }
        generate_implied_end_tags(b, "p");
        pop_until(b, "p");
        return 1;
    }
    if (!strcasecmp(n, "li")) {
        if (!in_scope(b, "li", 1)) return 1;
        generate_implied_end_tags(b, "li");
        pop_until(b, "li");
        return 1;
    }
    if (!strcasecmp(n, "dd") || !strcasecmp(n, "dt")) {
        if (!in_scope(b, n, 0)) return 1;
        generate_implied_end_tags(b, n);
        pop_until(b, n);
        return 1;
    }
    if ((n[0] == 'h' && n[1] >= '1' && n[1] <= '6' && !n[2])) {
        if (!in_scope(b, n, 0)) return 1;
        generate_implied_end_tags(b, NULL);
        pop_until(b, n);
        return 1;
    }
    if (is_formatting(n) || !strcasecmp(n, "nobr")) {
        adoption_agency(b, n);
        return 1;
    }
    if (!strcasecmp(n, "br")) {
        /* </br> is a start tag for a br. */
        token br = {.type = TOK_START, .name = mi_strdup("br")};
        close_p(b);
        mi_html_node *el = make_element(&br);
        if (el) append_child(current_node(b), el);
        free(br.name);
        return 1;
    }
    /* Any other end tag: pop to it, honouring scope. */
    for (size_t i = b->stack_n; i-- > 0;) {
        if (!strcasecmp(b->stack[i].name, n)) {
            generate_implied_end_tags(b, n);
            pop_until(b, n);
            return 1;
        }
        if (is_scope_boundary(b->stack[i].name, 0)) return 1;
    }
    return 1;
}

/* Reconstruct the active formatting elements, then insert. */
static void reconstruct_formatting(builder *b) {
    if (b->afe_n == 0) return;
    size_t i = b->afe_n;
    if (b->afe[i - 1].element && stack_index_of(b, b->afe[i - 1].element) >= 0) return;
    /* Find the first entry that is on the stack or is a marker (a null entry). */
    while (i > 0) {
        i--;
        if (!b->afe[i].element) { i++; break; }
        if (stack_index_of(b, b->afe[i].element) >= 0) { i++; break; }
        if (i == 0) break;
    }
    for (; i < b->afe_n; i++) {
        mi_html_node *old = b->afe[i].element;
        token clone = {.type = TOK_START, .name = mi_strdup(old->name)};
        clone.attr_count = old->attr_count;
        clone.attr_names = mi_calloc(old->attr_count ? old->attr_count : 1, sizeof *clone.attr_names);
        clone.attr_values = mi_calloc(old->attr_count ? old->attr_count : 1, sizeof *clone.attr_values);
        for (size_t j = 0; j < old->attr_count; j++) {
            clone.attr_names[j] = mi_strdup(old->attr_names[j]);
            clone.attr_values[j] = mi_strdup(old->attr_values[j]);
        }
        mi_html_node *fresh = make_element(&clone);
        token_free(&clone);
        insert_node(b, fresh, 0);
        stack_push(b, fresh);
        b->afe[i].element = fresh;
    }
}

static void process_token(builder *b, token *t) {
    switch (b->mode) {
        case IM_INITIAL:
            if (t->type == TOK_CHAR && isspace((unsigned char)(t->data ? t->data[0] : ' '))) return;
            if (t->type == TOK_COMMENT) { append_child(b->doc, make_text("", 0)); /* keep comments */ 
                mi_html_node *c = mi_calloc(1, sizeof *c); c->type = MI_HTML_COMMENT; c->text = mi_strdup(t->data ? t->data : "");
                /* The doc's last child is the empty text just added; replace it. */
                mi_html_node *last = b->doc->last_child;
                if (last && last->type == MI_HTML_TEXT && (!last->text || !*last->text)) {
                    if (last->prev_sibling) last->prev_sibling->next_sibling = c; else b->doc->first_child = c;
                    c->prev_sibling = last->prev_sibling;
                    c->parent = b->doc;
                    if (b->doc->last_child == last) b->doc->last_child = c;
                    free(last->text); free(last);
                } else append_child(b->doc, c);
                return; }
            if (t->type == TOK_DOCTYPE) {
                mi_html_node *d = mi_calloc(1, sizeof *d);
                d->type = MI_HTML_DOCTYPE;
                d->text = mi_strdup(t->doctype_name ? t->doctype_name : "html");
                append_child(b->doc, d);
                b->mode = IM_BEFORE_HTML;
                return;
            }
            b->mode = IM_BEFORE_HTML;
            /* Reprocess. */
            process_token(b, t);
            return;

        case IM_BEFORE_HTML:
            if (t->type == TOK_COMMENT) {
                mi_html_node *c = mi_calloc(1, sizeof *c); c->type = MI_HTML_COMMENT;
                c->text = mi_strdup(t->data ? t->data : "");
                append_child(b->doc, c);
                return;
            }
            if (t->type == TOK_DOCTYPE) return;
            if (t->type == TOK_CHAR && isspace((unsigned char)(t->data ? t->data[0] : ' '))) return;
            if (t->type == TOK_START && !strcasecmp(t->name, "html")) {
                b->html = make_element(t);
                append_child(b->doc, b->html);
                stack_push(b, b->html);
                b->mode = IM_BEFORE_HEAD;
                return;
            }
            /* Anything else: create an implied <html>. */
            {
                token h = {.type = TOK_START, .name = mi_strdup("html")};
                b->html = make_element(&h);
                free(h.name);
                append_child(b->doc, b->html);
                stack_push(b, b->html);
                b->mode = IM_BEFORE_HEAD;
            }
            process_token(b, t);
            return;

        case IM_BEFORE_HEAD:
            if (t->type == TOK_CHAR && isspace((unsigned char)(t->data ? t->data[0] : ' '))) return;
            if (t->type == TOK_COMMENT) {
                mi_html_node *c = mi_calloc(1, sizeof *c); c->type = MI_HTML_COMMENT;
                c->text = mi_strdup(t->data ? t->data : "");
                append_child(b->html, c);
                return;
            }
            if (t->type == TOK_DOCTYPE) return;
            if (t->type == TOK_START && !strcasecmp(t->name, "html")) { process_token(b, t); return; }
            if (t->type == TOK_START && !strcasecmp(t->name, "head")) {
                b->head = make_element(t);
                append_child(b->html, b->head);
                stack_push(b, b->head);
                b->mode = IM_IN_HEAD;
                return;
            }
            /* Implied <head>. */
            {
                token h = {.type = TOK_START, .name = mi_strdup("head")};
                b->head = make_element(&h);
                free(h.name);
                append_child(b->html, b->head);
                stack_push(b, b->head);
                b->mode = IM_IN_HEAD;
            }
            process_token(b, t);
            return;

        case IM_IN_HEAD:
            if (t->type == TOK_CHAR && isspace((unsigned char)(t->data ? t->data[0] : ' '))) {
                insert_char(b, t->data, 0);
                return;
            }
            if (t->type == TOK_COMMENT) {
                mi_html_node *c = mi_calloc(1, sizeof *c); c->type = MI_HTML_COMMENT;
                c->text = mi_strdup(t->data ? t->data : "");
                append_child(current_node(b), c);
                return;
            }
            if (t->type == TOK_START &&
                (!strcasecmp(t->name, "title") || !strcasecmp(t->name, "meta") ||
                 !strcasecmp(t->name, "link") || !strcasecmp(t->name, "style") ||
                 !strcasecmp(t->name, "script") || !strcasecmp(t->name, "noscript") ||
                 !strcasecmp(t->name, "base") || !strcasecmp(t->name, "basefont") ||
                 !strcasecmp(t->name, "bgsound") || !strcasecmp(t->name, "template"))) {
                /* Insert and, for the raw-text ones, switch to text mode. */
                mi_html_node *el = insert_element(b, t, 0);
                if (!strcasecmp(t->name, "title") || !strcasecmp(t->name, "style") ||
                    !strcasecmp(t->name, "script") || !strcasecmp(t->name, "noscript") ||
                    !strcasecmp(t->name, "noframes")) {
                    b->mode = IM_TEXT;
                } else if (el && (t->self_closing || !strcasecmp(t->name, "meta") ||
                                  !strcasecmp(t->name, "link") || !strcasecmp(t->name, "base") ||
                                  !strcasecmp(t->name, "basefont") || !strcasecmp(t->name, "bgsound"))) {
                    /* A void head element: pop it. */
                    pop_one(b);
                }
                return;
            }
            if (t->type == TOK_END && !strcasecmp(t->name, "head")) {
                pop_one(b);
                b->mode = IM_AFTER_HEAD;
                return;
            }
            if (t->type == TOK_END && (!strcasecmp(t->name, "body") ||
                                       !strcasecmp(t->name, "html") ||
                                       !strcasecmp(t->name, "br"))) {
                pop_one(b);
                b->mode = IM_AFTER_HEAD;
                process_token(b, t);
                return;
            }
            /* Anything else: pop the head and reprocess. */
            pop_one(b);
            b->mode = IM_AFTER_HEAD;
            process_token(b, t);
            return;

        case IM_AFTER_HEAD:
            if (t->type == TOK_CHAR && isspace((unsigned char)(t->data ? t->data[0] : ' '))) {
                insert_char(b, t->data, 0);
                return;
            }
            if (t->type == TOK_COMMENT) {
                mi_html_node *c = mi_calloc(1, sizeof *c); c->type = MI_HTML_COMMENT;
                c->text = mi_strdup(t->data ? t->data : "");
                append_child(b->html, c);
                return;
            }
            if (t->type == TOK_START && !strcasecmp(t->name, "body")) {
                b->body = make_element(t);
                append_child(b->html, b->body);
                stack_push(b, b->body);
                b->frameset_ok = 1;
                b->mode = IM_IN_BODY;
                return;
            }
            if (t->type == TOK_START && !strcasecmp(t->name, "frameset")) {
                mi_html_node *el = make_element(t);
                append_child(b->html, el);
                stack_push(b, el);
                b->mode = IM_IN_FRAMESET;
                return;
            }
            if (t->type == TOK_START && (!strcasecmp(t->name, "base") ||
                !strcasecmp(t->name, "link") || !strcasecmp(t->name, "meta") ||
                !strcasecmp(t->name, "script") || !strcasecmp(t->name, "style") ||
                !strcasecmp(t->name, "title") || !strcasecmp(t->name, "template"))) {
                /* Put it in the head. */
                stack_push(b, b->head);
                process_token(b, t);
                return;
            }
            if (t->type == TOK_END && (!strcasecmp(t->name, "body") ||
                                       !strcasecmp(t->name, "html") ||
                                       !strcasecmp(t->name, "br"))) {
                /* Implied body. */
                token bt = {.type = TOK_START, .name = mi_strdup("body")};
                b->body = make_element(&bt);
                free(bt.name);
                append_child(b->html, b->body);
                stack_push(b, b->body);
                b->frameset_ok = 1;
                b->mode = IM_IN_BODY;
                process_token(b, t);
                return;
            }
            {
                token bt = {.type = TOK_START, .name = mi_strdup("body")};
                b->body = make_element(&bt);
                free(bt.name);
                append_child(b->html, b->body);
                stack_push(b, b->body);
                b->frameset_ok = 1;
                b->mode = IM_IN_BODY;
            }
            process_token(b, t);
            return;

        case IM_IN_BODY: {
            if (t->type == TOK_CHAR) {
                if (t->data && t->data[0] == '\0') return;
                reconstruct_formatting(b);
                insert_char(b, t->data ? t->data : "", 0);
                if (t->data && !isspace((unsigned char)t->data[0])) b->frameset_ok = 0;
                return;
            }
            if (t->type == TOK_COMMENT) {
                mi_html_node *c = mi_calloc(1, sizeof *c); c->type = MI_HTML_COMMENT;
                c->text = mi_strdup(t->data ? t->data : "");
                append_child(current_node(b), c);
                return;
            }
            if (t->type == TOK_DOCTYPE) return;
            if (t->type == TOK_START) { in_body_start_tag(b, t); return; }
            if (t->type == TOK_END) { in_body_end_tag(b, t); return; }
            return;
        }

        case IM_TEXT: {
            if (t->type == TOK_CHAR) { insert_char(b, t->data, 0); return; }
            if (t->type == TOK_END) {
                mi_html_node *cur = current_node(b);
                if (cur && cur->type == MI_HTML_ELEMENT &&
                    !strcasecmp(cur->name, t->name)) {
                    pop_one(b);
                    b->mode = b->original_mode_valid ? b->original_mode : IM_IN_BODY;
                    return;
                }
                /* A mismatched close: treat as text. */
                char buf[64];
                snprintf(buf, sizeof buf, "</%s>", t->name);
                insert_char(b, buf, 0);
                return;
            }
            if (t->type == TOK_EOF) {
                pop_one(b);
                b->mode = b->original_mode_valid ? b->original_mode : IM_IN_BODY;
                return;
            }
            return;
        }

        case IM_IN_TABLE: {
            if (t->type == TOK_CHAR) {
                if (isspace((unsigned char)(t->data ? t->data[0] : ' '))) {
                    insert_char_for_mode(b, t->data);
                } else {
                    /* Foster-parent a non-space character, merged into the run
                     * already placed before the table. */
                    flush_table_text(b);
                    insert_char(b, t->data, 1);
                }
                return;
            }
            if (t->type == TOK_COMMENT) {
                mi_html_node *c = mi_calloc(1, sizeof *c); c->type = MI_HTML_COMMENT;
                c->text = mi_strdup(t->data ? t->data : "");
                append_child(current_node(b), c);
                return;
            }
            if (t->type == TOK_DOCTYPE) return;
            if (t->type == TOK_START) {
                if (!strcasecmp(t->name, "caption")) {
                    flush_table_text(b);
                    insert_element(b, t, 0);
                    b->mode = IM_IN_CAPTION;
                    return;
                }
                if (!strcasecmp(t->name, "colgroup")) {
                    flush_table_text(b);
                    insert_element(b, t, 0);
                    b->mode = IM_IN_COLUMN_GROUP;
                    return;
                }
                if (!strcasecmp(t->name, "col")) {
                    flush_table_text(b);
                    token cg = {.type = TOK_START, .name = mi_strdup("colgroup")};
                    insert_element(b, &cg, 0);
                    free(cg.name);
                    b->mode = IM_IN_COLUMN_GROUP;
                    process_token(b, t);
                    return;
                }
                if (!strcasecmp(t->name, "tbody") || !strcasecmp(t->name, "thead") ||
                    !strcasecmp(t->name, "tfoot")) {
                    flush_table_text(b);
                    insert_element(b, t, 0);
                    b->mode = IM_IN_TABLE_BODY;
                    return;
                }
                if (!strcasecmp(t->name, "tr")) {
                    flush_table_text(b);
                    token tb = {.type = TOK_START, .name = mi_strdup("tbody")};
                    insert_element(b, &tb, 0);
                    free(tb.name);
                    b->mode = IM_IN_TABLE_BODY;
                    process_token(b, t);
                    return;
                }
                if (!strcasecmp(t->name, "td") || !strcasecmp(t->name, "th")) {
                    flush_table_text(b);
                    token tb = {.type = TOK_START, .name = mi_strdup("tbody")};
                    insert_element(b, &tb, 0);
                    free(tb.name);
                    token tr = {.type = TOK_START, .name = mi_strdup("tr")};
                    insert_element(b, &tr, 0);
                    free(tr.name);
                    b->mode = IM_IN_ROW;
                    process_token(b, t);
                    return;
                }
                if (!strcasecmp(t->name, "table")) {
                    /* A table inside a table closes the outer one. */
                    if (in_scope(b, "table", 3)) {
                        pop_until(b, "table");
                        reset_insertion_mode(b);
                        process_token(b, t);
                        return;
                    }
                }
                if (!strcasecmp(t->name, "style") || !strcasecmp(t->name, "script") ||
                    !strcasecmp(t->name, "template")) {
                    process_token(b, t);
                    return;
                }
                if (!strcasecmp(t->name, "input")) {
                    const char *type = mi_html_attr(NULL, "type");
                    (void)type;
                    /* Fall through to foster parenting for anything else. */
                }
            }
            if (t->type == TOK_END) {
                if (!strcasecmp(t->name, "table")) {
                    if (in_scope(b, "table", 3)) {
                        pop_until(b, "table");
                        reset_insertion_mode(b);
                    }
                    return;
                }
                if (!strcasecmp(t->name, "body") || !strcasecmp(t->name, "caption") ||
                    !strcasecmp(t->name, "col") || !strcasecmp(t->name, "colgroup") ||
                    !strcasecmp(t->name, "html") || !strcasecmp(t->name, "tbody") ||
                    !strcasecmp(t->name, "td") || !strcasecmp(t->name, "tfoot") ||
                    !strcasecmp(t->name, "th") || !strcasecmp(t->name, "thead") ||
                    !strcasecmp(t->name, "tr")) {
                    return;
                }
            }
            /* Anything else: foster-parent it, in body mode. */
            {
                insertion_mode saved = b->mode;
                b->mode = IM_IN_BODY;
                process_token(b, t);
                b->mode = saved;
            }
            return;
        }

        case IM_IN_TABLE_BODY: {
            if (t->type == TOK_START && !strcasecmp(t->name, "tr")) {
                flush_table_text(b);
                insert_element(b, t, 0);
                b->mode = IM_IN_ROW;
                return;
            }
            if (t->type == TOK_START && (!strcasecmp(t->name, "td") || !strcasecmp(t->name, "th"))) {
                flush_table_text(b);
                token tr = {.type = TOK_START, .name = mi_strdup("tr")};
                insert_element(b, &tr, 0);
                free(tr.name);
                b->mode = IM_IN_ROW;
                process_token(b, t);
                return;
            }
            if (t->type == TOK_END && (!strcasecmp(t->name, "tbody") ||
                                       !strcasecmp(t->name, "tfoot") ||
                                       !strcasecmp(t->name, "thead"))) {
                if (in_scope(b, t->name, 3)) {
                    flush_table_text(b);
                    pop_until(b, t->name);
                    b->mode = IM_IN_TABLE;
                }
                return;
            }
            if (t->type == TOK_END && !strcasecmp(t->name, "table")) {
                if (in_scope(b, "tbody", 3) || in_scope(b, "thead", 3) || in_scope(b, "tfoot", 3)) {
                    flush_table_text(b);
                    /* Pop to the table body. */
                    for (size_t i = b->stack_n; i-- > 0;) {
                        if (!strcasecmp(b->stack[i].name, "tbody") ||
                            !strcasecmp(b->stack[i].name, "thead") ||
                            !strcasecmp(b->stack[i].name, "tfoot")) {
                            pop_until(b, b->stack[i].name);
                            break;
                        }
                    }
                    b->mode = IM_IN_TABLE;
                    process_token(b, t);
                }
                return;
            }
            /* A table-internal end tag that does not belong here. */
            if (t->type == TOK_END && (!strcasecmp(t->name, "body") ||
                !strcasecmp(t->name, "caption") || !strcasecmp(t->name, "col") ||
                !strcasecmp(t->name, "colgroup") || !strcasecmp(t->name, "html") ||
                !strcasecmp(t->name, "td") || !strcasecmp(t->name, "th") ||
                !strcasecmp(t->name, "tr"))) return;
            if (t->type == TOK_START && !strcasecmp(t->name, "caption")) {
                /* handled by in-table via fallthrough */
            }
            /* Anything else: process in table mode. */
            {
                insertion_mode saved = b->mode;
                b->mode = IM_IN_TABLE;
                process_token(b, t);
                b->mode = saved;
            }
            return;
        }

        case IM_IN_ROW: {
            if (t->type == TOK_START && (!strcasecmp(t->name, "td") || !strcasecmp(t->name, "th"))) {
                flush_table_text(b);
                insert_element(b, t, 0);
                b->mode = IM_IN_CELL;
                return;
            }
            if (t->type == TOK_END && !strcasecmp(t->name, "tr")) {
                if (in_scope(b, "tr", 3)) {
                    flush_table_text(b);
                    pop_until(b, "tr");
                    b->mode = IM_IN_TABLE_BODY;
                }
                return;
            }
            if (t->type == TOK_START && !strcasecmp(t->name, "tr")) {
                if (in_scope(b, "tr", 3)) {
                    flush_table_text(b);
                    pop_until(b, "tr");
                    b->mode = IM_IN_TABLE_BODY;
                    process_token(b, t);
                }
                return;
            }
            if (t->type == TOK_END && (!strcasecmp(t->name, "table") ||
                                       !strcasecmp(t->name, "tbody") ||
                                       !strcasecmp(t->name, "tfoot") ||
                                       !strcasecmp(t->name, "thead"))) {
                if (in_scope(b, "tr", 3)) {
                    flush_table_text(b);
                    pop_until(b, "tr");
                    b->mode = IM_IN_TABLE_BODY;
                    process_token(b, t);
                }
                return;
            }
            if (t->type == TOK_END && (!strcasecmp(t->name, "body") ||
                !strcasecmp(t->name, "caption") || !strcasecmp(t->name, "col") ||
                !strcasecmp(t->name, "colgroup") || !strcasecmp(t->name, "html") ||
                !strcasecmp(t->name, "td") || !strcasecmp(t->name, "th"))) return;
            {
                insertion_mode saved = b->mode;
                b->mode = IM_IN_TABLE;
                process_token(b, t);
                b->mode = saved;
            }
            return;
        }

        case IM_IN_CELL: {
            if (t->type == TOK_END && (!strcasecmp(t->name, "td") || !strcasecmp(t->name, "th"))) {
                if (in_scope(b, t->name, 3)) {
                    generate_implied_end_tags(b, NULL);
                    pop_until(b, t->name);
                    b->mode = IM_IN_ROW;
                }
                return;
            }
            if (t->type == TOK_START && (!strcasecmp(t->name, "caption") ||
                !strcasecmp(t->name, "col") || !strcasecmp(t->name, "colgroup") ||
                !strcasecmp(t->name, "tbody") || !strcasecmp(t->name, "td") ||
                !strcasecmp(t->name, "tfoot") || !strcasecmp(t->name, "th") ||
                !strcasecmp(t->name, "thead") || !strcasecmp(t->name, "tr"))) {
                /* Close the current cell, then reprocess in row mode. */
                if (in_scope(b, "td", 3) || in_scope(b, "th", 3)) {
                    for (size_t i = b->stack_n; i-- > 0;) {
                        if (!strcasecmp(b->stack[i].name, "td") ||
                            !strcasecmp(b->stack[i].name, "th")) {
                            pop_until(b, b->stack[i].name);
                            break;
                        }
                    }
                    b->mode = IM_IN_ROW;
                    process_token(b, t);
                }
                return;
            }
            if (t->type == TOK_END && (!strcasecmp(t->name, "table") ||
                !strcasecmp(t->name, "tbody") || !strcasecmp(t->name, "tfoot") ||
                !strcasecmp(t->name, "thead") || !strcasecmp(t->name, "tr"))) {
                if (in_scope(b, t->name, 3)) {
                    for (size_t i = b->stack_n; i-- > 0;) {
                        if (!strcasecmp(b->stack[i].name, "td") ||
                            !strcasecmp(b->stack[i].name, "th")) {
                            pop_until(b, b->stack[i].name);
                            break;
                        }
                    }
                    b->mode = IM_IN_ROW;
                    process_token(b, t);
                }
                return;
            }
            if (t->type == TOK_END && (!strcasecmp(t->name, "body") ||
                !strcasecmp(t->name, "caption") || !strcasecmp(t->name, "col") ||
                !strcasecmp(t->name, "colgroup") || !strcasecmp(t->name, "html"))) return;
            /* Anything else: process in body mode. */
            {
                insertion_mode saved = b->mode;
                b->mode = IM_IN_BODY;
                process_token(b, t);
                b->mode = saved;
            }
            return;
        }

        case IM_IN_CAPTION:
        case IM_IN_COLUMN_GROUP:
        case IM_IN_SELECT:
        case IM_IN_SELECT_IN_TABLE: {
            /* A compact handling: insert, and let the modes above resume on the
             * matching end tag. These modes are less exercised by extraction
             * rules, so they are handled simply but correctly for nesting. */
            if (t->type == TOK_CHAR) {
                if (b->mode == IM_IN_COLUMN_GROUP && isspace((unsigned char)(t->data ? t->data[0] : ' '))) {
                    insert_char(b, t->data, 0);
                } else {
                    insert_char(b, t->data, 0);
                }
                return;
            }
            if (t->type == TOK_COMMENT) {
                mi_html_node *c = mi_calloc(1, sizeof *c); c->type = MI_HTML_COMMENT;
                c->text = mi_strdup(t->data ? t->data : "");
                append_child(current_node(b), c);
                return;
            }
            if (t->type == TOK_START) {
                if (b->mode == IM_IN_SELECT &&
                    (!strcasecmp(t->name, "option") || !strcasecmp(t->name, "optgroup"))) {
                    if (!strcasecmp(t->name, "optgroup") && b->stack_n &&
                        !strcasecmp(current_node(b)->name, "option")) pop_one(b);
                    insert_element(b, t, 0);
                    return;
                }
                if (b->mode == IM_IN_SELECT && !strcasecmp(t->name, "select")) {
                    if (in_scope(b, "select", 4)) {
                        pop_until(b, "select");
                        reset_insertion_mode(b);
                    }
                    return;
                }
                insert_element(b, t, 0);
                return;
            }
            if (t->type == TOK_END) {
                if (!strcasecmp(t->name, "caption") && b->mode == IM_IN_CAPTION) {
                    if (in_scope(b, "caption", 3)) {
                        generate_implied_end_tags(b, NULL);
                        pop_until(b, "caption");
                        b->mode = IM_IN_TABLE;
                    }
                    return;
                }
                if (!strcasecmp(t->name, "colgroup") && b->mode == IM_IN_COLUMN_GROUP) {
                    pop_until(b, "colgroup");
                    b->mode = IM_IN_TABLE;
                    return;
                }
                if (!strcasecmp(t->name, "select") && (b->mode == IM_IN_SELECT ||
                                                       b->mode == IM_IN_SELECT_IN_TABLE)) {
                    if (in_scope(b, "select", 4)) {
                        pop_until(b, "select");
                        reset_insertion_mode(b);
                    }
                    return;
                }
                if (!strcasecmp(t->name, "option") || !strcasecmp(t->name, "optgroup")) {
                    if (b->stack_n && !strcasecmp(current_node(b)->name, t->name)) pop_one(b);
                    return;
                }
                /* A stray end tag: ignore in these modes. */
                return;
            }
            return;
        }

        case IM_AFTER_BODY:
            if (t->type == TOK_CHAR && isspace((unsigned char)(t->data ? t->data[0] : ' '))) {
                process_token(b, t); /* handled as in body */
                b->mode = IM_IN_BODY; process_token(b, t); b->mode = IM_AFTER_BODY;
                return;
            }
            if (t->type == TOK_END && !strcasecmp(t->name, "html")) {
                b->mode = IM_AFTER_AFTER_BODY;
                return;
            }
            b->mode = IM_IN_BODY;
            process_token(b, t);
            b->mode = IM_AFTER_BODY;
            return;

        case IM_AFTER_AFTER_BODY:
        case IM_IN_FRAMESET:
        case IM_AFTER_FRAMESET:
        default:
            /* A compact tail: keep the tree by inserting into the current node. */
            if (t->type == TOK_CHAR && b->stack_n) insert_char(b, t->data, 0);
            else if (t->type == TOK_START && b->stack_n) insert_element(b, t, 0);
            else if (t->type == TOK_END && b->stack_n &&
                     !strcasecmp(current_node(b)->name, t->name)) pop_one(b);
            return;
    }
}

/* ------------------------------------------------------------------ */
/* Driver                                                              */
/* ------------------------------------------------------------------ */

mi_html_node *mi_html5_parse(const char *html, size_t len, const mi_html5_config *cfg) {
    builder b;
    memset(&b, 0, sizeof b);
    b.scripting = cfg ? cfg->scripting : 0;
    b.max_depth = (cfg && cfg->max_depth > 0) ? cfg->max_depth : 512;
    b.mode = IM_INITIAL;
    b.frameset_ok = 1;
    mi_json_init(&b.table_text);

    b.doc = mi_calloc(1, sizeof *b.doc);
    if (!b.doc) return NULL;
    b.doc->type = MI_HTML_DOCUMENT;

    tokenizer tz = {.p = html, .end = html + len};
    token t;
    for (;;) {
        next_token(&tz, &t);
        if (t.type == TOK_EOF) { token_free(&t); break; }

        /*
         * RCDATA/RAWTEXT content is read as one text token when the current
         * element's content is raw, which is the spec's tokenizer state chosen
         * by the tree builder.
         */
        if (t.type == TOK_START) {
            int decode = 0;
            if (is_rawtext(t.name, b.scripting, &decode)) {
                /*
                 * The element's content is RCDATA or RAWTEXT, so it is read as
                 * one raw token rather than parsed. The insertion mode before the
                 * element is remembered and restored, which is what the spec's
                 * text mode does — losing it would leave the parser stuck after
                 * the element and the rest of the page would nest wrongly.
                 */
                insertion_mode before = b.mode;
                process_token(&b, &t);
                if (b.stack_n && !strcasecmp(current_node(&b)->name, t.name)) {
                    char *text = read_raw_text(&tz.p, tz.end, t.name, decode);
                    if (text) {
                        if (*text) insert_char(&b, text, 0);
                        free(text);
                    }
                    /* The end tag was consumed; pop the element and restore the
                     * mode, or move on from head if we were in the head. */
                    pop_one(&b);
                    if (before == IM_IN_HEAD) b.mode = IM_AFTER_HEAD;
                    else if (before == IM_BEFORE_HEAD) b.mode = IM_AFTER_HEAD;
                    else b.mode = before;
                }
                token_free(&t);
                continue;
            }
        }

        process_token(&b, &t);
        token_free(&t);
    }

    /* EOF: close everything. */
    flush_table_text(&b);
    while (b.stack_n) pop_one(&b);
    afe_clear(&b);
    free(b.stack);
    free(b.afe);
    mi_json_free(&b.table_text);
    return b.doc;
}
