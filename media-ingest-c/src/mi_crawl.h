/* mi_crawl.h - walk a site and find files matching a name.
 *
 * WHAT THIS IS FOR
 *
 * "I want the STM32 datasheet from alldatasheets.com." The user gives a site
 * and a search term, not a URL to a file. The file may be several clicks in, and
 * the page that links it may be built by JavaScript.
 *
 * So this walks the site: it fetches pages, follows their links, and reports
 * every file it finds whose name matches the term. It is a *crawler with a
 * filter*, not a downloader — nothing is fetched but HTML, and the caller gets
 * back the URLs a download would use.
 *
 * TWO WAYS TO READ A PAGE
 *
 *   static   fetch the HTML and read its links. Cheap, fast, and right for the
 *            majority of sites.
 *   rendered if the static pass finds nothing, the page is re-read through the
 *            browser, so links that only exist after the scripts have run are
 *            seen too. This costs a browser, so it is used as a fallback rather
 *            than for every page.
 *
 * The fallback is the whole point: a site whose links are assembled in
 * JavaScript looks empty to a plain fetch, and that is exactly the case a
 * crawler has to handle rather than report as "not found".
 */
#ifndef MI_CRAWL_H
#define MI_CRAWL_H

#include <stddef.h>
#include <stdint.h>

/* A file the crawl found. */
typedef struct {
    char *url;      /* absolute */
    char *filename; /* the name a save should use */
    /* The link text, when the reference came from an anchor. This is what a
     * person would read, and is usually a better label than the filename. */
    char *title;
    char *mime;     /* guessed from the extension; may be "" */
    /* How well the name matched the term, 0-100. Lets the caller show the
     * likely answer first instead of a wall of maybes. */
    int score;
    /* The page this was found on, so a result can be traced back. */
    char *found_on;
    /* 1 when this page had to be rendered to see the link. */
    int via_browser;
} mi_crawl_hit;

/* One page the crawl visited, and what it yielded. Kept so the caller can
 * explain *why* nothing was found: "40 pages read, 12 needed the browser, no
 * file matched" is a different answer from "that host refused us". */
typedef struct {
    char *url;
    int links_seen;
    int hits;
    int rendered;   /* 1 when the browser was needed */
    int status;     /* HTTP status, or 0 when the fetch failed */
} mi_crawl_page;

typedef struct {
    mi_crawl_hit *hits;
    size_t hit_count;
    mi_crawl_page *pages;
    size_t page_count;
    /* Counters, so a caller can report what the walk actually did. */
    int fetched;
    int failed;
    int rendered;
    int skipped_offsite;
} mi_crawl_result;

void mi_crawl_result_free(mi_crawl_result *res);

typedef struct {
    /*
     * How deep to follow links from the start page.
     *
     * 0 reads only the page given. 1 also reads the pages it links. Two or
     * three is usually enough to reach a file listing, and every extra level
     * multiplies the requests, so this is deliberately small by default.
     */
    int max_depth;
    /* The most pages to read, a hard stop on a site with a large index. */
    int max_pages;
    /* The most hits to collect. Once this many match, the walk stops early:
     * the caller has what it came for. */
    int max_hits;
    /* Seconds allowed for the whole walk. A crawl is a background job with a
     * human waiting; it must not run forever. */
    double budget_seconds;
    /* Only follow links on the start page's host. Off by default: a datasheet
     * host commonly links its files from a CDN on another host. */
    int same_host_only;
    /*
     * Whether a page with no matching links may be re-read through the browser.
     *
     * On by default, and the reason this module exists: a JavaScript-built
     * listing yields nothing to a plain fetch, so without this the crawl
     * reports "not found" for a site that plainly has the file.
     */
    int use_browser_fallback;
    /* The User-Agent to send. NULL uses the built-in default. */
    const char *user_agent;
    /*
     * The browser pool to render with, or NULL to never render.
     *
     * Passed in rather than owned: the pool is Vyzor's single browser gate, and
     * a crawl must share it rather than start a second one.
     */
    struct mi_render_pool *render_pool;
} mi_crawl_config;

void mi_crawl_config_init(mi_crawl_config *cfg);

/*
 * Walk `site_url` looking for files whose name matches `term`.
 *
 * `term` may be NULL or empty to list every file found. Matching is
 * case-insensitive and ignores the separators people vary in a filename, so
 * "stm32 datasheet", "stm32-datasheet" and "STM32_Datasheet" all match
 * "stm32datasheet.pdf".
 *
 * Returns MI_OK even when nothing matched — "the walk completed and found
 * nothing" is a result, not an error. A negative MI_ERR_* means the walk could
 * not run at all (bad URL, no pages reachable).
 */
int mi_crawl(const mi_crawl_config *cfg, const char *site_url, const char *term,
             mi_crawl_result *out);

/*
 * How well `filename` matches `term`, 0-100.
 *
 * Exposed for testing: the scoring is the part most likely to be subtly wrong,
 * and it decides what a user sees first.
 */
int mi_crawl_score(const char *filename, const char *title, const char *term);

#endif /* MI_CRAWL_H */
