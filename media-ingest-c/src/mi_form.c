#define _GNU_SOURCE
#include "mi_form.h"
#include "mi_json.h"
#include "mi_util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void mi_forms_free(mi_forms *f) {
    if (!f) return;
    for (size_t i = 0; i < f->count; i++) {
        mi_form *fm = &f->forms[i];
        free(fm->action);
        free(fm->method);
        free(fm->enctype);
        free(fm->id);
        free(fm->name);
        for (size_t j = 0; j < fm->field_count; j++) {
            free(fm->fields[j].name);
            free(fm->fields[j].value);
        }
        free(fm->fields);
    }
    free(f->forms);
    memset(f, 0, sizeof *f);
}

/* Resolve `ref` against `base`. A copy of the browse module's rule. */
static char *resolve_url(const char *base, const char *ref) {
    if (!ref) ref = "";
    if (!*ref) return base ? mi_strdup(base) : mi_strdup("");
    if (strstr(ref, "://") || !strncmp(ref, "//", 2)) return mi_strdup(ref);
    if (!base || !*base) return mi_strdup(ref);
    if (*ref == '#') return mi_strdup(base);
    if (*ref == '/') {
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
    const char *slash = strrchr(base, '/');
    if (!slash) return mi_strdup(ref);
    size_t dir = (size_t)(slash - base) + 1;
    char *out = mi_malloc(dir + strlen(ref) + 1);
    if (!out) return NULL;
    memcpy(out, base, dir);
    strcpy(out + dir, ref);
    return out;
}

static mi_form_field_kind kind_of(const char *type) {
    if (!type || !*type) return MI_FIELD_TEXT;
    if (!strcasecmp(type, "password")) return MI_FIELD_PASSWORD;
    if (!strcasecmp(type, "hidden")) return MI_FIELD_HIDDEN;
    if (!strcasecmp(type, "email")) return MI_FIELD_EMAIL;
    if (!strcasecmp(type, "submit")) return MI_FIELD_SUBMIT;
    if (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio"))
        return MI_FIELD_CHECKBOX;
    if (!strcasecmp(type, "file")) return MI_FIELD_FILE;
    if (!strcasecmp(type, "text") || !strcasecmp(type, "search")) return MI_FIELD_TEXT;
    return MI_FIELD_OTHER;
}

static void add_field(mi_form *fm, const char *name, const char *value,
                      mi_form_field_kind kind, int checked) {
    if (!name || !*name) return; /* an unnamed field is not submitted */
    size_t n = fm->field_count + 1;
    mi_form_field *g = mi_realloc(fm->fields, n * sizeof *g);
    if (!g) return;
    fm->fields = g;
    mi_form_field *f = &fm->fields[fm->field_count++];
    f->name = mi_strdup(name);
    f->value = mi_strdup(value ? value : "");
    f->kind = kind;
    f->checked = checked;
}

/* The text of a <textarea>, for its default value. */
static char *textarea_value(const mi_html_node *ta) {
    return mi_html_own_text(ta);
}

/* Collect a <select>'s chosen option (the one with `selected`, else the first). */
static char *select_value(const mi_html_node *sel) {
    const mi_html_node *first = NULL;
    for (const mi_html_node *o = sel->first_child; o; o = o->next_sibling) {
        if (!mi_html_is(o, "option")) continue;
        if (!first) first = o;
        if (mi_html_attr(o, "selected")) {
            const char *v = mi_html_attr(o, "value");
            if (v) return mi_strdup(v);
            return mi_html_own_text(o);
        }
    }
    if (first) {
        const char *v = mi_html_attr(first, "value");
        if (v) return mi_strdup(v);
        return mi_html_own_text(first);
    }
    return NULL;
}

/* Walk one form's subtree, collecting its fields. */
static void collect_fields(const mi_html_node *n, mi_form *fm) {
    for (; n; n = n->next_sibling) {
        if (n->type != MI_HTML_ELEMENT) continue;
        if (mi_html_is(n, "input")) {
            const char *type = mi_html_attr(n, "type");
            mi_form_field_kind k = kind_of(type);
            const char *value = mi_html_attr(n, "value");
            int checked = mi_html_attr(n, "checked") != NULL;
            /* A checkbox/radio that is not checked is not submitted, but it is
             * still reported so a caller can see it exists. */
            if (k == MI_FIELD_CHECKBOX && !checked) {
                /* Keep it, marked unchecked. */
            }
            add_field(fm, mi_html_attr(n, "name"), value ? value : "", k, checked);
        } else if (mi_html_is(n, "textarea")) {
            char *v = textarea_value(n);
            add_field(fm, mi_html_attr(n, "name"), v ? v : "", MI_FIELD_TEXT, 1);
            free(v);
        } else if (mi_html_is(n, "select")) {
            char *v = select_value(n);
            add_field(fm, mi_html_attr(n, "name"), v ? v : "", MI_FIELD_OTHER, 1);
            free(v);
        } else if (mi_html_is(n, "button")) {
            /* A <button> submits with its name/value when it is the submitter,
             * but which one was clicked is the caller's choice; report it. */
            const char *type = mi_html_attr(n, "type");
            if (!type || !strcasecmp(type, "submit"))
                add_field(fm, mi_html_attr(n, "name"), mi_html_attr(n, "value"),
                          MI_FIELD_SUBMIT, 1);
        }
        if (n->first_child) collect_fields(n->first_child, fm);
    }
}

int mi_forms_extract(const mi_html_node *doc, const char *page_url, mi_forms *out) {
    memset(out, 0, sizeof *out);
    if (!doc) return 0;
    for (const mi_html_node *n = mi_html_next(doc, doc); n; n = mi_html_next(doc, n)) {
        if (!mi_html_is(n, "form")) continue;
        size_t idx = out->count;
        mi_form *g = mi_realloc(out->forms, (idx + 1) * sizeof *g);
        if (!g) break;
        out->forms = g;
        mi_form *fm = &out->forms[idx];
        memset(fm, 0, sizeof *fm);
        out->count++;

        const char *action = mi_html_attr(n, "action");
        fm->action = resolve_url(page_url, action ? action : "");
        const char *method = mi_html_attr(n, "method");
        fm->method = mi_strdup(method && *method ? method : "get");
        for (char *q = fm->method; *q; q++) *q = (char)tolower((unsigned char)*q);
        const char *enctype = mi_html_attr(n, "enctype");
        if (enctype) fm->enctype = mi_strdup(enctype);
        const char *id = mi_html_attr(n, "id");
        if (id) fm->id = mi_strdup(id);
        const char *name = mi_html_attr(n, "name");
        if (name) fm->name = mi_strdup(name);

        collect_fields(n->first_child, fm);
    }
    return 0;
}

/* Whether a field name looks like a username/email. */
static int looks_like_user(const char *name) {
    if (!name) return 0;
    static const char *N[] = {"user", "username", "email", "login", "account",
                              "identifier", "uname", NULL};
    for (size_t i = 0; N[i]; i++)
        if (strcasestr(name, N[i])) return 1;
    return 0;
}

const mi_form *mi_forms_find_login(const mi_forms *forms) {
    if (!forms) return NULL;
    const mi_form *best = NULL;
    int best_score = 0;
    for (size_t i = 0; i < forms->count; i++) {
        const mi_form *fm = &forms->forms[i];
        int has_password = 0, has_user = 0;
        for (size_t j = 0; j < fm->field_count; j++) {
            if (fm->fields[j].kind == MI_FIELD_PASSWORD) has_password = 1;
            if (looks_like_user(fm->fields[j].name)) has_user = 1;
        }
        if (!has_password) continue;
        int score = 1 + has_user;
        if (score > best_score) { best_score = score; best = fm; }
    }
    return best;
}

/* The hidden-field names that are usually a CSRF token. */
static int looks_like_csrf(const char *name) {
    if (!name) return 0;
    static const char *N[] = {"csrf", "xsrf", "_token", "authenticity_token",
                              "requestverificationtoken", "token", NULL};
    for (size_t i = 0; N[i]; i++)
        if (strcasestr(name, N[i])) return 1;
    return 0;
}

const char *mi_form_csrf_token(const mi_form *form, char **name_out) {
    if (name_out) *name_out = NULL;
    if (!form) return NULL;
    /* A hidden field whose name looks like a token, longest match preferred by
     * checking the specific names first. */
    for (size_t i = 0; i < form->field_count; i++) {
        const mi_form_field *f = &form->fields[i];
        if (f->kind == MI_FIELD_HIDDEN && looks_like_csrf(f->name) && *f->value) {
            if (name_out) *name_out = f->name;
            return f->value;
        }
    }
    return NULL;
}

/* Percent-encode for an application/x-www-form-urlencoded body: a space is '+',
 * and the reserved set is encoded. */
static void url_encode_into(mi_json_buf *b, const char *s) {
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            char c[2] = {(char)*p, 0};
            mi_json_raw(b, c);
        } else if (*p == ' ') {
            mi_json_raw(b, "+");
        } else {
            char esc[4];
            snprintf(esc, sizeof esc, "%%%02X", *p);
            mi_json_raw(b, esc);
        }
    }
}

char *mi_form_encode(const mi_form *form, const mi_form_override *overrides,
                     size_t override_count) {
    if (!form) return NULL;
    mi_json_buf b;
    mi_json_init(&b);
    int first = 1;

    /* Track which overrides were used, so an override for a field the form did
     * not have is appended once. */
    int *used = mi_calloc(override_count ? override_count : 1, sizeof *used);

    for (size_t i = 0; i < form->field_count; i++) {
        const mi_form_field *f = &form->fields[i];
        /* A checkbox/radio that is not checked is not submitted. A submit button
         * is only submitted when it was the one pressed, which the caller
         * decides; it is skipped by default. */
        if (f->kind == MI_FIELD_CHECKBOX && !f->checked) continue;
        if (f->kind == MI_FIELD_SUBMIT) continue;

        const char *value = f->value;
        for (size_t k = 0; k < override_count; k++) {
            if (!strcmp(overrides[k].name, f->name)) {
                value = overrides[k].value;
                used[k] = 1;
                break;
            }
        }
        if (!first) mi_json_raw(&b, "&");
        first = 0;
        url_encode_into(&b, f->name);
        mi_json_raw(&b, "=");
        url_encode_into(&b, value);
    }
    /* An override for a field the form did not have is appended. */
    for (size_t k = 0; k < override_count; k++) {
        if (used[k]) continue;
        if (!first) mi_json_raw(&b, "&");
        first = 0;
        url_encode_into(&b, overrides[k].name);
        mi_json_raw(&b, "=");
        url_encode_into(&b, overrides[k].value);
    }
    free(used);
    char *out = mi_strdup(mi_json_data(&b));
    mi_json_free(&b);
    return out;
}
