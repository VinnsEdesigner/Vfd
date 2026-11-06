#define _GNU_SOURCE
#include "mi_css.h"
#include "mi_util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* The compiled form                                                   */
/* ------------------------------------------------------------------ */

typedef enum {
    COMB_NONE = 0, /* the first compound in a complex selector */
    COMB_DESCENDANT,
    COMB_CHILD,
    COMB_ADJACENT,  /* + */
    COMB_SIBLING,   /* ~ */
} combinator;

typedef enum {
    ATTR_PRESENT,
    ATTR_EQ,
    ATTR_PREFIX,   /* ^= */
    ATTR_SUFFIX,   /* $= */
    ATTR_CONTAINS, /* *= */
    ATTR_WORD,     /* ~= */
} attr_op;

typedef struct {
    char *name;
    attr_op op;
    char *value;
} attr_test;

typedef enum {
    PSEUDO_FIRST_CHILD,
    PSEUDO_LAST_CHILD,
    PSEUDO_NTH_CHILD,
    PSEUDO_NOT,
} pseudo_kind;

typedef struct {
    pseudo_kind kind;
    int a, b; /* nth-child: index = a*n + b, 1-based */
    struct mi_css_selector *not; /* :not(...) */
} pseudo_test;

/* One compound selector: tag + id + classes + attributes + pseudos. All of a
 * compound must match the same element. */
typedef struct {
    char *tag;      /* NULL or "*" means any */
    char *id;       /* #id */
    char **classes; /* .class */
    size_t class_count;
    attr_test *attrs;
    size_t attr_count;
    pseudo_test *pseudos;
    size_t pseudo_count;
} compound;

/* A chain of compounds joined by combinators, read right to left when matching:
 * the last compound is the subject. */
typedef struct {
    compound *compounds;
    combinator *combinators; /* between compounds[i-1] and compounds[i] */
    size_t count;
} chain;

struct mi_css_selector {
    /* A selector may be a group — `a, b` — so it is a list of chains and a node
     * matches if *any* chain matches. */
    chain *chains;
    size_t chain_count;
};

static char g_last_error[160];

const char *mi_css_last_error(void) { return g_last_error; }

/* ------------------------------------------------------------------ */
/* Parsing helpers                                                     */
/* ------------------------------------------------------------------ */

static void set_err(char *err, size_t cap, const char *msg) {
    snprintf(g_last_error, sizeof g_last_error, "%s", msg);
    if (err && cap) snprintf(err, cap, "%s", msg);
}

static int is_ident_start(int c) { return isalpha(c) || c == '_' || c == '-'; }
static int is_ident(int c) { return isalnum(c) || c == '_' || c == '-'; }

/* Read an identifier into a fresh string. */
static char *read_ident(const char **pp) {
    const char *p = *pp;
    const char *start = p;
    while (*p && is_ident((unsigned char)*p)) p++;
    if (p == start) return NULL;
    *pp = p;
    return mi_strndup(start, (size_t)(p - start));
}

/* Read a quoted string, or a bare identifier, into a fresh string. */
static char *read_value(const char **pp) {
    const char *p = *pp;
    if (*p == '"' || *p == '\'') {
        char q = *p++;
        const char *start = p;
        while (*p && *p != q) p++;
        if (*p != q) return NULL;
        char *s = mi_strndup(start, (size_t)(p - start));
        *pp = p + 1;
        return s;
    }
    return read_ident(pp);
}

static void compound_free(compound *c) {
    free(c->tag);
    free(c->id);
    for (size_t i = 0; i < c->class_count; i++) free(c->classes[i]);
    free(c->classes);
    for (size_t i = 0; i < c->attr_count; i++) {
        free(c->attrs[i].name);
        free(c->attrs[i].value);
    }
    free(c->attrs);
    for (size_t i = 0; i < c->pseudo_count; i++) {
        if (c->pseudos[i].kind == PSEUDO_NOT) mi_css_free(c->pseudos[i].not);
    }
    free(c->pseudos);
    memset(c, 0, sizeof *c);
}

static void add_class(compound *c, char *cls) {
    char **g = mi_realloc(c->classes, (c->class_count + 1) * sizeof *g);
    if (!g) { free(cls); return; }
    c->classes = g;
    c->classes[c->class_count++] = cls;
}

static attr_test *add_attr(compound *c) {
    attr_test *g = mi_realloc(c->attrs, (c->attr_count + 1) * sizeof *g);
    if (!g) return NULL;
    c->attrs = g;
    attr_test *t = &c->attrs[c->attr_count++];
    memset(t, 0, sizeof *t);
    return t;
}

static pseudo_test *add_pseudo(compound *c) {
    pseudo_test *g = mi_realloc(c->pseudos, (c->pseudo_count + 1) * sizeof *g);
    if (!g) return NULL;
    c->pseudos = g;
    pseudo_test *t = &c->pseudos[c->pseudo_count++];
    memset(t, 0, sizeof *t);
    return t;
}

/* Parse one compound selector (no combinators). */
static int parse_compound(const char **pp, compound *c, char *err, size_t errcap) {
    const char *p = *pp;
    int any = 0;

    if (*p == '*') { p++; any = 1; }
    else if (is_ident_start((unsigned char)*p)) {
        c->tag = read_ident(&p);
        if (c->tag) for (char *q = c->tag; *q; q++) *q = (char)tolower((unsigned char)*q);
        any = 1;
    }

    for (;;) {
        if (*p == '#') {
            p++;
            c->id = read_ident(&p);
            if (!c->id) { set_err(err, errcap, "expected id after '#'"); return -1; }
            any = 1;
        } else if (*p == '.') {
            p++;
            char *cls = read_ident(&p);
            if (!cls) { set_err(err, errcap, "expected class after '.'"); return -1; }
            add_class(c, cls);
            any = 1;
        } else if (*p == '[') {
            p++;
            while (*p == ' ') p++;
            attr_test *t = add_attr(c);
            if (!t) { set_err(err, errcap, "out of memory"); return -1; }
            t->name = read_ident(&p);
            if (!t->name) { set_err(err, errcap, "expected attribute name"); return -1; }
            while (*p == ' ') p++;
            if (*p == ']') {
                t->op = ATTR_PRESENT;
                p++;
            } else {
                if (*p == '^' && p[1] == '=') { t->op = ATTR_PREFIX; p += 2; }
                else if (*p == '$' && p[1] == '=') { t->op = ATTR_SUFFIX; p += 2; }
                else if (*p == '*' && p[1] == '=') { t->op = ATTR_CONTAINS; p += 2; }
                else if (*p == '~' && p[1] == '=') { t->op = ATTR_WORD; p += 2; }
                else if (*p == '=') { t->op = ATTR_EQ; p += 1; }
                else { set_err(err, errcap, "bad attribute operator"); return -1; }
                while (*p == ' ') p++;
                t->value = read_value(&p);
                if (!t->value) { set_err(err, errcap, "expected attribute value"); return -1; }
                while (*p == ' ') p++;
                if (*p != ']') { set_err(err, errcap, "expected ']'"); return -1; }
                p++;
            }
            any = 1;
        } else if (*p == ':') {
            p++;
            if (*p == ':') p++; /* accept ::first-child as :first-child */
            char *name = read_ident(&p);
            if (!name) { set_err(err, errcap, "expected pseudo-class name"); return -1; }
            pseudo_test *t = add_pseudo(c);
            if (!t) { free(name); set_err(err, errcap, "out of memory"); return -1; }
            if (!strcasecmp(name, "first-child")) t->kind = PSEUDO_FIRST_CHILD;
            else if (!strcasecmp(name, "last-child")) t->kind = PSEUDO_LAST_CHILD;
            else if (!strcasecmp(name, "nth-child")) {
                t->kind = PSEUDO_NTH_CHILD;
                if (*p != '(') { free(name); set_err(err, errcap, "nth-child needs (n)"); return -1; }
                p++;
                while (*p == ' ') p++;
                if (!strncasecmp(p, "odd", 3)) { t->a = 2; t->b = 1; p += 3; }
                else if (!strncasecmp(p, "even", 4)) { t->a = 2; t->b = 0; p += 4; }
                else {
                    /* a*n+b, or just n, or just b. */
                    int sign = 1;
                    if (*p == '-') { sign = -1; p++; }
                    int first = 0;
                    if (isdigit((unsigned char)*p)) {
                        while (isdigit((unsigned char)*p)) first = first * 10 + (*p++ - '0');
                        first *= sign;
                    }
                    if (*p == 'n' || *p == 'N') {
                        p++;
                        t->a = (first == 0 && sign == -1) ? -1 : (first == 0 ? 1 : first);
                        t->b = 0;
                        if (*p == '+' || *p == '-') {
                            int s2 = (*p == '-') ? -1 : 1;
                            p++;
                            int v = 0;
                            while (isdigit((unsigned char)*p)) v = v * 10 + (*p++ - '0');
                            t->b = v * s2;
                        }
                    } else {
                        t->a = 0;
                        t->b = first;
                    }
                }
                while (*p == ' ') p++;
                if (*p != ')') { free(name); set_err(err, errcap, "expected ')'"); return -1; }
                p++;
            } else if (!strcasecmp(name, "not")) {
                t->kind = PSEUDO_NOT;
                if (*p != '(') { free(name); set_err(err, errcap, "not needs (sel)"); return -1; }
                p++;
                const char *start = p;
                int depth = 1;
                while (*p && depth) {
                    if (*p == '(') depth++;
                    else if (*p == ')') depth--;
                    if (depth) p++;
                }
                if (depth) { free(name); set_err(err, errcap, "unbalanced '(' in :not"); return -1; }
                char *inner = mi_strndup(start, (size_t)(p - start));
                t->not = mi_css_compile(inner, err, errcap);
                free(inner);
                if (!t->not) { free(name); return -1; }
                p++; /* the ')' */
            } else {
                free(name);
                set_err(err, errcap, "unsupported pseudo-class");
                return -1;
            }
            free(name);
            any = 1;
        } else {
            break;
        }
    }
    *pp = p;
    /* A compound that matched nothing means a dangling combinator (`div >`) or a
     * stray character; either way the selector is malformed and guessing would
     * silently match the wrong nodes. */
    if (!any) {
        set_err(err, errcap, "expected a selector here");
        return -1;
    }
    return 0;
}

/* Parse one chain (no top-level commas). */
static int parse_chain(const char **pp, chain *ch, char *err, size_t errcap) {
    const char *p = *pp;
    for (;;) {
        while (*p == ' ') p++;
        compound *g = mi_realloc(ch->compounds, (ch->count + 1) * sizeof *g);
        if (!g) { set_err(err, errcap, "out of memory"); return -1; }
        ch->compounds = g;
        compound *c = &ch->compounds[ch->count];
        memset(c, 0, sizeof *c);
        if (parse_compound(&p, c, err, errcap) != 0) {
            compound_free(c);
            return -1;
        }
        ch->count++;

        while (*p == ' ') p++;
        if (*p == 0 || *p == ',') break;

        combinator comb;
        if (*p == '>') { comb = COMB_CHILD; p++; }
        else if (*p == '+') { comb = COMB_ADJACENT; p++; }
        else if (*p == '~') { comb = COMB_SIBLING; p++; }
        else comb = COMB_DESCENDANT;
        combinator *cg = mi_realloc(ch->combinators, ch->count * sizeof *cg);
        if (!cg) { set_err(err, errcap, "out of memory"); return -1; }
        ch->combinators = cg;
        ch->combinators[ch->count - 1] = comb;
    }
    *pp = p;
    return 0;
}

static void chain_free(chain *ch) {
    for (size_t i = 0; i < ch->count; i++) compound_free(&ch->compounds[i]);
    free(ch->compounds);
    free(ch->combinators);
    memset(ch, 0, sizeof *ch);
}

mi_css_selector *mi_css_compile(const char *selector, char *err, size_t errcap) {
    if (!selector || !*selector) {
        set_err(err, errcap, "empty selector");
        return NULL;
    }
    mi_css_selector *sel = mi_calloc(1, sizeof *sel);
    if (!sel) { set_err(err, errcap, "out of memory"); return NULL; }

    const char *p = selector;
    for (;;) {
        chain *g = mi_realloc(sel->chains, (sel->chain_count + 1) * sizeof *g);
        if (!g) { mi_css_free(sel); set_err(err, errcap, "out of memory"); return NULL; }
        sel->chains = g;
        chain *ch = &sel->chains[sel->chain_count];
        memset(ch, 0, sizeof *ch);
        if (parse_chain(&p, ch, err, errcap) != 0) {
            chain_free(ch);
            mi_css_free(sel);
            return NULL;
        }
        sel->chain_count++;

        while (*p == ' ') p++;
        if (*p == ',') { p++; continue; }
        break;
    }
    return sel;
}

void mi_css_free(mi_css_selector *sel) {
    if (!sel) return;
    for (size_t i = 0; i < sel->chain_count; i++) chain_free(&sel->chains[i]);
    free(sel->chains);
    free(sel);
}

/* ------------------------------------------------------------------ */
/* Matching                                                            */
/* ------------------------------------------------------------------ */

/* Whether `el` is the nth element child of its parent (1-based). */
static int element_index(const mi_html_node *el, int *index_out, int *total_out) {
    int idx = 0, total = 0;
    for (const mi_html_node *c = el->parent ? el->parent->first_child : NULL; c;
         c = c->next_sibling) {
        if (c->type != MI_HTML_ELEMENT) continue;
        total++;
        if (c == el) idx = total;
    }
    *index_out = idx;
    *total_out = total;
    return idx;
}

/* A class list attribute contains this class as a whole word. */
static int class_has(const char *list, const char *cls) {
    if (!list) return 0;
    size_t n = strlen(cls);
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if ((size_t)(p - s) == n && !strncmp(s, cls, n)) return 1;
    }
    return 0;
}

static int attr_matches(const mi_html_node *el, const attr_test *t) {
    const char *v = mi_html_attr(el, t->name);
    if (t->op == ATTR_PRESENT) return v != NULL;
    if (!v) return 0;
    switch (t->op) {
        case ATTR_EQ: return strcmp(v, t->value) == 0;
        case ATTR_PREFIX: {
            size_t n = strlen(t->value);
            return strncmp(v, t->value, n) == 0;
        }
        case ATTR_SUFFIX: {
            size_t n = strlen(t->value), l = strlen(v);
            return l >= n && strcmp(v + l - n, t->value) == 0;
        }
        case ATTR_CONTAINS: return strstr(v, t->value) != NULL;
        case ATTR_WORD: return class_has(v, t->value);
        default: return 0;
    }
}

/* Match one compound against one element. */
static int match_compound(const compound *c, const mi_html_node *el) {
    if (!el || el->type != MI_HTML_ELEMENT) return 0;
    if (c->tag && strcasecmp(c->tag, el->name)) return 0;
    if (c->id) {
        const char *id = mi_html_attr(el, "id");
        if (!id || strcmp(id, c->id)) return 0;
    }
    if (c->class_count) {
        const char *cls = mi_html_attr(el, "class");
        for (size_t i = 0; i < c->class_count; i++) {
            if (!class_has(cls, c->classes[i])) return 0;
        }
    }
    for (size_t i = 0; i < c->attr_count; i++) {
        if (!attr_matches(el, &c->attrs[i])) return 0;
    }
    for (size_t i = 0; i < c->pseudo_count; i++) {
        const pseudo_test *t = &c->pseudos[i];
        int idx = 0, total = 0;
        switch (t->kind) {
            case PSEUDO_FIRST_CHILD:
                element_index(el, &idx, &total);
                if (idx != 1) return 0;
                break;
            case PSEUDO_LAST_CHILD:
                element_index(el, &idx, &total);
                if (idx != total) return 0;
                break;
            case PSEUDO_NTH_CHILD:
                element_index(el, &idx, &total);
                if (idx == 0) return 0;
                if (t->a == 0) { if (idx != t->b) return 0; }
                else {
                    int diff = idx - t->b;
                    if (t->a > 0 ? (diff < 0 || diff % t->a) : (diff > 0 || diff % t->a))
                        return 0;
                }
                break;
            case PSEUDO_NOT:
                if (mi_css_matches(t->not, el)) return 0;
                break;
        }
    }
    return 1;
}

/* Match the compound at index `i` of a chain, walking *left* through the
 * combinators. Right-to-left matching is what makes a descendant combinator
 * cheap: only the subject compound is tested against every node. */
static int match_chain_at(const chain *ch, size_t i, const mi_html_node *el) {
    if (!match_compound(&ch->compounds[i], el)) return 0;
    if (i == 0) return 1;
    combinator comb = ch->combinators[i - 1];
    switch (comb) {
        case COMB_CHILD:
            return el->parent && el->parent->type == MI_HTML_ELEMENT &&
                   match_chain_at(ch, i - 1, el->parent);
        case COMB_DESCENDANT:
            for (const mi_html_node *a = el->parent; a && a->type == MI_HTML_ELEMENT;
                 a = a->parent) {
                if (match_chain_at(ch, i - 1, a)) return 1;
            }
            return 0;
        case COMB_ADJACENT: {
            const mi_html_node *p = el->prev_sibling;
            while (p && p->type != MI_HTML_ELEMENT) p = p->prev_sibling;
            return p && match_chain_at(ch, i - 1, p);
        }
        case COMB_SIBLING:
            for (const mi_html_node *p = el->prev_sibling; p; p = p->prev_sibling) {
                if (p->type != MI_HTML_ELEMENT) continue;
                if (match_chain_at(ch, i - 1, p)) return 1;
            }
            return 0;
        default:
            return 0;
    }
}

int mi_css_matches(const mi_css_selector *sel, const mi_html_node *node) {
    if (!sel || !node || sel->chain_count == 0) return 0;
    for (size_t i = 0; i < sel->chain_count; i++) {
        const chain *ch = &sel->chains[i];
        if (ch->count && match_chain_at(ch, ch->count - 1, node)) return 1;
    }
    return 0;
}

int mi_css_select(const mi_css_selector *sel, const mi_html_node *root,
                  mi_html_node ***out, size_t *count) {
    *out = NULL;
    *count = 0;
    if (!sel || !root) return 0;
    size_t cap = 8, n = 0;
    mi_html_node **items = mi_calloc(cap, sizeof *items);
    if (!items) return -1;
    for (mi_html_node *node = mi_html_next(root, root); node;
         node = mi_html_next(root, node)) {
        if (node->type != MI_HTML_ELEMENT) continue;
        if (!mi_css_matches(sel, node)) continue;
        if (n == cap) {
            size_t want = cap * 2;
            mi_html_node **g = mi_realloc(items, want * sizeof *g);
            if (!g) break;
            items = g;
            cap = want;
        }
        items[n++] = node;
    }
    *out = items;
    *count = n;
    return 0;
}
