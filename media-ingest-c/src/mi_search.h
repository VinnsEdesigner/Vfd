/* mi_search.h - submit a site's own search form, and read the results.
 *
 * A crawler that only follows links misses a site whose content is behind a
 * search box: a directory, a store, a forum. The site already has the search —
 * this finds the form, types the term into it, submits it, and returns the
 * resulting URLs.
 *
 * The pieces are already here: `mi_form` reads the form and its hidden fields
 * (including any CSRF token, which the search form often carries), and the fetch
 * layer posts it. This is the glue, plus the one decision that matters — which
 * field is the search box.
 *
 * A GET search form puts the term in the query and is the common case; a POST
 * search form is submitted with a body. Both are handled, because the form says
 * which it is.
 */
#ifndef MI_SEARCH_H
#define MI_SEARCH_H

#include <stddef.h>
#include "mi_download.h"

typedef struct {
    /* The field to type the term into. NULL uses the heuristic: a text/search
     * field named like a query ("q", "query", "search", "s", "term", "keyword"). */
    const char *query_field;
    /* Extra fields to send alongside, e.g. a category or a language. */
    const char *extra_name;
    const char *extra_value;
    /* The most result links to return. 0 uses a default. */
    int max_results;
} mi_search_options;

typedef struct {
    /* The result URLs, in the order the page listed them, absolute. */
    char **urls;
    size_t count;
    /* The page the search ended on, after any redirect. */
    char *result_page;
    /* The search form's action, for reporting. */
    char *action;
    /* A sentence for the log. */
    char *detail;
} mi_search_result;

void mi_search_result_free(mi_search_result *r);

/*
 * Find the search form on `page_url`, submit `term`, and return the result URLs.
 *
 * `cfg` is the fetch configuration (proxy, impersonation, cookies, jar). A site
 * whose search needs a login uses the jar, so logging in first and searching
 * second works. Returns 0 and fills `out`; -1 when the page cannot be fetched,
 * or the page has no search form (with a detail saying so).
 */
int mi_search_site(const mi_download_config *cfg, const char *page_url,
                   const char *term, const mi_search_options *opts,
                   mi_search_result *out);

#endif
