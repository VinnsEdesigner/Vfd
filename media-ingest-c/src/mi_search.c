#define _GNU_SOURCE
#include "mi_search.h"
#include "mi_browse.h"
#include "mi_form.h"
#include "mi_html.h"
#include "mi_util.h"

#include <stdlib.h>
#include <string.h>

void mi_search_result_free(mi_search_result *r) {
    if (!r) return;
    for (size_t i = 0; i < r->count; i++) free(r->urls[i]);
    free(r->urls);
    free(r->result_page);
    free(r->action);
    free(r->detail);
    memset(r, 0, sizeof *r);
}

/* The field a search term belongs in. */
static int looks_like_query(const char *name) {
    if (!name) return 0;
    static const char *N[] = {"q", "query", "search", "s", "term", "keyword",
                              "kw", "text", NULL};
    for (size_t i = 0; N[i]; i++)
        if (!strcasecmp(name, N[i])) return 1;
    /* A name that *contains* one of the words, e.g. "search_query". */
    for (size_t i = 0; N[i]; i++)
        if (strcasestr(name, N[i])) return 1;
    return 0;
}

/* Whether a form is a search form: a text field, and no password. */
static int is_search_form(const mi_form *fm) {
    int has_text = 0;
    for (size_t i = 0; i < fm->field_count; i++) {
        if (fm->fields[i].kind == MI_FIELD_PASSWORD) return 0;
        if (fm->fields[i].kind == MI_FIELD_TEXT ||
            fm->fields[i].kind == MI_FIELD_EMAIL)
            has_text = 1;
    }
    return has_text;
}

/* The form and the field to search with. */
static const mi_form *find_search(const mi_forms *forms, const mi_search_options *opts,
                                  const char **field_out) {
    for (size_t i = 0; i < forms->count; i++) {
        const mi_form *fm = &forms->forms[i];
        if (!is_search_form(fm)) continue;
        /* An explicit field name, when the caller knows it. */
        if (opts && opts->query_field) {
            for (size_t j = 0; j < fm->field_count; j++) {
                if (!strcmp(fm->fields[j].name, opts->query_field)) {
                    *field_out = fm->fields[j].name;
                    return fm;
                }
            }
            continue;
        }
        /* A field named like a query. */
        for (size_t j = 0; j < fm->field_count; j++) {
            if (looks_like_query(fm->fields[j].name)) {
                *field_out = fm->fields[j].name;
                return fm;
            }
        }
    }
    /* No named query field: fall back to the first text field of the first
     * search-shaped form. */
    for (size_t i = 0; i < forms->count; i++) {
        const mi_form *fm = &forms->forms[i];
        if (!is_search_form(fm)) continue;
        for (size_t j = 0; j < fm->field_count; j++) {
            if (fm->fields[j].kind == MI_FIELD_TEXT ||
                fm->fields[j].kind == MI_FIELD_EMAIL) {
                *field_out = fm->fields[j].name;
                return fm;
            }
        }
    }
    return NULL;
}

int mi_search_site(const mi_download_config *cfg, const char *page_url,
                   const char *term, const mi_search_options *opts,
                   mi_search_result *out) {
    memset(out, 0, sizeof *out);
    if (!cfg || !page_url || !term) return -1;

    mi_download_config c = *cfg;
    c.require_media = 0;

    /* 1. The page with the search form. */
    mi_fetch_info info;
    char *page = mi_download_fetch_document(&c, page_url, 4 * 1024 * 1024, &info);
    char *base = info.final_url ? mi_strdup(info.final_url) : mi_strdup(page_url);
    mi_fetch_info_free(&info);
    if (!page) {
        out->detail = mi_strdup("could not fetch the page");
        free(base);
        return -1;
    }

    mi_html_node *doc = mi_html_parse(page, strlen(page));
    mi_forms forms;
    mi_forms_extract(doc, base, &forms);
    const char *field = NULL;
    const mi_form *form = find_search(&forms, opts, &field);
    if (!form) {
        mi_forms_free(&forms);
        mi_html_free(doc);
        free(page);
        free(base);
        out->detail = mi_strdup("the page has no search form");
        return -1;
    }
    out->action = mi_strdup(form->action);

    /* 2. Build the body: the term in the query field, an extra when given, and
     * every hidden field (the CSRF token included) at the page's value. */
    mi_form_override ov[2];
    size_t ovn = 0;
    ov[ovn].name = field;
    ov[ovn].value = term;
    ovn++;
    if (opts && opts->extra_name && opts->extra_value) {
        ov[ovn].name = opts->extra_name;
        ov[ovn].value = opts->extra_value;
        ovn++;
    }
    char *body = mi_form_encode(form, ov, ovn);
    char *action = mi_strdup(form->action);
    int is_post = strcasecmp(form->method, "post") == 0;
    mi_forms_free(&forms);
    mi_html_free(doc);
    free(page);

    /* 3. Submit. */
    char *result = NULL;
    if (is_post) {
        result = mi_download_post(&c, action, body, "application/x-www-form-urlencoded",
                                  4 * 1024 * 1024, &info);
    } else {
        char *sep = strchr(action, '?') ? "&" : "?";
        char *full = mi_asprintf("%s%s%s", action, sep, body);
        result = mi_download_fetch_document(&c, full, 4 * 1024 * 1024, &info);
        free(full);
    }
    free(body);
    if (!result) {
        mi_fetch_info_free(&info);
        out->detail = mi_asprintf("the search could not be submitted to %s", action);
        free(action);
        free(base);
        return -1;
    }
    out->result_page = info.final_url ? mi_strdup(info.final_url) : mi_strdup(action);
    mi_fetch_info_free(&info);
    free(action);

    /* 4. The result links. */
    mi_browse_result br;
    if (mi_browse_parse(result, strlen(result), out->result_page, MI_BROWSE_ALL, &br) == MI_OK) {
        int max = opts && opts->max_results > 0 ? opts->max_results : 100;
        size_t cap = br.count ? br.count : 1;
        out->urls = mi_calloc(cap, sizeof *out->urls);
        for (size_t i = 0; i < br.count && (int)out->count < max; i++) {
            if (!br.items[i].url) continue;
            int dup = 0;
            for (size_t j = 0; j < out->count; j++)
                if (!strcmp(out->urls[j], br.items[i].url)) { dup = 1; break; }
            if (dup) continue;
            out->urls[out->count++] = mi_strdup(br.items[i].url);
        }
        mi_browse_result_free(&br);
    }
    free(result);
    free(base);
    out->detail = mi_asprintf("submitted '%s' to %s; %zu result link(s)", term,
                              out->result_page ? out->result_page : "?",
                              out->count);
    return 0;
}
