#define _GNU_SOURCE
#include "mi_xpath.h"
#include "mi_util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* The compiled form                                                   */
/* ------------------------------------------------------------------ */

typedef enum { PRED_HAS_ATTR, PRED_ATTR_EQ, PRED_CONTAINS, PRED_STARTS_WITH,
               PRED_POSITION, PRED_LAST } pred_kind;

typedef struct {
    pred_kind kind;
    char *attr;      /* for the attribute predicates */
    char *value;     /* for the value predicates */
    int position;    /* for PRED_POSITION (1-based) */
} predicate;

typedef struct {
    char *name;       /* element name, or "*" */
    predicate *preds;
    size_t pred_count;
} step;

struct mi_xpath {
    step *steps;
    size_t count;
    int absolute;     /* starts with '/' */
    int descendant;   /* the first step is `//` (any descendant) */
    /* The value selectors, which are not steps. */
    int is_attribute;
    char *result_attr;
    int is_text;
};

static char g_last_error[160];
const char *mi_xpath_last_error(void) { return g_last_error; }

static void set_err(char *err, size_t cap, const char *msg) {
    snprintf(g_last_error, sizeof g_last_error, "%s", msg);
    if (err && cap) snprintf(err, cap, "%s", msg);
}

static int is_name_char(int c) { return isalnum(c) || c == '_' || c == '-' || c == ':'; }

static char *read_name(const char **pp) {
    const char *p = *pp;
    const char *start = p;
    while (*p && is_name_char((unsigned char)*p)) p++;
    if (p == start) return NULL;
    *pp = p;
    return mi_strndup(start, (size_t)(p - start));
}

/* Parse a predicate `[...]`. */
static int parse_predicate(const char **pp, predicate *out, char *err, size_t errcap) {
    const char *p = *pp;
    memset(out, 0, sizeof *out);
    while (*p == ' ') p++;
    if (*p == '@') {
        p++;
        out->attr = read_name(&p);
        if (!out->attr) { set_err(err, errcap, "expected attribute after '@'"); return -1; }
        while (*p == ' ') p++;
        if (*p == ']') { out->kind = PRED_HAS_ATTR; *pp = p; return 0; }
        if (*p != '=') { set_err(err, errcap, "expected '=' in predicate"); return -1; }
        p++;
        while (*p == ' ') p++;
        char quote = *p;
        if (quote != '\'' && quote != '"') { set_err(err, errcap, "expected a quoted value"); return -1; }
        p++;
        const char *start = p;
        while (*p && *p != quote) p++;
        if (*p != quote) { set_err(err, errcap, "unterminated value"); return -1; }
        out->value = mi_strndup(start, (size_t)(p - start));
        out->kind = PRED_ATTR_EQ;
        *pp = p + 1; /* past the closing quote */
        return 0;
    }
    if (!strncmp(p, "contains(", 9)) {
        p += 9;
        while (*p == ' ') p++;
        if (*p != '@') { set_err(err, errcap, "contains() takes an attribute"); return -1; }
        p++;
        out->attr = read_name(&p);
        while (*p == ' ') p++;
        if (*p != ',') { set_err(err, errcap, "contains() needs a comma"); return -1; }
        p++;
        while (*p == ' ') p++;
        char quote = *p;
        if (quote != '\'' && quote != '"') { set_err(err, errcap, "expected a quoted value"); return -1; }
        p++;
        const char *start = p;
        while (*p && *p != quote) p++;
        out->value = mi_strndup(start, (size_t)(p - start));
        if (*p) p++;
        while (*p == ' ') p++;
        if (*p != ')') { set_err(err, errcap, "expected ')'"); return -1; }
        out->kind = PRED_CONTAINS;
        *pp = p + 1; /* past the ')' */
        return 0;
    }
    if (!strncmp(p, "starts-with(", 12)) {
        p += 12;
        while (*p == ' ') p++;
        if (*p != '@') { set_err(err, errcap, "starts-with() takes an attribute"); return -1; }
        p++;
        out->attr = read_name(&p);
        while (*p == ' ') p++;
        if (*p != ',') { set_err(err, errcap, "starts-with() needs a comma"); return -1; }
        p++;
        while (*p == ' ') p++;
        char quote = *p;
        if (quote != '\'' && quote != '"') { set_err(err, errcap, "expected a quoted value"); return -1; }
        p++;
        const char *start = p;
        while (*p && *p != quote) p++;
        out->value = mi_strndup(start, (size_t)(p - start));
        if (*p) p++;
        while (*p == ' ') p++;
        if (*p != ')') { set_err(err, errcap, "expected ')'"); return -1; }
        out->kind = PRED_STARTS_WITH;
        *pp = p + 1; /* past the ')' */
        return 0;
    }
    if (!strncmp(p, "last()", 6)) {
        out->kind = PRED_LAST;
        *pp = p + 6;
        return 0;
    }
    if (isdigit((unsigned char)*p)) {
        int v = 0;
        while (isdigit((unsigned char)*p)) v = v * 10 + (*p++ - '0');
        out->kind = PRED_POSITION;
        out->position = v;
        *pp = p;
        return 0;
    }
    set_err(err, errcap, "unsupported predicate");
    return -1;
}

mi_xpath *mi_xpath_compile(const char *expr, char *err, size_t errcap) {
    if (!expr || !*expr) { set_err(err, errcap, "empty expression"); return NULL; }
    mi_xpath *x = mi_calloc(1, sizeof *x);
    if (!x) { set_err(err, errcap, "out of memory"); return NULL; }

    const char *p = expr;
    if (*p == '/') {
        x->absolute = 1;
        p++;
        if (*p == '/') { x->descendant = 1; p++; }
    }
    if (!*p) { set_err(err, errcap, "no steps"); mi_xpath_free(x); return NULL; }

    while (*p) {
        while (*p == ' ') p++;
        if (*p == 0) break;

        /* The value selectors end the expression. */
        if (!strncmp(p, "@", 1)) {
            p++;
            x->result_attr = read_name(&p);
            if (!x->result_attr) { set_err(err, errcap, "expected attribute name"); mi_xpath_free(x); return NULL; }
            x->is_attribute = 1;
            while (*p == ' ') p++;
            if (*p) { set_err(err, errcap, "trailing text after attribute"); mi_xpath_free(x); return NULL; }
            break;
        }
        if (!strncmp(p, "text()", 6)) {
            x->is_text = 1;
            p += 6;
            while (*p == ' ') p++;
            if (*p) { set_err(err, errcap, "trailing text after text()"); mi_xpath_free(x); return NULL; }
            break;
        }

        step *g = mi_realloc(x->steps, (x->count + 1) * sizeof *g);
        if (!g) { set_err(err, errcap, "out of memory"); mi_xpath_free(x); return NULL; }
        x->steps = g;
        step *s = &x->steps[x->count];
        memset(s, 0, sizeof *s);
        if (*p == '*') { s->name = mi_strdup("*"); p++; }
        else {
            s->name = read_name(&p);
            if (!s->name) { set_err(err, errcap, "expected an element name"); mi_xpath_free(x); return NULL; }
        }
        /* Predicates. */
        while (*p == '[') {
            p++;
            predicate *pg = mi_realloc(s->preds, (s->pred_count + 1) * sizeof *pg);
            if (!pg) { set_err(err, errcap, "out of memory"); mi_xpath_free(x); return NULL; }
            s->preds = pg;
            predicate *pr = &s->preds[s->pred_count];
            if (parse_predicate(&p, pr, err, errcap) != 0) { mi_xpath_free(x); return NULL; }
            s->pred_count++;
            while (*p == ' ') p++;
            if (*p != ']') { set_err(err, errcap, "expected ']'"); mi_xpath_free(x); return NULL; }
            p++;
        }
        x->count++;
        while (*p == ' ') p++;
        if (*p == '/') {
            p++;
            if (*p == '/') { set_err(err, errcap, "only the first step may be '//'"); mi_xpath_free(x); return NULL; }
        } else if (*p) {
            set_err(err, errcap, "unexpected character in path");
            mi_xpath_free(x);
            return NULL;
        }
    }
    if (x->count == 0 && !x->is_attribute && !x->is_text) {
        set_err(err, errcap, "no steps");
        mi_xpath_free(x);
        return NULL;
    }
    return x;
}

void mi_xpath_free(mi_xpath *x) {
    if (!x) return;
    for (size_t i = 0; i < x->count; i++) {
        free(x->steps[i].name);
        for (size_t j = 0; j < x->steps[i].pred_count; j++) {
            free(x->steps[i].preds[j].attr);
            free(x->steps[i].preds[j].value);
        }
        free(x->steps[i].preds);
    }
    free(x->steps);
    free(x->result_attr);
    free(x);
}

int mi_xpath_is_attribute(const mi_xpath *x) { return x && x->is_attribute; }
const char *mi_xpath_result_attribute(const mi_xpath *x) { return x ? x->result_attr : NULL; }
int mi_xpath_is_text(const mi_xpath *x) { return x && x->is_text; }

/* ------------------------------------------------------------------ */
/* Matching                                                            */
/* ------------------------------------------------------------------ */

/* Whether a node satisfies one predicate. `index`/`total` are the node's
 * 1-based position among its same-name siblings, for position predicates. */
static int pred_ok(const predicate *p, const mi_html_node *n, int index, int total) {
    switch (p->kind) {
        case PRED_HAS_ATTR: return mi_html_attr(n, p->attr) != NULL;
        case PRED_ATTR_EQ: {
            const char *v = mi_html_attr(n, p->attr);
            return v && !strcmp(v, p->value);
        }
        case PRED_CONTAINS: {
            const char *v = mi_html_attr(n, p->attr);
            return v && strstr(v, p->value) != NULL;
        }
        case PRED_STARTS_WITH: {
            const char *v = mi_html_attr(n, p->attr);
            return v && !strncmp(v, p->value, strlen(p->value));
        }
        case PRED_POSITION: return index == p->position;
        case PRED_LAST: return index == total;
        default: return 0;
    }
}

/* Whether an element matches a step's name and predicates. The position is
 * computed among siblings with the same name, which is what `[1]` means. */
static int step_matches(const step *s, const mi_html_node *n) {
    if (n->type != MI_HTML_ELEMENT) return 0;
    if (strcmp(s->name, "*") && strcasecmp(s->name, n->name)) return 0;
    if (s->pred_count == 0) return 1;
    /* Position among same-name siblings. */
    int index = 0, total = 0;
    for (const mi_html_node *c = n->parent ? n->parent->first_child : NULL; c;
         c = c->next_sibling) {
        if (c->type != MI_HTML_ELEMENT) continue;
        if (strcmp(s->name, "*") && strcasecmp(s->name, c->name)) continue;
        total++;
        if (c == n) index = total;
    }
    for (size_t i = 0; i < s->pred_count; i++) {
        if (!pred_ok(&s->preds[i], n, index, total)) return 0;
    }
    return 1;
}

/* Apply step `si` to the current node set, producing the next set. The first
 * step may be a descendant search; later steps are children. */
static int apply_step(const mi_xpath *x, size_t si, mi_html_node **in, size_t in_n,
                      mi_html_node ***out, size_t *out_n) {
    size_t cap = 8, n = 0;
    mi_html_node **res = mi_calloc(cap, sizeof *res);
    if (!res) return -1;
    const step *s = &x->steps[si];
    int descendant = (si == 0 && x->descendant);

    for (size_t i = 0; i < in_n; i++) {
        if (descendant) {
            for (mi_html_node *node = mi_html_next(in[i], in[i]); node;
                 node = mi_html_next(in[i], node)) {
                if (step_matches(s, node)) {
                    if (n == cap) { cap *= 2; mi_html_node **g = mi_realloc(res, cap * sizeof *g); if (!g) { free(res); return -1; } res = g; }
                    res[n++] = node;
                }
            }
        } else {
            for (mi_html_node *c = in[i]->first_child; c; c = c->next_sibling) {
                if (step_matches(s, c)) {
                    if (n == cap) { cap *= 2; mi_html_node **g = mi_realloc(res, cap * sizeof *g); if (!g) { free(res); return -1; } res = g; }
                    res[n++] = c;
                }
            }
        }
    }
    *out = res;
    *out_n = n;
    return 0;
}

int mi_xpath_select(const mi_xpath *x, const mi_html_node *root,
                    mi_html_node ***out, size_t *count) {
    *out = NULL;
    *count = 0;
    if (!x || !root) return 0;

    mi_html_node **cur = mi_malloc(sizeof *cur);
    if (!cur) return -1;
    cur[0] = (mi_html_node *)root;
    size_t cur_n = 1;

    for (size_t si = 0; si < x->count; si++) {
        mi_html_node **next = NULL;
        size_t next_n = 0;
        if (apply_step(x, si, cur, cur_n, &next, &next_n) != 0) { free(cur); return -1; }
        free(cur);
        cur = next;
        cur_n = next_n;
    }
    *out = cur;
    *count = cur_n;
    return 0;
}
