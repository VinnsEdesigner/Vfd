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

/* One selectable quality of a resolved stream. */
typedef struct {
    int height;
    /* A label to show, e.g. "1080p" or "audio only". */
    char *label;
    /* Bytes for this choice, or -1 when the extractor did not say. */
    int64_t bytes;
    /* 1 when this is the largest offered. */
    int best;
} mi_crawl_quality;

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
    /*
     * 1 when a media extractor resolved this page, so the hit is the *media*
     * rather than a link the walk found.
     *
     * Worth distinguishing because a streaming page's own links match a search
     * just as well as its video does, and the extractor's answer is the one the
     * caller asked for — so it ranks first at an equal score.
     */
    int resolved;
    /*
     * The qualities a resolved stream offers, largest first, or none.
     *
     * A stream is not one file: it is a set of renditions, and which one to take
     * is the viewer's decision — a phone on mobile data does not want the 4K
     * master. Reported so the console can offer the choice rather than always
     * taking the largest, which is what it did before this existed.
     */
    mi_crawl_quality *qualities;
    size_t quality_count;
    /*
     * A poster for the result, when one was found.
     *
     * A file listing is a wall of names without one, and a person recognises a
     * video by its picture long before its title. Two sources, in order: the
     * media extractor reports a thumbnail for a page it resolved, and a link the
     * walk found may itself be an image — a gallery's own thumbnails are often
     * linked beside the full files.
     *
     * NULL when neither applies, which the console shows as a kind glyph.
     */
    char *thumbnail;
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
    /*
     * Pages that answered with a captcha challenge instead of content.
     *
     * Reported so the caller can say "the site put up a wall" rather than "no
     * file matched", which are very different answers to the same empty result.
     */
    int challenges_seen;
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
    /*
     * The most pages a walk may drive a browser for.
     *
     * A browser costs several seconds a page and there is no cheap way to know
     * whether one is needed, so the cost is bounded by counting rather than by
     * guessing. The default is small because the pages that need a browser are
     * the first few: a JavaScript site hides its listing behind its landing
     * page, not behind every page on the site.
     */
    int max_browser_pages;
    /*
     * Whether the walk may ask a media extractor about a page.
     *
     * On by default when the extractor is installed. A streaming page's media
     * URL is not among its links, so without this a crawl of one reports the
     * channel rather than the video — which is what it did before this existed.
     */
    int extract_enabled;
    /* The extractor binary; NULL uses "yt-dlp" from PATH. */
    const char *extractor_path;
    /*
     * The gallery extractor's binary; NULL uses "gallery-dl" from PATH.
     *
     * A second tool for a second question — yt-dlp resolves video, gallery-dl
     * lists the images on a page — so a walk that finds a gallery URL can ask
     * the right one.
     */
    const char *gallery_extractor_path;
    /*
     * Cookies for a site that refuses an anonymous request.
     *
     * Netscape format, the same input the download path takes. A crawl of a
     * site behind a sign-in needs them for the same reason a single download
     * does: the extractor cannot resolve a page it is turned away from. Carried
     * here so the walk and the download agree on the credential.
     *
     * NULL sends none. A credential, so it is never logged.
     */
    const char *cookies_text;
    /*
     * A proxy every request the walk makes goes through, e.g.
     * "http://host:port" or "socks5://host:port".
     *
     * A crawl makes many requests to one host, so a host that refuses this
     * address refuses the whole walk. The same egress reaches the page fetches,
     * the extractor and the browser fallback. NULL is direct.
     */
    const char *proxy;
    /*
     * A yt-dlp / curl_cffi impersonation target for the extractor step, e.g.
     * "chrome". The walk's own page fetches are libcurl and cannot impersonate;
     * this reaches the extractor, which can. NULL is off.
     */
    const char *impersonate;
    /* The User-Agent to send. NULL uses the built-in default. */
    const char *user_agent;
    /*
     * The browser pool to render with, or NULL to never render.
     *
     * Passed in rather than owned: the pool is Vyzor's single browser gate, and
     * a crawl must share it rather than start a second one.
     */
    struct mi_render_pool *render_pool;
    /*
     * The interactive pool, used for the fallback when it is available.
     *
     * A render only *looks* at a page: it loads it, waits, and dumps the DOM. A
     * session can be *driven* — wait for the network to settle, click what needs
     * clicking, and then read the links the browser actually has. For a listing
     * assembled by script, or one behind a "Load more" or a category tab, that
     * is the difference between finding the file and reporting nothing. Falls
     * back to the renderer when this is NULL.
     */
    struct mi_interactive_pool *interactive_pool;
    /*
     * CSS selectors to click before reading, in order, once the page has loaded.
     *
     * A page's listing is often behind one: a consent button, a tab, a "show
     * all". Clicking what matches is what makes those reachable. Each is tried
     * and any that matches has an effect; a selector that matches nothing costs
     * one round trip.
     */
    const char *const *click_selectors;
    size_t click_selector_count;
    /* Seconds to let a page settle after load before reading it. */
    double settle_seconds;
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
