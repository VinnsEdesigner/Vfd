/* mi_linkcheck.h - check the links on a page, and report the broken ones.
 *
 * A crawl answers "what files are here"; a link check answers "which of these
 * links are dead". That is a different question and a common one: a site owner
 * wants to know a link 404s before a visitor finds out.
 *
 * The shape is: fetch a page, read its links, and ask each link what it answers.
 * A link is *broken* when the server says it is gone — a 4xx or 5xx — or when it
 * cannot be reached at all. A redirect is not broken (it resolves), but it is
 * reported, because a chain that ends at a login page is a soft break worth
 * seeing.
 *
 * The checks run concurrently: a link check is almost entirely waiting, so a
 * page with a hundred links should not take a hundred round trips in series.
 *
 * Only http(s) is checked. A `mailto:`, a `tel:`, a `javascript:` and a
 * fragment are references, not links to fetch, and are reported as `skipped`
 * rather than silently dropped — a caller should see they were there.
 */
#ifndef MI_LINKCHECK_H
#define MI_LINKCHECK_H

#include <stddef.h>
#include "mi_download.h"

typedef enum {
    MI_LINK_OK = 0,      /* 2xx */
    MI_LINK_REDIRECT,    /* 3xx that resolved to a 2xx */
    MI_LINK_BROKEN,      /* 4xx/5xx */
    MI_LINK_UNREACHABLE, /* no response at all */
    MI_LINK_SKIPPED,     /* not an http(s) URL: mailto, tel, fragment, … */
} mi_link_status;

const char *mi_link_status_name(mi_link_status s);

typedef struct {
    char *url;      /* the link as found, resolved absolute */
    char *found_on; /* the page it was on */
    mi_link_status status;
    int http_status; /* the response code, or 0 */
    char *final_url; /* after redirects, when it redirected */
    char *detail;    /* a sentence for the log; never the body */
    /* The anchor text, so a report reads like the page did. */
    char *title;
} mi_link_result;

typedef struct {
    mi_link_result *results;
    size_t count;
    /* Counters, so a summary is one read. */
    size_t ok, redirect, broken, unreachable, skipped;
} mi_link_report;

void mi_link_report_free(mi_link_report *r);

typedef struct {
    /* How many links to check at once. 0 uses a small default. */
    int concurrency;
    /* Seconds for one link. 0 uses a default. */
    int timeout_seconds;
    /* Follow a redirect to see whether it resolves, rather than reporting the
     * 3xx itself. On by default. */
    int follow_redirects;
    /* The most links to check. 0 uses a default. */
    int max_links;
} mi_linkcheck_options;

/*
 * Check every link on `page_url` (which is fetched first).
 *
 * `cfg` is the fetch configuration (proxy, impersonation, cookies, jar). Returns
 * 0 and fills `out`, or -1 when the page itself cannot be fetched.
 */
int mi_linkcheck_page(const mi_download_config *cfg, const char *page_url,
                      const mi_linkcheck_options *opts, mi_link_report *out);

/*
 * Check a list of URLs directly, without fetching a page first. `urls` is an
 * array of `count` URLs.
 */
int mi_linkcheck_urls(const mi_download_config *cfg, const char *const *urls,
                      size_t count, const mi_linkcheck_options *opts,
                      mi_link_report *out);

/* Render a report as JSON, for the API. Owned by the caller. */
char *mi_link_report_to_json(const mi_link_report *r);

#endif
