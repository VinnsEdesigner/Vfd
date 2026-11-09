/* mi_crawl.c - walk a site and find files matching a name.
 *
 * The shape of the walk is a breadth-first frontier with a visited set, which
 * is the standard crawler and is chosen here for one practical reason: a
 * breadth-first walk reaches a site's listing pages in the fewest requests, so
 * `max_pages` buys the most coverage. A depth-first walk would spend its budget
 * going deep into the first branch it found.
 *
 * Every limit is enforced at the point where it costs something, rather than
 * checked afterwards: the page cap before a fetch, the time budget before each
 * fetch, and the hit cap as soon as a hit is recorded, so a walk that has what
 * the caller wanted stops rather than finishing the site.
 */
#define _GNU_SOURCE
#include "mi_crawl.h"

#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "mi_browse.h"
#include "mi_captcha.h"
#include "mi_frontier.h"
#include "mi_antibot.h"
#include "mi_fingerprint.h"
#include "mi_sitemap.h"
#include "mi_politeness.h"
#include "mi_download.h"
#include "mi_extract.h"
#include "mi_interactive.h"
#include "mi_json.h"
#include "mi_render.h"
#include "mi_util.h"

/* A page's HTML, capped. A listing page is small; anything past this is not a
 * listing and reading it all would only cost memory. */
#define CRAWL_MAX_BODY (2 * 1024 * 1024)
/* The most pages one walk may read, whatever the config says, so a mistake in a
 * caller cannot turn into an unbounded crawl. */
#define CRAWL_HARD_MAX_PAGES 5000
/* The most pages to fetch at once, when the caller did not say. Parallel fetches
 * are what make the budget buy coverage; the per-host politeness interval keeps
 * any one host's rate honest, so this spreads across hosts. */
#define CRAWL_DEFAULT_WORKERS 4
#define CRAWL_MAX_WORKERS 16
/* How many candidate hits one walk may probe to confirm they are media. */
#define CRAWL_DEFAULT_MAX_VERIFY 24

/*
 * The config defaults, named once.
 *
 * `mi_crawl_config_init` fills them, and `mi_crawl` falls back to the same
 * values when a caller zeroed a field after init. They used to be literals in
 * both places and drifted — the fallbacks still said 40 pages / 45 s after the
 * defaults moved to 200 / 90 s — so a caller who zeroed `max_pages` got the old
 * budget back. One name, used in both, cannot drift.
 */
#define CRAWL_DEFAULT_MAX_DEPTH 3
#define CRAWL_DEFAULT_MAX_PAGES 200
#define CRAWL_DEFAULT_MAX_HITS 50
#define CRAWL_DEFAULT_BUDGET_SECONDS 90.0
#define CRAWL_DEFAULT_MAX_BROWSER_PAGES 8
#define CRAWL_DEFAULT_DEDUP_DISTANCE 3
#define CRAWL_DEFAULT_SETTLE_SECONDS 2.0
#define CRAWL_DEFAULT_FRONTIER_LEASE 300

/*
 * Retry policy for a page fetch.
 *
 * A crawl reads hundreds of pages, so one flaky 5xx or a dropped connection
 * silently costs a page slot and its links. A handful of retries with
 * exponential backoff turns that into a page read. Bounded, because a hard 5xx
 * is sometimes the site's answer and hammering it is worse than skipping it.
 */
#define CRAWL_FETCH_RETRIES 2
#define CRAWL_FETCH_BACKOFF_SECONDS 0.5

/* How long to let a click's effect arrive before reading, and how long to let a
 * page settle after load. Short on purpose: a crawl drives many pages, and a
 * per-page cost of ten seconds makes the walk useless even when it is right. */
#define CLICK_SETTLE_SECONDS 0.6
#define READ_SETTLE_SECONDS 2.0

void mi_crawl_config_init(mi_crawl_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->max_depth = CRAWL_DEFAULT_MAX_DEPTH;
    cfg->max_pages = CRAWL_DEFAULT_MAX_PAGES;
    cfg->max_hits = CRAWL_DEFAULT_MAX_HITS;
    cfg->budget_seconds = CRAWL_DEFAULT_BUDGET_SECONDS;
    cfg->same_host_only = 1;
    cfg->use_browser_fallback = 1;
    cfg->max_browser_pages = CRAWL_DEFAULT_MAX_BROWSER_PAGES;
    cfg->workers = CRAWL_DEFAULT_WORKERS;
    cfg->verify_hits = 1;
    cfg->max_verify = CRAWL_DEFAULT_MAX_VERIFY;
    cfg->browser_scroll = 1;
    cfg->extract_enabled = 1;
    cfg->extractor_path = NULL;
    cfg->gallery_extractor_path = NULL;
    cfg->torrent_extractor_path = NULL;
    cfg->torrent_site = NULL;
    cfg->cookies_text = NULL;
    cfg->cookies_from_browser = NULL;
    cfg->proxy = NULL;
    cfg->impersonate = NULL;
    cfg->brain = NULL;
    cfg->politeness = NULL;
    cfg->obey_robots = 1;
    cfg->use_sitemap = 1;
    cfg->dedup = 1;
    cfg->dedup_distance = CRAWL_DEFAULT_DEDUP_DISTANCE;
    cfg->user_agent = NULL;
    cfg->frontier_db_url = NULL;
    cfg->frontier_token = NULL;
    cfg->frontier_run_id = NULL;
    cfg->frontier_db_path = NULL;
    cfg->frontier_lease_seconds = CRAWL_DEFAULT_FRONTIER_LEASE;
    cfg->render_pool = NULL;
    cfg->interactive_pool = NULL;
    cfg->click_selectors = NULL;
    cfg->click_selector_count = 0;
    cfg->settle_seconds = CRAWL_DEFAULT_SETTLE_SECONDS;
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Sleep for a fractional number of seconds, without a signal-time surprise. */
static void sleep_seconds(double s) {
    if (s <= 0) return;
    struct timespec ts;
    ts.tv_sec = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    if (ts.tv_nsec > 999999999L) ts.tv_nsec = 999999999L;
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------ */
/* The dedup set: fingerprints of pages already read                   */
/* ------------------------------------------------------------------ */

/*
 * A small list of the fingerprints seen, with a linear scan. A crawl reads at
 * most a few hundred pages, so a scan over 64-bit values is nothing next to the
 * network, and it avoids a hash table for a bounded set.
 */
typedef struct {
    mi_simhash *items;
    size_t count, cap;
} dedup_set;

static int dedup_seen(const dedup_set *d, mi_simhash fp, int max_distance) {
    for (size_t i = 0; i < d->count; i++) {
        if (mi_simhash_similar(d->items[i], fp, max_distance)) return 1;
    }
    return 0;
}

static void dedup_add(dedup_set *d, mi_simhash fp) {
    if (!fp) return;
    if (d->count == d->cap) {
        size_t want = d->cap ? d->cap * 2 : 64;
        mi_simhash *g = mi_realloc(d->items, want * sizeof *g);
        if (!g) return;
        d->items = g;
        d->cap = want;
    }
    d->items[d->count++] = fp;
}

/* ------------------------------------------------------------------ */
/* The frontier and the visited set                                    */
/* ------------------------------------------------------------------ */

/*
 * The queue and the seen-set are `mi_frontier` (see mi_frontier.h): an in-process
 * queue, or one shared through Turso so several workers walk one site once. The
 * walk does not care which it was given — it pushes, takes, and marks seen.
 *
 * These wrappers keep the call sites readable. `strset_has` is expressed through
 * `mark_seen`: "already seen" is exactly "not newly added", which is the atomic
 * operation the shared backend provides.
 */
typedef mi_frontier frontier;

/* Take the next URL. Returns 1 and sets the out-params, 0 when empty. The URL is
 * owned by the caller. */
static int frontier_take(frontier *f, char **url, int *depth) {
    return mi_frontier_take(f, url, depth);
}

static int strset_has(frontier *f, const char *url) {
    return !mi_frontier_mark_seen(f, url);
}
static int strset_add(frontier *f, const char *url) {
    return mi_frontier_mark_seen(f, url);
}

/* ------------------------------------------------------------------ */
/* Matching                                                            */
/* ------------------------------------------------------------------ */

/*
 * Fold a string for comparison: lower-case, and drop the separators people vary.
 *
 * "STM32 Datasheet", "stm32-datasheet" and "stm32_datasheet" are the same name
 * as far as a person searching for one is concerned, and a filename on a site
 * may be written any of those ways. Folding both sides means the caller does not
 * have to guess which separator the site used.
 */
static char *fold(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *out = mi_malloc(n + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '-' || c == '_' || c == '.' || c == '/' || c == '+') continue;
        out[j++] = (char)tolower(c);
    }
    out[j] = '\0';
    return out;
}

/*
 * Bounded Levenshtein distance between two short strings.
 *
 * Returns the number of edits, or `max + 1` when they are further apart than
 * `max` (which lets the caller stop early). Used only on folded names, which are
 * short, so the quadratic cost is nothing.
 */
static int edit_distance(const char *a, const char *b, int max) {
    size_t la = strlen(a), lb = strlen(b);
    if (la == 0) return (int)lb > max ? max + 1 : (int)lb;
    if (lb == 0) return (int)la > max ? max + 1 : (int)la;
    if ((int)(la > lb ? la - lb : lb - la) > max) return max + 1;
    /* Two rolling rows of the DP table. */
    int *prev = mi_malloc((lb + 1) * sizeof *prev);
    int *cur = mi_malloc((lb + 1) * sizeof *cur);
    if (!prev || !cur) { free(prev); free(cur); return max + 1; }
    for (size_t j = 0; j <= lb; j++) prev[j] = (int)j;
    for (size_t i = 1; i <= la; i++) {
        cur[0] = (int)i;
        int row_min = cur[0];
        for (size_t j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int del = prev[j] + 1;
            int ins = cur[j - 1] + 1;
            int sub = prev[j - 1] + cost;
            int v = del < ins ? del : ins;
            if (sub < v) v = sub;
            cur[j] = v;
            if (v < row_min) row_min = v;
        }
        if (row_min > max) { free(prev); free(cur); return max + 1; }
        int *tmp = prev; prev = cur; cur = tmp;
    }
    int d = prev[lb];
    free(prev);
    free(cur);
    return d > max ? max + 1 : d;
}

/*
 * Whether `needle` appears in `hay` within `max_dist` edits — the fuzzy form of
 * `strstr`, so a name misspelled by a character or two still matches. Only a
 * window the length of the needle (plus or minus the budget) is compared at each
 * position, so an unrelated long name cannot match by accident.
 */
static int fuzzy_contains(const char *hay, const char *needle, int max_dist) {
    size_t hn = strlen(hay), nn = strlen(needle);
    if (nn == 0) return 0;
    /* The whole-string case first: the name is the term, misspelled. */
    if (edit_distance(hay, needle, max_dist) <= max_dist) return 1;
    if (hn < nn) return 0;
    for (size_t i = 0; i + nn <= hn + (size_t)max_dist; i++) {
        size_t take = nn + (size_t)max_dist;
        if (i + take > hn) take = hn - i;
        char *win = mi_strndup(hay + i, take);
        if (!win) return 0;
        int d = edit_distance(win, needle, max_dist);
        free(win);
        if (d <= max_dist) return 1;
    }
    return 0;
}

/* How many edits a term of this length may be misspelled by. Short terms get no
 * slack, because one edit in a three-letter term matches almost anything. */
static int fuzzy_budget(size_t term_len) {
    if (term_len < 5) return 0;
    if (term_len < 10) return 1;
    return 2;
}

int mi_crawl_score(const char *filename, const char *title, const char *term) {
    if (!term || !*term) return 50; /* no term: everything is equally interesting */

    char *t = fold(term);
    if (!t) return 0;
    char *f = fold(filename);
    char *ti = fold(title);

    int score = 0;
    /*
     * An exact match on the folded name is the strongest signal: the file is
     * called what was asked for. A substring match is next, and a match only in
     * the link text is weaker, because the text can describe the file loosely.
     */
    if (f && !strcmp(f, t)) {
        score = 100;
    } else if (f && strstr(f, t)) {
        score = 80;
    } else if (ti && !strcmp(ti, t)) {
        score = 70;
    } else if (ti && strstr(ti, t)) {
        score = 55;
    } else {
        /*
         * Fall back to the term's words, *weighted by their length*.
         *
         * Every word appearing somewhere is the same 45 as before, but a partial
         * match now reports how much of the term was covered rather than a flat
         * 20: "stm32 datasheet" matching only "datasheet" is a weaker answer than
         * matching only "stm32", and a caller ranking results should be able to
         * tell. The long words are what discriminate, so their weight is their
         * length.
         */
        char *copy = mi_strdup(term);
        if (copy) {
            int words = 0, found = 0, weight = 0, matched_weight = 0;
            char *save = NULL;
            for (char *w = strtok_r(copy, " -_./+", &save); w; w = strtok_r(NULL, " -_./+", &save)) {
                words++;
                char *wf = fold(w);
                if (wf) {
                    int wlen = (int)strlen(wf);
                    if (wlen == 0) wlen = 1;
                    weight += wlen;
                    if ((f && strstr(f, wf)) || (ti && strstr(ti, wf))) {
                        found++;
                        matched_weight += wlen;
                    }
                    free(wf);
                }
            }
            free(copy);
            if (words > 0 && found == words) {
                score = 45;
            } else if (found > 0 && weight > 0) {
                score = 10 + (35 * matched_weight) / weight;
            }
        }
    }

    /*
     * Fuzzy matching, for a name misspelled by a character or two.
     *
     * "STM32 Datasheet" folds and matches cleanly, but a site's file called
     * "stm32-datashet.pdf" would score 0 on a plain substring test and be lost.
     * The slack scales with the term's length so a short term cannot match by
     * accident, and it only runs when the exact tests found nothing — so it can
     * add a hit but never displace a real one.
     */
    if (score == 0 && t[0]) {
        int budget = fuzzy_budget(strlen(t));
        if (budget > 0) {
            if ((f && fuzzy_contains(f, t, budget)) ||
                (ti && fuzzy_contains(ti, t, budget))) {
                score = 30;
            } else {
                /* Per-word, so one misspelled word among correct ones still
                 * carries the match. */
                char *copy = mi_strdup(term);
                if (copy) {
                    int words = 0, fuzzed = 0;
                    char *save = NULL;
                    for (char *w = strtok_r(copy, " -_./+", &save); w;
                         w = strtok_r(NULL, " -_./+", &save)) {
                        char *wf = fold(w);
                        if (!wf) continue;
                        words++;
                        int b = fuzzy_budget(strlen(wf));
                        if (b > 0 && ((f && fuzzy_contains(f, wf, b)) ||
                                      (ti && fuzzy_contains(ti, wf, b)))) {
                            fuzzed++;
                        }
                        free(wf);
                    }
                    free(copy);
                    if (words > 0 && fuzzed == words) score = 25;
                    else if (fuzzed > 0) score = 12;
                }
            }
        }
    }

    free(t);
    free(f);
    free(ti);
    return score;
}

/* ------------------------------------------------------------------ */
/* Results                                                             */
/* ------------------------------------------------------------------ */

/*
 * The URL that serves the *bytes*, when the page linked a viewer instead.
 *
 * A host that shows a file inside its own page commonly links the viewer and
 * leaves the direct URL for the viewer to request. GitHub is the case that
 * matters: `/blob/master/Makefile` is an HTML page about the file, and
 * `/raw/master/Makefile` is the file — the same path with `blob` swapped for
 * `raw`. Downloading the viewer gets 748 KB of HTML where the file is 82 KB.
 *
 * Returns a new string when a direct form is known, or NULL to keep the URL as
 * it is. Only a rewrite of a *known* host shape is done; guessing at an
 * arbitrary site would produce URLs that do not exist.
 */
static char *direct_url_for(const char *url) {
    static const struct {
        const char *viewer; /* the marker in the viewer path */
        const char *direct; /* what it becomes */
    } REWRITES[] = {
        {"/blob/", "/raw/"},
        {"/src/", "/raw/"},       /* Bitbucket's viewer */
        {"/-/blob/", "/-/raw/"},  /* GitLab's */
    };
    for (size_t i = 0; i < sizeof REWRITES / sizeof REWRITES[0]; i++) {
        const char *p = strstr(url, REWRITES[i].viewer);
        if (!p) continue;
        size_t head = (size_t)(p - url);
        return mi_asprintf("%.*s%s%s", (int)head, url, REWRITES[i].direct,
                           p + strlen(REWRITES[i].viewer));
    }
    return NULL;
}

static mi_crawl_hit *add_hit(mi_crawl_result *res, const mi_browse_item *item,
                             int score, const char *found_on, int via_browser) {
    mi_crawl_hit *grown = mi_realloc(res->hits, (res->hit_count + 1) * sizeof *grown);
    if (!grown) return NULL;
    res->hits = grown;
    mi_crawl_hit *h = &res->hits[res->hit_count++];
    memset(h, 0, sizeof *h);
    /*
     * Record the URL that serves the bytes, not the viewer.
     *
     * A host that shows a file in its own page links the viewer, and the direct
     * URL is derived rather than linked — so a hit recorded as-is would download
     * an HTML page. The filename is left alone: it is the same file either way,
     * and it is what the match was scored on.
     */
    char *direct = direct_url_for(item->url);
    h->url = direct ? direct : mi_strdup(item->url);
    h->filename = mi_strdup(item->filename);
    h->title = item->title ? mi_strdup(item->title) : NULL;
    h->mime = mi_strdup(item->mime ? item->mime : "");
    h->score = score;
    h->found_on = mi_strdup(found_on);
    h->via_browser = via_browser;
    h->resolved = 0;
    return h;
}

/*
 * Add one entry of an extractor list as a hit.
 *
 * An *entry* is a list item rather than a stream: yt-dlp returns entries for a
 * search or a channel (`--flat-playlist`) and py1337x returns them for a torrent
 * search. Each carries a title and a page URL but no formats, so the hit's URL is
 * the item's *page* — which is what a later resolve or download acts on.
 *
 * Returns 1 when a hit was added.
 */
static int add_entry_hit(mi_crawl_result *res, const mi_extract_entry *e,
                         const char *term, const char *found_on) {
    const char *page = (e->url && *e->url) ? e->url : NULL;
    if (!page) return 0; /* an entry with nowhere to go is not a result */
    const char *label = (e->title && *e->title) ? e->title : page;
    int score = mi_crawl_score(label, label, term);
    if (score <= 0) return 0;
    mi_browse_item item;
    memset(&item, 0, sizeof item);
    item.url = (char *)page;
    item.filename = (char *)label;
    item.title = (char *)label;
    item.mime = (char *)"";
    item.bytes = -1;
    if (!add_hit(res, &item, score, found_on, 0)) return 0;
    mi_crawl_hit *h = &res->hits[res->hit_count - 1];
    /* The extractor's answer is the authority for this page, so it ranks above a
     * link the walk happened to find at an equal score. */
    h->resolved = 1;
    if (e->thumbnail && *e->thumbnail) h->thumbnail = mi_strdup(e->thumbnail);
    return 1;
}

static void add_page(mi_crawl_result *res, const char *url, int links, int hits,
                     int rendered, int status) {
    mi_crawl_page *grown = mi_realloc(res->pages, (res->page_count + 1) * sizeof *grown);
    if (!grown) return;
    res->pages = grown;
    mi_crawl_page *p = &res->pages[res->page_count++];
    p->url = mi_strdup(url);
    p->links_seen = links;
    p->hits = hits;
    p->rendered = rendered;
    p->status = status;
}

void mi_crawl_result_free(mi_crawl_result *res) {
    if (!res) return;
    for (size_t i = 0; i < res->hit_count; i++) {
        free(res->hits[i].url);
        free(res->hits[i].filename);
        free(res->hits[i].title);
        free(res->hits[i].mime);
        free(res->hits[i].found_on);
        free(res->hits[i].thumbnail);
        for (size_t q = 0; q < res->hits[i].quality_count; q++) {
            free(res->hits[i].qualities[q].label);
        }
        free(res->hits[i].qualities);
    }
    free(res->hits);
    for (size_t i = 0; i < res->page_count; i++) free(res->pages[i].url);
    free(res->pages);
    memset(res, 0, sizeof *res);
}

/* ------------------------------------------------------------------ */
/* The walk                                                            */
/* ------------------------------------------------------------------ */

/** The host of a URL, for the same-host test. NULL when it cannot be read. */
static char *host_of(const char *url) {
    const char *p = strstr(url, "://");
    if (!p) return NULL;
    p += 3;
    const char *end = p;
    while (*end && *end != '/' && *end != '?' && *end != '#') end++;
    size_t n = (size_t)(end - p);
    if (n == 0) return NULL;
    char *out = mi_malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, p, n);
    out[n] = '\0';
    return out;
}

/*
 * Is this URL a page to follow, rather than a file to report?
 *
 * "No extension means a page" is the obvious rule and it is wrong: Makefile,
 * Dockerfile, LICENSE and README have no extension, and they are exactly the
 * files someone searches a repository for. That rule made a crawl of GitHub
 * report zero hits for "Makefile" while the link sat in the HTML.
 *
 * So the two questions are kept separate:
 *   - `is_page`  — could this be HTML to walk into? Extension-less counts,
 *                  because /repo/blob/master/src is a page and costs only one
 *                  fetch to find out.
 *   - `is_asset` — is this certainly not content? Scripts, styles, fonts and
 *                  icons are dropped from the results, which is what stops a
 *                  page's own plumbing filling the list.
 * A URL is a candidate hit unless it is an asset; it is followed if it is a page.
 */
static int is_page(const char *url) {
    const char *q = strchr(url, '?');
    size_t n = q ? (size_t)(q - url) : strlen(url);
    const char *slash = NULL;
    for (size_t i = 0; i < n; i++) {
        if (url[i] == '/') slash = url + i;
    }
    const char *name = slash ? slash + 1 : url;
    const char *dot = NULL;
    for (const char *p = name; p < url + n; p++) {
        if (*p == '.') dot = p;
    }
    if (!dot || dot >= url + n) return 1; /* no extension: could be either */
    static const char *PAGE_EXT[] = {".html", ".htm", ".php", ".asp", ".aspx", ".jsp",
                                     ".shtml", ".cgi", ".pl",
                                     /* A feed is a page to walk: it names the
                                      * site's newest items, which the walk then
                                      * follows and scores. `.xml` is included
                                      * because `/feed.xml` is the commonest
                                      * feed URL of all; a non-feed XML yields
                                      * nothing and costs one page. */
                                     ".rss", ".atom", ".rdf", ".xml", NULL};
    for (size_t i = 0; PAGE_EXT[i]; i++) {
        if (!strncasecmp(dot, PAGE_EXT[i], strlen(PAGE_EXT[i]))) return 1;
    }
    return 0;
}

/** Certainly not content: a page's own scripts, styles, fonts and icons.
 *
 * Images are deliberately *not* here. A raster image is media a media crawler
 * was asked to find — an `og:image`, a gallery's `srcset`, a thumbnail — and
 * treating `.png` as an asset made the extraction work unreachable at the hit
 * filter. `.svg` stays: it is almost always a logo or an icon sprite, not a
 * file anyone asked for by name. */
static int is_asset(const char *url) {
    const char *q = strchr(url, '?');
    size_t n = q ? (size_t)(q - url) : strlen(url);
    static const char *ASSET_EXT[] = {".css",  ".js",   ".mjs",  ".map",  ".woff",
                                      ".woff2", ".ttf", ".eot",  ".ico",  ".svg",
                                      NULL};
    for (size_t i = 0; ASSET_EXT[i]; i++) {
        size_t elen = strlen(ASSET_EXT[i]);
        if (n >= elen && !strncasecmp(url + n - elen, ASSET_EXT[i], elen)) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Shared state for the parallel walk                                  */
/* ------------------------------------------------------------------ */

/*
 * Everything the workers share.
 *
 * The walk fetches several pages at once, so the things that used to be local
 * variables — the result being built, the page counter, the time the walk
 * started — are here, guarded by `lock`. The frontier is already thread-safe
 * (see mi_frontier), and so are the politeness state, the anti-bot brain and the
 * browser pools, so those are used directly.
 */
typedef struct {
    const mi_crawl_config *cfg;
    frontier *f;
    const char *term;
    const char *start_url;
    mi_crawl_result *res;
    double started;
    double deadline;
    int max_pages;
    int max_hits;
    int pages_read;
    int verifies;
    /* The fingerprints of pages already read. Guarded by `lock`, because two
     * workers may fingerprint a page at once. */
    dedup_set *dedup;
    pthread_mutex_t lock;
} crawl_shared;

/* Whether the crawl may keep going: pages left, hits left, time left. */
static int crawl_should_continue(crawl_shared *s) {
    pthread_mutex_lock(&s->lock);
    int go = s->pages_read < s->max_pages &&
             (int)s->res->hit_count < s->max_hits &&
             now_seconds() < s->deadline;
    pthread_mutex_unlock(&s->lock);
    return go;
}

/* Reserve one page slot, so the cap holds across workers. Returns 0 when the
 * cap is already reached. */
static int crawl_take_page_slot(crawl_shared *s) {
    pthread_mutex_lock(&s->lock);
    int ok = s->pages_read < s->max_pages;
    if (ok) s->pages_read++;
    pthread_mutex_unlock(&s->lock);
    return ok;
}

/* Whether a candidate hit may still be probed. */
static int crawl_take_verify_slot(crawl_shared *s) {
    pthread_mutex_lock(&s->lock);
    int cap = s->cfg->max_verify > 0 ? s->cfg->max_verify : CRAWL_DEFAULT_MAX_VERIFY;
    int ok = s->verifies < cap;
    if (ok) s->verifies++;
    pthread_mutex_unlock(&s->lock);
    return ok;
}

/* ------------------------------------------------------------------ */
/* URL canonicalisation and term-aware priority                        */
/* ------------------------------------------------------------------ */

/* Whether a URL's path or query carries the term, folded — the strongest signal
 * that the page it names is about what the caller asked for. */
static int url_mentions_term(const char *url, const char *folded_term) {
    if (!folded_term || !*folded_term) return 0;
    char *fu = fold(url);
    if (!fu) return 0;
    int hit = strstr(fu, folded_term) != NULL;
    free(fu);
    return hit;
}

/*
 * A priority for a frontier entry: higher is fetched sooner.
 *
 * With a small budget on a large site, *which* pages the walk reads decides
 * whether it finds anything. A URL whose own path carries the term is the best
 * next fetch, and a shallower page is next (it is closer to the site's index, so
 * it is more likely to link the rest). A sitemap entry adds the site's own
 * priority on top of this, in `seed_from_sitemaps`. Everything else keeps
 * insertion order, so an unprioritised walk is unchanged.
 */
static int url_priority(const char *url, int depth, const char *folded_term) {
    int p = 0;
    if (url_mentions_term(url, folded_term)) p += 100;
    /* Shallow pages first: the site's own index pages reach the most. */
    p += 10 - depth;
    return p;
}

/*
 * Add a URL to the frontier, canonicalised and seen-checked.
 *
 * Canonicalising before the seen-set is what stops the same page under
 * `?utm_source=` and without it from being two entries and two fetches out of a
 * small budget. The priority is computed here so every enqueue point agrees.
 */
static void frontier_add(crawl_shared *s, const char *url, int depth, const char *folded_term) {
    if (!url || !*url) return;
    char *norm = mi_url_normalize(url);
    const char *u = norm ? norm : url;
    if (!strset_has(s->f, u)) {
        mi_frontier_push_prio(s->f, u, depth, url_priority(u, depth, folded_term));
    }
    free(norm);
}

/*
 * Add one candidate hit, with the browser flag.
 *
 * The extractor's answer, the anchor pass and the rendered pass all reach here,
 * so this is where a hit is appended under the lock. Returns the index of the
 * new hit, or -1.
 */
static int res_add_hit(crawl_shared *s, const mi_browse_item *item, int score,
                       const char *found_on, int via_browser) {
    pthread_mutex_lock(&s->lock);
    mi_crawl_hit *h = NULL;
    if ((int)s->res->hit_count < s->max_hits) {
        h = add_hit(s->res, item, score, found_on, via_browser);
    }
    int idx = h ? (int)s->res->hit_count - 1 : -1;
    pthread_mutex_unlock(&s->lock);
    return idx;
}

static int res_add_page(crawl_shared *s, const char *url, int links, int hits,
                        int rendered, int status) {
    pthread_mutex_lock(&s->lock);
    add_page(s->res, url, links, hits, rendered, status);
    pthread_mutex_unlock(&s->lock);
    return 0;
}

/* Add one extractor *entry* as a hit, under the lock. */
static int res_add_entry_hit(crawl_shared *s, const mi_extract_entry *e,
                             const char *found_on) {
    pthread_mutex_lock(&s->lock);
    int added = 0;
    if ((int)s->res->hit_count < s->max_hits) {
        added = add_entry_hit(s->res, e, s->term, found_on);
    }
    pthread_mutex_unlock(&s->lock);
    return added;
}

/* Whether the dedup set has seen this fingerprint, and record it. Under the
 * lock, so two workers fingerprinting at once cannot both add it. */
static int res_dedup_check(crawl_shared *s, mi_simhash fp) {
    if (!fp) return 0;
    pthread_mutex_lock(&s->lock);
    int seen = dedup_seen(s->dedup, fp, s->cfg->dedup_distance);
    dedup_add(s->dedup, fp);
    pthread_mutex_unlock(&s->lock);
    return seen;
}

/*
 * The hit cap and the page cap, under the lock.
 *
 * `room` is how many more hits may be added; a worker uses it to stop mid-page
 * rather than build a list longer than the caller asked for.
 */
static int res_hits_left(crawl_shared *s) {
    pthread_mutex_lock(&s->lock);
    int room = s->max_hits - (int)s->res->hit_count;
    pthread_mutex_unlock(&s->lock);
    return room > 0 ? room : 0;
}

/*
 * Confirm a candidate hit from the response rather than the URL string.
 *
 * A filename is guessed from the URL, so a signed or extension-less address is
 * treated as a page and an HTML viewer named like a file is recorded as a hit
 * with nothing behind it. A ranged probe answers both questions at once: the
 * Content-Type and the magic bytes say whether this is real media, and the
 * filename is rewritten when the URL's own name is not what the bytes are.
 *
 * Returns 1 when the candidate is confirmed media (and fills `mime`), 0 when it
 * is a page rather than a file, and -1 when the probe could not tell — in which
 * case the caller keeps the candidate, because a host that refuses a probe must
 * not lose a real hit.
 */
static int verify_candidate(const mi_crawl_config *cfg, const char *url,
                            char **mime_out) {
    mi_download_config dcfg;
    mi_download_config_init(&dcfg);
    dcfg.require_media = 0;
    dcfg.user_agent = cfg->user_agent;
    dcfg.proxy = cfg->proxy;
    dcfg.impersonate = cfg->impersonate;
    dcfg.cookies_text = cfg->cookies_text;
    dcfg.cookies_from_browser = cfg->cookies_from_browser;
    dcfg.timeout_seconds = 10;

    mi_download_plan plan;
    if (mi_probe(&dcfg, url, &plan) != 0) return -1;

    int verdict = -1;
    if (plan.have_media && plan.media.kind != MI_KIND_UNKNOWN) {
        verdict = 1;
        if (mime_out) {
            const char *name = mi_kind_name(plan.media.kind);
            if (name && *name) *mime_out = mi_strdup(name);
        }
    } else if (plan.content_type && *plan.content_type) {
        /* No media magic. A response that says it is a document is a page, not a
         * file, and the candidate was a URL-shape guess. Any textual type
         * counts: a viewer page can serve `text/xml`, `text/plain` or
         * `text/vtt` as easily as `text/html`, and none of those is a file the
         * walk set out to find. */
        const char *ct = plan.content_type;
        if (!strncasecmp(ct, "text/", 5) ||
            !strncasecmp(ct, "application/xhtml", 17) ||
            !strncasecmp(ct, "application/xml", 15) ||
            !strncasecmp(ct, "application/json", 16)) {
            verdict = 0;
        } else if (!strncasecmp(ct, "video/", 6) || !strncasecmp(ct, "audio/", 6) ||
                   !strncasecmp(ct, "image/", 6) ||
                   !strncasecmp(ct, "application/vnd.apple.mpegurl", 29) ||
                   !strncasecmp(ct, "application/dash", 16) ||
                   !strncasecmp(ct, "application/octet-stream", 24) ||
                   !strncasecmp(ct, "application/pdf", 15)) {
            verdict = 1;
        }
    }
    mi_download_plan_free(&plan);
    return verdict;
}

/*
 * Verify the candidate hits a page produced, in place.
 *
 * Called once per page, under no lock except the counters', so a probe does not
 * stall the other workers. A candidate the probe shows is a page is removed from
 * the result; a candidate it confirms gets the real MIME and a score bump so
 * verified media outranks an unverified link.
 */
static void verify_page_hits(crawl_shared *s, size_t first, size_t count) {
    if (!s->cfg->verify_hits) return;
    for (size_t i = 0; i < count; i++) {
        if (now_seconds() >= s->deadline) return;
        pthread_mutex_lock(&s->lock);
        size_t idx = first + i;
        if (idx >= s->res->hit_count) { pthread_mutex_unlock(&s->lock); return; }
        char *url = mi_strdup(s->res->hits[idx].url);
        int resolved = s->res->hits[idx].resolved;
        pthread_mutex_unlock(&s->lock);
        if (!url) return;
        /* The extractor's answer is already authoritative, and a magnet is not
         * fetchable by a probe. */
        if (resolved || !strncasecmp(url, "magnet:", 7)) { free(url); continue; }
        if (!crawl_take_verify_slot(s)) { free(url); continue; }

        char *mime = NULL;
        int v = verify_candidate(s->cfg, url, &mime);
        pthread_mutex_lock(&s->lock);
        if (idx < s->res->hit_count && s->res->hits[idx].url &&
            !strcmp(s->res->hits[idx].url, url)) {
            if (v == 1) {
                s->res->verified++;
                if (mime) {
                    free(s->res->hits[idx].mime);
                    s->res->hits[idx].mime = mime;
                    mime = NULL;
                }
                /* Verified media outranks an unverified link at an equal name
                 * score, which is what puts a real file above a viewer. */
                if (s->res->hits[idx].score < 95) s->res->hits[idx].score += 5;
            } else if (v == 0) {
                /* A page, not a file: drop it. The candidate was a URL-shape
                 * guess and the response disproved it. */
                s->res->dropped_non_media++;
                free(s->res->hits[idx].url);
                free(s->res->hits[idx].filename);
                free(s->res->hits[idx].title);
                free(s->res->hits[idx].mime);
                free(s->res->hits[idx].found_on);
                free(s->res->hits[idx].thumbnail);
                for (size_t q = 0; q < s->res->hits[idx].quality_count; q++)
                    free(s->res->hits[idx].qualities[q].label);
                free(s->res->hits[idx].qualities);
                memmove(&s->res->hits[idx], &s->res->hits[idx + 1],
                        (s->res->hit_count - idx - 1) * sizeof *s->res->hits);
                s->res->hit_count--;
                /* The array shifted, so the caller's indices move too; the
                 * simplest correct thing is to stop verifying this page's
                 * remaining candidates and let the next page re-index. */
                free(mime);
                free(url);
                pthread_mutex_unlock(&s->lock);
                return;
            }
        }
        pthread_mutex_unlock(&s->lock);
        free(mime);
        free(url);
    }
}

/**
 * Read a page by *driving* a browser, rather than only looking at it.
 *
 * A render loads a page, waits, and dumps the DOM. That is enough for a page
 * that builds its list on load, and useless for one that fetches the list by XHR
 * after settling, or hides it behind a tab, a consent button or a "show all".
 *
 * A session can do both: wait for the network to go quiet, click whatever the
 * caller says needs clicking, scroll so a lazy-loaded listing exists at all, and
 * then read the links the browser actually has — including ones the DOM dump
 * misses because they were attached after it was taken.
 *
 * Returns the number of hits added. `*rendered` is set when the browser was
 * actually used, so the caller can report that the page needed it.
 */
static int read_page_driven(crawl_shared *s, const char *url, int depth, int *rendered) {
    const mi_crawl_config *cfg = s->cfg;
    const char *term = s->term;
    double deadline = s->deadline;
    *rendered = 0;
    if (!cfg->interactive_pool || !mi_interactive_pool_available(cfg->interactive_pool)) {
        return 0;
    }

    mi_interactive *it = NULL;
    char err[256];
    err[0] = '\0';
    /* A viewport wide enough that a desktop listing is not collapsed into a
     * mobile layout, which often hides columns of a file table. */
    mi_it_status st = mi_interactive_open(cfg->interactive_pool, url, 1280, 900, 0, &it,
                                          err, sizeof err);
    if (st != MI_IT_OK || !it) return 0;
    *rendered = 1;
    /* Counted on the result as well as reported to the caller: the summary the
     * API returns is built from the result, so a browser that was used but not
     * counted would make the crawl look like it never needed one. */
    pthread_mutex_lock(&s->lock);
    s->res->rendered++;
    pthread_mutex_unlock(&s->lock);

    int hits = 0;
    int links = 0;

    /*
     * Settle, then click, then scroll, then read.
     *
     * The order matters: clicking before the page has finished loading misses
     * elements that do not exist yet, and reading before clicking misses the
     * listing the click produces. Every wait is short, because a crawl pays it
     * per page and a browser already costs seconds to start.
     */
    double settle_until = now_seconds() + (cfg->settle_seconds > 0 ? cfg->settle_seconds
                                                                   : READ_SETTLE_SECONDS);
    if (settle_until > deadline) settle_until = deadline;
    int closed = 0;
    while (now_seconds() < settle_until && !closed) {
        if (mi_interactive_pump(it, 200) == MI_IT_EV_CLOSED) closed = 1;
    }

    /*
     * The click pass.
     *
     * The selectors are *alternatives* — a consent button, a tab, a "show all" —
     * not a sequence, and there is no cheap way to know which one applies. So
     * each is tried with a short wait: a selector that matches nothing costs
     * 0.6s, and one that matches usually produces its effect within that. The
     * whole pass is bounded, so a page with no such button pays a few seconds
     * rather than ten.
     */
    for (size_t i = 0; i < cfg->click_selector_count && !closed; i++) {
        if (now_seconds() >= deadline) break;
        mi_interactive_click_selector(it, cfg->click_selectors[i]);
        double until = now_seconds() + CLICK_SETTLE_SECONDS;
        while (now_seconds() < until && !closed) {
            if (mi_interactive_pump(it, 150) == MI_IT_EV_CLOSED) closed = 1;
        }
    }

    /*
     * The scroll pass.
     *
     * A modern listing lazy-loads: only the first viewport exists in the DOM
     * until the page is scrolled, so a listing read without scrolling is a
     * fraction of itself and everything below the fold is invisible. Scrolling
     * to the bottom repeatedly, until the link count stops growing, is what
     * reaches the rest — bounded by a few rounds and the deadline so an infinite
     * scroller cannot run away.
     */
    if (cfg->browser_scroll && !closed) {
        for (int round = 0; round < 8 && !closed; round++) {
            if (now_seconds() >= deadline) break;
            mi_interactive_eval(it, "window.scrollTo(0, document.body.scrollHeight);"
                                    "document.body.scrollHeight");
            double until = now_seconds() + 0.5;
            while (now_seconds() < until && !closed) {
                if (mi_interactive_pump(it, 150) == MI_IT_EV_CLOSED) closed = 1;
            }
            /* Ask for the links and compare the count; if it has stopped
             * growing there is nothing below. */
            mi_interactive_read_dom(it);
            const char *probe = NULL;
            double read_until = now_seconds() + 1.5;
            while (now_seconds() < read_until && !closed) {
                if (mi_interactive_pump(it, 100) == MI_IT_EV_CLOSED) { closed = 1; break; }
                probe = mi_interactive_dom_links(it);
                if (probe) break;
            }
            if (!probe) break;
            int now_links = 0;
            for (const char *q = probe; *q; q++) if (*q == '\n') now_links++;
            if (now_links <= links) { links = now_links; break; }
            links = now_links;
        }
    }

    /* Ask for the links, then pump until the answer arrives. */
    mi_interactive_read_dom(it);
    const char *links_text = NULL;
    double read_until = now_seconds() + 3.0;
    while (now_seconds() < read_until && !closed) {
        if (mi_interactive_pump(it, 100) == MI_IT_EV_CLOSED) { closed = 1; break; }
        links_text = mi_interactive_dom_links(it);
        if (links_text) break;
    }

    if (links_text && *links_text) {
        /* One URL per line, as the reader returns them. */
        const char *p = links_text;
        int room = res_hits_left(s);
        size_t first = s->res->hit_count;
        while (*p && hits < room) {
            const char *nl = strchr(p, '\n');
            size_t n = nl ? (size_t)(nl - p) : strlen(p);
            if (n > 0 && n < 2048) {
                char *u = mi_strndup(p, n);
                if (u) {
                    links++;
                    if (!is_asset(u)) {
                        const char *slash = strrchr(u, '/');
                        const char *q = strchr(u, '?');
                        size_t flen = 0;
                        if (slash) {
                            const char *end = q && q > slash ? q : u + strlen(u);
                            flen = (size_t)(end - (slash + 1));
                        }
                        char *fname = flen ? mi_strndup(slash + 1, flen) : mi_strdup(u);
                        if (fname) {
                            int score = mi_crawl_score(fname, fname, term);
                            if (score > 0) {
                                mi_browse_item item;
                                memset(&item, 0, sizeof item);
                                item.url = u;
                                item.filename = fname;
                                item.title = NULL;
                                item.mime = (char *)"";
                                item.bytes = -1;
                                if (res_add_hit(s, &item, score, url, 1) >= 0) hits++;
                            }
                            free(fname);
                        }
                        /* Follow it if it looks like a page to walk into. */
                        if (is_page(u) && depth < cfg->max_depth) {
                            frontier_add(s, u, depth + 1, s->term);
                        }
                    }
                    free(u);
                }
            }
            if (!nl) break;
            p = nl + 1;
        }
        verify_page_hits(s, first, s->res->hit_count - first);
    }

    mi_interactive_close(it);
    res_add_page(s, url, links, hits, 1, 200);
    return hits;
}

/*
 * Fetch one page, retrying a transient failure.
 *
 * A 5xx or a dropped connection is usually transient, so one bad response
 * should not silently cost a page slot and the links it would have reached. A
 * 4xx or 3xx is the server's answer, so retrying it only wastes budget. The
 * wait doubles each attempt and never runs past the deadline, so a retry cannot
 * outlive the walk's budget. Returns the body (NULL on failure) and fills
 * `info`; on total failure `info` is left zeroed.
 */
static char *crawl_fetch_page(const mi_download_config *dcfg, const char *url,
                              double deadline, mi_fetch_info *info) {
    for (int attempt = 0; attempt <= CRAWL_FETCH_RETRIES; attempt++) {
        char *body = mi_download_fetch_document(dcfg, url, CRAWL_MAX_BODY, info);
        if (body) return body;
        int st = info->status;
        int retryable = st == 0 || st == 408 || st == 429 || (st >= 500 && st < 600);
        if (!retryable || attempt == CRAWL_FETCH_RETRIES) return NULL;
        double wait = CRAWL_FETCH_BACKOFF_SECONDS * (double)(1 << attempt);
        if (now_seconds() + wait >= deadline) return NULL;
        sleep_seconds(wait);
        mi_fetch_info_free(info); /* the next attempt starts clean */
    }
    return NULL;
}

/**
 * Read one page and record what it yields.
 *
 * The static fetch is tried first. If it finds no matching file and the browser
 * fallback is allowed, the page is read again through the browser — which is
 * what makes a JavaScript-built listing work.
 */
static int read_page(crawl_shared *s, const char *url, int depth) {
    const mi_crawl_config *cfg = s->cfg;
    const char *term = s->term;
    const char *start_url = s->start_url;
    double deadline = s->deadline;
    mi_crawl_result *res = s->res;
    dedup_set *dedup = s->dedup;
    frontier *f = s->f;
    mi_download_config dcfg;
    mi_download_config_init(&dcfg);
    dcfg.require_media = 0;
    dcfg.user_agent = cfg->user_agent;
    dcfg.proxy = cfg->proxy;
    /* The page fetch goes through the impersonation helper when a browser was
     * named: a site that fingerprints TLS refuses the plain fetch however it is
     * dressed, and the helper is the only path that wears a real handshake. It
     * falls back to libcurl on its own if the helper is missing. */
    dcfg.impersonate = cfg->impersonate;
    dcfg.cookies_text = cfg->cookies_text;
    dcfg.cookies_from_browser = cfg->cookies_from_browser;
    /* A crawl fetches many pages; a single slow one must not consume the whole
     * budget, so each fetch gets a fraction of it. */
    dcfg.timeout_seconds = 15;

    /*
     * The hit cap is enforced here as well as in the walk loop.
     *
     * A page can list hundreds of files, and collecting all of them because the
     * check only happened between pages would do far more work than the caller
     * asked for — and return a list longer than it wanted to render.
     */
    int room = res_hits_left(s);

    /*
     * A host in cooldown is not fetched.
     *
     * A crawl makes many requests to one host, so a host that has been refusing
     * us would be hammered page after page — which turns a soft block into a
     * hard one. The brain remembers the cooldown; the walk honours it.
     */
    char *host = cfg->brain ? mi_antibot_host_of(url) : NULL;
    if (cfg->brain && host) {
        int cd = mi_antibot_cooldown(cfg->brain, host);
        if (cd > 0) {
            mi_logf(MI_LOG_INFO, "crawl: %s is in cooldown for %ds; skipping", host, cd);
            free(host);
            return 0;
        }
        /* Reuse the identity that worked for this host, so the walk does not
         * present a new face on every page. */
        mi_site_identity id;
        if (mi_antibot_recall_identity(cfg->brain, host, &id)) {
            if (id.proxy) dcfg.proxy = id.proxy;
            if (id.impersonate) dcfg.impersonate = id.impersonate;
            if (id.user_agent) dcfg.user_agent = id.user_agent;
        }
    }

    /*
     * Conditional GET, so a re-crawl does not re-read an unchanged page.
     *
     * The validators the page returned last time are sent back; a 304 means the
     * cached copy is current and there is nothing to parse. This is what makes an
     * incremental crawl cheap rather than a full re-download every run.
     */
    char *prev_etag = NULL, *prev_lm = NULL;
    if (f && mi_frontier_get_validators(f, url, &prev_etag, &prev_lm)) {
        dcfg.if_none_match = prev_etag;
        dcfg.if_modified_since = prev_lm;
    }

    mi_fetch_info info;
    char *body = crawl_fetch_page(&dcfg, url, deadline, &info);
    pthread_mutex_lock(&s->lock);
    res->fetched++;
    pthread_mutex_unlock(&s->lock);
    if (info.not_modified) {
        mi_logf(MI_LOG_INFO, "crawl: %s is unchanged (304)", url);
        pthread_mutex_lock(&s->lock);
        res->not_modified++;
        pthread_mutex_unlock(&s->lock);
        mi_fetch_info_free(&info);
        free(prev_etag);
        free(prev_lm);
        if (cfg->brain && host) mi_antibot_note_success(cfg->brain, host);
        free(host);
        return 0;
    }
    /* Remember the validators this response carried, for the next run. */
    if (f && (info.etag || info.last_modified)) {
        mi_frontier_set_validators(f, url, info.etag, info.last_modified);
    }
    mi_fetch_info_free(&info);
    free(prev_etag);
    free(prev_lm);
    if (cfg->brain && host) {
        if (body) mi_antibot_note_success(cfg->brain, host);
    }

    /*
     * A challenge is not content. Recognising it here means the walk reports
     * "the site put up a wall" rather than reading the challenge markup for
     * links that are not there and concluding "no file matched".
     */
    if (body) {
        mi_captcha_challenge ch;
        if (mi_captcha_detect(body, strlen(body), &ch)) {
            pthread_mutex_lock(&s->lock);
            res->challenges_seen++;
            pthread_mutex_unlock(&s->lock);
            mi_logf(MI_LOG_INFO, "crawl: %s answered with a %s challenge", url,
                    mi_captcha_kind_name(ch.kind));
            mi_captcha_challenge_free(&ch);
            if (cfg->brain && host)
                mi_antibot_note_block(cfg->brain, host, MI_BLOCK_CAPTCHA);
            free(host);
            free(body);
            return 0;
        }
    }
    /*
     * A soft block: a 200 whose body is a wall. This is the failure that looks
     * like success, so it is counted and recorded rather than read for links.
     */
    if (body) {
        mi_block_kind soft = mi_antibot_soft_block(body, strlen(body));
        if (soft != MI_BLOCK_NONE) {
            pthread_mutex_lock(&s->lock);
            res->blocked++;
            pthread_mutex_unlock(&s->lock);
            mi_logf(MI_LOG_INFO, "crawl: %s is a soft block (%s)", url,
                    mi_antibot_kind_name(soft));
            if (cfg->brain && host) mi_antibot_note_block(cfg->brain, host, soft);
            free(host);
            free(body);
            return 0;
        }
    }
    /*
     * Content deduplication. The page is fingerprinted; if its content is a
     * near-duplicate of one already read, it is not read again — but its links
     * are still followed below, because they may reach pages that are new.
     */
    int dup = 0;
    if (cfg->dedup && dedup && body) {
        mi_html_node *doc = mi_html_parse(body, strlen(body));
        mi_simhash fp = doc ? mi_simhash_page(doc) : 0;
        if (doc) mi_html_free(doc);
        dup = res_dedup_check(s, fp);
    }
    if (dup) {
        pthread_mutex_lock(&s->lock);
        res->duplicates++;
        pthread_mutex_unlock(&s->lock);
        mi_logf(MI_LOG_WARN, "crawl: %s is a near-duplicate; links only", url);
    }
    if (cfg->brain && host) free(host);

    int links = 0, hits = 0, rendered = 0;
    size_t first_hit = 0;
    if (body) {
        size_t blen = strlen(body);

        /* First: what does this page reference? */
        mi_browse_result br;
        if (mi_browse_parse(body, blen, url, MI_BROWSE_ALL, &br) == MI_OK) {
            links = (int)br.count;
            first_hit = res->hit_count;
            for (size_t i = 0; i < br.count; i++) {
                const mi_browse_item *item = &br.items[i];
                if (!item->url) continue;
                if (is_asset(item->url)) continue;
                int score = mi_crawl_score(item->filename, item->title, term);
                if (score > 0) {
                    if (hits >= room) break;
                    if (res_add_hit(s, item, score, url, 0) >= 0) hits++;
                }
            }
            /* Follow the pages it links, if depth allows. */
            if (depth < cfg->max_depth) {
                for (size_t i = 0; i < br.count; i++) {
                    const char *u = br.items[i].url;
                    if (!u || !is_page(u)) continue;
                    if (cfg->same_host_only) {
                        char *h1 = host_of(u), *h2 = host_of(url);
                        int same = h1 && h2 && !strcasecmp(h1, h2);
                        free(h1);
                        free(h2);
                        if (!same) {
                            pthread_mutex_lock(&s->lock);
                            res->skipped_offsite++;
                            pthread_mutex_unlock(&s->lock);
                            continue;
                        }
                    }
                    frontier_add(s, u, depth + 1, term);
                }
            }
            mi_browse_result_free(&br);
        }
        free(body);
    } else {
        pthread_mutex_lock(&s->lock);
        res->failed++;
        pthread_mutex_unlock(&s->lock);
    }

    /*
     * Confirm the candidates from the response rather than the URL.
     *
     * A filename guessed from the URL is a guess: an HTML viewer named like a
     * file becomes a hit with nothing behind it, and a signed, extension-less
     * media address is dropped as a page. A ranged probe tells the two apart, so
     * a verified file outranks an unverified link and a page that looked like a
     * file is not reported as one.
     */
    if (first_hit < res->hit_count) {
        verify_page_hits(s, first_hit, res->hit_count - first_hit);
    }

    /*
     * The browser fallback.
     *
     * Only when the static pass found nothing matching on this page. Driving a
     * browser costs far more than the crawl is worth, so it is reserved for the
     * pages that look empty — which are exactly the ones a plain fetch cannot
     * read.
     *
     * The driven session is preferred when there is one: it can wait for the
     * network to settle and click what needs clicking, so a listing fetched by
     * XHR or hidden behind a tab is reachable. The renderer is the fallback for
     * when there is no session available, and can only look.
     */
    /*
     * The extractor's media, when the page has one.
     *
     * This is the *hardening* of the walk rather than a separate feature: a
     * streaming page's media URL is not in its links — it is signed and computed
     * at play time — so a crawl that only reads links reports the channel as the
     * result and misses the video. Asking the extractor is the same job a crawl
     * does, done by the tool that can.
     *
     * Deliberately *not* gated on `hits == 0`. A streaming page's links include
     * its own channel and navigation, so the walk usually has a hit already —
     * measured: a watch page matched `@RickAstleyYT` and the video was never
     * looked for. When the extractor knows the page it is the authority, so it
     * is asked regardless and its result is placed first.
     *
     * The hit's URL is the *page*, not a stream URL: a stream URL is signed,
     * bound to this session, and expires within hours, so storing one would give
     * a download that fails tomorrow. Recording the page means the download
     * resolves it again when it runs.
     */
    int extracted = 0;
    /*
     * The extractor runs on the *start* page only.
     *
     * It is a Python subprocess costing a second or more, and it is asked about
     * the page the caller named — a video page. Asking it about every page the
     * walk reaches would be up to forty subprocesses for no result: a crawl of a
     * watch page measured 75 seconds that way, where the start page alone
     * answers in two. The pages a crawl follows are listing and navigation
     * pages, which are what the link pass is for.
     */
    int is_start = start_url && !strcmp(url, start_url);
    if (is_start && cfg->extract_enabled && now_seconds() < deadline) {
        mi_extract_config ecfg;
        mi_extract_config_init(&ecfg);
        ecfg.binary = cfg->extractor_path;
        ecfg.gallery_binary = cfg->gallery_extractor_path;
        ecfg.torrent_binary = cfg->torrent_extractor_path;
        ecfg.torrent_site = cfg->torrent_site;
        ecfg.cookies_text = cfg->cookies_text;
        ecfg.cookies_from_browser = cfg->cookies_from_browser;
        ecfg.proxy = cfg->proxy;
        ecfg.impersonate = cfg->impersonate;
        /*
         * The availability gate must be for the tool this URL *routes to*, not
         * for yt-dlp. A torrent index routes to the torrent extractor and a
         * gallery host to gallery-dl, and a host with py1337x but no yt-dlp would
         * otherwise never be asked — the crawl would silently report nothing.
         */
        mi_extract_kind ekind = mi_extract_pick(&ecfg, url);
        if (mi_extract_available_kind(&ecfg, ekind)) {
            mi_extract_result er;
            if (mi_extract(&ecfg, url, &er) == MI_OK &&
                (er.format_count > 0 || er.entry_count > 0)) {
                extracted = 1;
                /*
                 * The page itself is a result only when the extractor resolved a
                 * *single item*. A list answer (a torrent search, a channel, a
                 * search) carries its results in `entries` — one hit each, added
                 * below — and its top-level title is the *query*, not a file, so
                 * adding a page hit for it would point the caller at the search
                 * page it already knows about.
                 */
                int score = er.entry_count > 0 ? 0 : mi_crawl_score(er.title, er.title, term);
                if (score > 0) {
                    mi_browse_item item;
                    memset(&item, 0, sizeof item);
                    item.url = (char *)url;
                    item.filename = er.title ? er.title : (char *)url;
                    item.title = er.title;
                    item.mime = (char *)"";
                    item.bytes = -1;
                    /* The resolved hit and its qualities are built under the lock,
                     * because another worker's hit would otherwise realloc the
                     * array out from under `h`. */
                    pthread_mutex_lock(&s->lock);
                    if ((int)res->hit_count < s->max_hits && add_hit(res, &item, score, url, 0)) {
                        hits++;
                        /* Marked so it ranks above the page's own links at an
                         * equal score — a channel link matches a search just as
                         * well as the video does. */
                        mi_crawl_hit *h = &res->hits[res->hit_count - 1];
                        h->resolved = 1;
                        /* The page's own poster, when the extractor reported one. */
                        if (er.thumbnail && *er.thumbnail) h->thumbnail = mi_strdup(er.thumbnail);
                        /*
                         * The renditions, largest first, so the console can offer
                         * the choice. One entry per distinct height, carrying the
                         * best bitrate at that height — the extractor lists many
                         * variants of the same size (a codec each) and a picker
                         * showing all of them would be a wall of near-duplicates.
                         */
                        for (size_t fi = 0; fi < er.format_count; fi++) {
                            const mi_extract_format *fmt = &er.formats[fi];
                            if (!fmt->has_video) continue;
                            int height = fmt->height;
                            if (height <= 0) continue;
                            int seen = 0;
                            for (size_t qi = 0; qi < h->quality_count; qi++) {
                                if (h->qualities[qi].height == height) {
                                    seen = 1;
                                    /* Prefer the one that states a size. */
                                    if (h->qualities[qi].bytes < 0 && fmt->filesize > 0) {
                                        h->qualities[qi].bytes = fmt->filesize;
                                    }
                                    break;
                                }
                            }
                            if (seen) continue;
                            mi_crawl_quality *grown = mi_realloc(
                                h->qualities, (h->quality_count + 1) * sizeof *grown);
                            if (!grown) break;
                            h->qualities = grown;
                            mi_crawl_quality *q = &h->qualities[h->quality_count++];
                            memset(q, 0, sizeof *q);
                            q->height = height;
                            q->label = mi_asprintf("%dp", height);
                            q->bytes = fmt->filesize;
                            q->best = 0;
                        }
                        /* Largest first, and marked, so "Best" needs no guessing
                         * in the console. */
                        for (size_t qi = 0; qi + 1 < h->quality_count; qi++) {
                            for (size_t qj = qi + 1; qj < h->quality_count; qj++) {
                                if (h->qualities[qj].height > h->qualities[qi].height) {
                                    mi_crawl_quality tmp = h->qualities[qi];
                                    h->qualities[qi] = h->qualities[qj];
                                    h->qualities[qj] = tmp;
                                }
                            }
                        }
                        if (h->quality_count > 0) h->qualities[0].best = 1;
                    }
                    pthread_mutex_unlock(&s->lock);
                }
                /*
                 * A *list* answer: one hit per entry.
                 *
                 * A torrent search is a list — each result is a torrent with its
                 * own page, and the caller wants all of them, not the first. yt-dlp
                 * returns the same shape for a search or a channel, so both are
                 * handled here. Entries carry no formats, so there are no
                 * renditions to attach; the hit's URL is the entry's page, which a
                 * later resolve (for a video) or magnet fetch (for a torrent) acts
                 * on.
                 */
                for (size_t ei = 0; ei < er.entry_count; ei++) {
                    if (res_add_entry_hit(s, &er.entries[ei], url)) hits++;
                }
            }
            mi_extract_result_free(&er);
        }
    }

    /*
     * The browser fallback, and what bounds it.
     *
     * A browser costs several seconds a page, and there is no cheap way to know
     * in advance whether a page needs one: the honest test is to render it and
     * compare, which is the cost being avoided. Two signals were tried and both
     * were wrong — "no match on this page" renders nearly every page of a crawl
     * for a specific name, and "few links on this page" skips a JavaScript
     * listing whose static HTML is a full navigation with no content in it
     * (measured: 27 content links and still nothing readable).
     *
     * So the correct trigger is kept and the *number* of browsers is capped
     * instead. A crawl may drive `max_browser_pages` pages; after that it reads
     * only what the HTML offers. That makes the cost predictable without making
     * the result wrong — the first few unreadable pages are exactly the ones a
     * JavaScript site hides its listing behind.
     */
    if (hits == 0 && !extracted && cfg->use_browser_fallback && now_seconds() < deadline) {
        pthread_mutex_lock(&s->lock);
        int browser_cap = cfg->max_browser_pages > 0 ? cfg->max_browser_pages
                                            : CRAWL_DEFAULT_MAX_BROWSER_PAGES;
        int browser_ok = s->res->rendered < browser_cap;
        pthread_mutex_unlock(&s->lock);
        if (browser_ok &&
            cfg->interactive_pool && mi_interactive_pool_available(cfg->interactive_pool)) {
            int was_rendered = 0;
            hits += read_page_driven(s, url, depth, &was_rendered);
            if (was_rendered) {
                /*
                 * read_page_driven recorded this page's entry itself, so return
                 * rather than recording a second one for the same URL. The
                 * caller still gets the hit count, which is what it uses.
                 */
                (void)rendered;
                return hits;
            }
        }
        if (browser_ok && cfg->render_pool && mi_render_pool_available(cfg->render_pool)) {
            mi_render_result rr;
            memset(&rr, 0, sizeof rr);
            if (mi_render_run(cfg->render_pool, url, MI_RENDER_DOM, &rr) == MI_RENDER_OK &&
                rr.data && rr.len > 0) {
                rendered = 1;
                pthread_mutex_lock(&s->lock);
                res->rendered++;
                pthread_mutex_unlock(&s->lock);
                mi_browse_result br;
                if (mi_browse_parse((const char *)rr.data, rr.len, url, MI_BROWSE_ALL, &br) == MI_OK) {
                    if ((int)br.count > links) links = (int)br.count;
                    size_t rfirst = res->hit_count;
                    for (size_t i = 0; i < br.count; i++) {
                        const mi_browse_item *item = &br.items[i];
                        if (!item->url || is_asset(item->url)) continue;
                        int score = mi_crawl_score(item->filename, item->title, term);
                        if (score > 0) {
                            if (hits >= room) break;
                            if (res_add_hit(s, item, score, url, 1) >= 0) hits++;
                        }
                    }
                    if (rfirst < res->hit_count) {
                        verify_page_hits(s, rfirst, res->hit_count - rfirst);
                    }
                    if (depth < cfg->max_depth) {
                        for (size_t i = 0; i < br.count; i++) {
                            const char *u = br.items[i].url;
                            if (!u || !is_page(u)) continue;
                            frontier_add(s, u, depth + 1, term);
                        }
                    }
                    mi_browse_result_free(&br);
                }
            }
            mi_render_result_free(&rr);
        }
    }

    res_add_page(s, url, links, hits, rendered, body ? 200 : 0);
    return hits;
}

/*
 * Which of two equally-good matches to prefer.
 *
 * A file often appears on a page twice: once as a *viewer* link and once as the
 * bytes. GitHub is the clearest case — `/blob/master/Makefile` is an HTML page
 * *about* the file, `/raw/refs/heads/master/Makefile` is the file. Both carry
 * the same filename, so they score the same, and picking the viewer downloads
 * the page rather than the file. Measured: the blob URL saved 748,550 bytes of
 * HTML instead of the 80 KB Makefile.
 *
 * The general rule is the one `is_page` already answers: a URL with a real file
 * extension, or an explicit raw/download path, is the file; anything else is a
 * page about it. So a page never outranks a non-page at the same score.
 */
static int prefers_over(const mi_crawl_hit *a, const mi_crawl_hit *b) {
    if (a->score != b->score) return a->score > b->score;
    /*
     * A hit the extractor produced outranks a link the walk found.
     *
     * A streaming page's own links are its channel and its navigation, and one
     * of them will match the term just as well as the video does — measured: a
     * watch page returned `@RickAstleyYT` and the video both at score 80. The
     * extractor's answer is the thing the caller asked for, so it goes first.
     */
    if (a->resolved != b->resolved) return a->resolved;
    /*
     * A raw/download path first, because it is the explicit statement that this
     * URL is the bytes. It has to be checked before `is_page`, which cannot help
     * here: "Makefile" has no extension, so both the viewer link and the raw
     * link look extension-less and `is_page` calls them both pages.
     */
    int a_raw = strstr(a->url, "/raw/") != NULL || strstr(a->url, "raw=true") != NULL ||
                strstr(a->url, "?download") != NULL || strstr(a->url, "/download/") != NULL;
    int b_raw = strstr(b->url, "/raw/") != NULL || strstr(b->url, "raw=true") != NULL ||
                strstr(b->url, "?download") != NULL || strstr(b->url, "/download/") != NULL;
    if (a_raw != b_raw) return a_raw;
    /* Then a real file extension beats a viewer page. */
    int a_page = is_page(a->url), b_page = is_page(b->url);
    if (a_page != b_page) return !a_page;
    return 0;
}

/*
 * Fetch and store a host's robots.txt, once.
 *
 * The fetch goes through the same path as any other page — same identity, proxy
 * and impersonation — so a site that fingerprints TLS is read the same way here
 * as everywhere else. A robots.txt that cannot be read (a 404, a 5xx, a
 * network error) is stored as empty, which allows everything: a broken
 * robots.txt must not take a site off the web, and it must not be re-fetched on
 * every page.
 */
/* The scheme of a URL ("http" or "https"), defaulting to https. */
static const char *scheme_of(const char *url) {
    if (url && !strncasecmp(url, "http://", 7)) return "http";
    return "https";
}

/*
 * The authority of a URL: host and port, without userinfo, path or query.
 *
 * Distinct from mi_antibot_host_of, which strips the port because it is used as
 * an identity key. A *request* needs the port back: building
 * `http://<host>/robots.txt` from the port-stripped host sent the fetch to port
 * 80, so a site on any other port — every test server, and real sites on :8080 —
 * never had its robots.txt read at all.
 */
static char *authority_of(const char *url) {
    if (!url) return NULL;
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *at = strchr(p, '@');
    const char *slash = strchr(p, '/');
    if (at && (!slash || at < slash)) p = at + 1;
    const char *end = p;
    while (*end && *end != '/' && *end != '?' && *end != '#') end++;
    if (end == p) return NULL;
    return mi_strndup(p, (size_t)(end - p));
}

/*
 * Fetch a host's robots.txt, once.
 *
 * robots.txt is served over the same scheme and authority as the page being
 * walked, so both are taken from the page URL rather than assumed. `host` is the
 * port-stripped key the rules are stored under (the same key the politeness
 * check uses); `page_url` is where the robots.txt lives.
 */
static void fetch_robots(const mi_crawl_config *cfg, const char *host, const char *page_url) {
    char *authority = authority_of(page_url);
    if (!authority) return;
    char *url = mi_asprintf("%s://%s/robots.txt", scheme_of(page_url), authority);
    free(authority);
    if (!url) return;
    mi_download_config dcfg;
    mi_download_config_init(&dcfg);
    dcfg.require_media = 0;
    dcfg.user_agent = cfg->user_agent;
    dcfg.proxy = cfg->proxy;
    dcfg.impersonate = cfg->impersonate;
    dcfg.timeout_seconds = 10;
    /* robots.txt is small; a cap keeps a hostile host from streaming forever. */
    char *text = mi_download_fetch_text(&dcfg, url, 512 * 1024);
    mi_politeness_set_robots(cfg->politeness, host, text ? text : "", cfg->user_agent);
    if (text) {
        mi_logf(MI_LOG_INFO, "crawl: read robots.txt for %s (%zu bytes)", host,
                strlen(text));
    }
    free(text);
    free(url);
}

/*
 * The most child sitemaps one index may name before the rest are ignored.
 *
 * A large site shards its index into hundreds of child sitemaps, and following
 * every one would spend the whole budget reading sitemaps instead of pages. The
 * cap keeps the seed useful without letting it take over.
 */
#define CRAWL_MAX_CHILD_SITEMAPS 20
/* The most URLs one walk may take from sitemaps. A sitemap-rich site lists its
 * whole content, and pushing all of it at depth 0 would consume the page budget
 * before link-following starts. This bounds the seed so the walk still gets to
 * follow links; the rest of the budget is the walk's. */
#define CRAWL_SITEMAP_SEED_FRACTION 4

/*
 * A recency bonus from an entry's `<lastmod>`, higher for newer.
 *
 * The date is W3C-ish (`2024-01-01` or `2024-01-01T09:00:00Z`); only the year
 * and month are read, which is all a *ranking* needs — this orders a frontier,
 * it does not schedule a crawl. The result is a fixed function of the string,
 * with no clock in it, so a test is not time-dependent. Bounded to the same
 * order as the site `priority` contribution so neither signal swamps the other,
 * and both stay well under the term-match bonus.
 */
static int lastmod_bonus(const char *lastmod) {
    if (!lastmod || !*lastmod) return 0;
    int y = 0, m = 0;
    if (sscanf(lastmod, "%d-%d", &y, &m) < 1) return 0;
    if (y < 1990 || y > 2100) return 0;
    int bonus = (y - 2000) * 2;
    if (m >= 1 && m <= 12) bonus += (m - 1) / 3; /* 0..3 within the year */
    if (bonus < 0) bonus = 0;
    if (bonus > 60) bonus = 60;
    return bonus;
}

/*
 * Seed the frontier from the host's sitemaps.
 *
 * The sitemaps come from the robots.txt `Sitemap:` directives when there are
 * any, plus the conventional `/sitemap.xml`. A sitemapindex names further
 * sitemaps, which are followed — bounded, so an index that names hundreds of
 * children cannot run away.
 *
 * Every `<loc>` is pushed at depth 0 with a priority from the sitemap's own
 * `priority` and `lastmod`: a URL the site ranked high, or that changed
 * recently, is fetched before a stale one. And the seed is *bounded to a
 * fraction of the page budget*, because pushing every URL a sitemap-rich site
 * lists would consume the whole walk before link-following started — the
 * failure this fixes.
 */
static int seed_from_sitemaps(const mi_crawl_config *cfg, frontier *f,
                              const char *host, int max_pages, const char *term) {
    char **maps = cfg->politeness ? mi_politeness_sitemaps(cfg->politeness, host) : NULL;

    char **queue = NULL;
    size_t queue_n = 0;
    size_t queue_cap = 0;
    for (size_t i = 0; maps && maps[i]; i++) {
        if (queue_n == queue_cap) {
            queue_cap = queue_cap ? queue_cap * 2 : 4;
            char **g = mi_realloc(queue, queue_cap * sizeof *g);
            if (!g) break;
            queue = g;
        }
        queue[queue_n++] = mi_strdup(maps[i]);
    }
    if (queue_n == 0) {
        if (queue_cap == 0) { queue = mi_calloc(2, sizeof *queue); queue_cap = 2; }
        char *def = mi_asprintf("https://%s/sitemap.xml", host);
        if (def) queue[queue_n++] = def;
    }
    mi_politeness_free_sitemaps(maps);

    mi_download_config dcfg;
    mi_download_config_init(&dcfg);
    dcfg.require_media = 0;
    dcfg.user_agent = cfg->user_agent;
    dcfg.proxy = cfg->proxy;
    dcfg.impersonate = cfg->impersonate;
    dcfg.cookies_text = cfg->cookies_text;
    dcfg.cookies_from_browser = cfg->cookies_from_browser;
    dcfg.timeout_seconds = 15;

    /* The seed is a fraction of the page budget, so the walk still gets to
     * follow links; and the whole seed is capped regardless. */
    int seed_cap = max_pages / CRAWL_SITEMAP_SEED_FRACTION;
    if (seed_cap < 1) seed_cap = 1;
    int child_seen = 0;
    char *folded_term = fold(term);
    int pushed = 0;
    for (size_t qi = 0; qi < queue_n && pushed < seed_cap; qi++) {
        mi_fetch_info info;
        char *body = mi_download_fetch_document(&dcfg, queue[qi], 8 * 1024 * 1024, &info);
        mi_fetch_info_free(&info);
        if (!body) continue;
        mi_sitemap sm;
        if (mi_sitemap_parse((const unsigned char *)body, strlen(body), &sm) == 0) {
            for (size_t i = 0; i < sm.count && pushed < seed_cap; i++) {
                if (sm.is_index) {
                    if (child_seen >= CRAWL_MAX_CHILD_SITEMAPS) break;
                    if (queue_n == queue_cap) {
                        queue_cap = queue_cap ? queue_cap * 2 : 4;
                        char **g = mi_realloc(queue, queue_cap * sizeof *g);
                        if (!g) break;
                        queue = g;
                    }
                    queue[queue_n++] = mi_strdup(sm.entries[i].loc);
                    child_seen++;
                } else {
                    const mi_sitemap_entry *e = &sm.entries[i];
                    /* The site's own priority, scaled; a recency bonus from
                     * `lastmod`; plus a bonus for a URL whose path carries the
                     * term — the same signal the walk's own ordering uses. */
                    int prio = 0;
                    if (e->priority > 0) prio += (int)(e->priority * 20);
                    prio += lastmod_bonus(e->lastmod);
                    if (url_mentions_term(e->loc, folded_term)) prio += 100;
                    if (mi_frontier_push_prio(f, e->loc, 0, prio) == 0) pushed++;
                }
            }
            if (sm.count > 0) {
                mi_logf(MI_LOG_INFO, "crawl: %s yielded %zu %s", queue[qi], sm.count,
                        sm.is_index ? "child sitemaps" : "urls");
            }
            mi_sitemap_free(&sm);
        }
        free(body);
    }
    free(folded_term);
    for (size_t i = 0; i < queue_n; i++) free(queue[i]);
    free(queue);
    return pushed;
}

/*
 * One worker: take a URL from the frontier and read it, until the walk is done.
 *
 * Several of these run at once. Each takes a URL, honours the host's robots and
 * pace, reads the page, and repeats. The politeness slot is reserved atomically
 * (see mi_politeness_reserve), so parallel workers do not defeat the per-host
 * pacing; the frontier is thread-safe; and everything shared — the result, the
 * counters — is guarded by the crawl's lock.
 */
static void *crawl_worker(void *arg) {
    crawl_shared *s = arg;
    const mi_crawl_config *cfg = s->cfg;
    for (;;) {
        if (!crawl_should_continue(s)) break;
        char *url = NULL;
        int depth = 0;
        if (frontier_take(s->f, &url, &depth) <= 0) break;
        if (!crawl_take_page_slot(s)) { free(url); break; }

        /*
         * robots.txt and pace, before the page is fetched.
         *
         * A crawler that ignores robots is a liability, and one that fetches as
         * fast as the network allows turns a soft block into a hard one. Both
         * are per host, and the rules are fetched once — the first time the walk
         * reaches a host — then cached in the shared politeness state.
         */
        char *host = mi_antibot_host_of(url);
        int skip = 0;
        if (host && cfg->politeness) {
            if (cfg->obey_robots) {
                if (!mi_politeness_have_robots(cfg->politeness, host)) {
                    fetch_robots(cfg, host, url);
                }
                if (!mi_politeness_allowed(cfg->politeness, host, url)) {
                    mi_logf(MI_LOG_WARN, "crawl: robots.txt disallows %s", url);
                    skip = 1;
                }
            }
            if (!skip) {
                /*
                 * Reserve this host's next slot and wait for it.
                 *
                 * Reserving (rather than reading a delay and then noting the
                 * request) is what keeps the pace honest when several workers
                 * share a host: each gets its own turn, so the interval is the
                 * host's, not the workers'.
                 */
                double wait = mi_politeness_reserve(cfg->politeness, host);
                if (wait > 0 && now_seconds() + wait >= s->deadline) {
                    /* The wait would blow the budget. Skip this host rather than
                     * abandoning the whole walk: a URL on another host may still
                     * fit, and the deadline check at the top of the loop stops
                     * the worker once nothing does. */
                    skip = 1;
                } else if (wait > 0) {
                    struct timespec ts = {.tv_sec = (time_t)wait,
                                          .tv_nsec = (long)((wait - (double)(time_t)wait) * 1e9)};
                    nanosleep(&ts, NULL);
                }
            }
        }
        free(host);

        if (!skip) read_page(s, url, depth);
        free(url);
    }
    return NULL;
}

int mi_crawl(const mi_crawl_config *cfg, const char *site_url, const char *term,
             mi_crawl_result *out) {
    if (!cfg || !site_url || !out) return MI_ERR_ARG;
    memset(out, 0, sizeof *out);

    /*
     * Only http(s) is understood — that is a mechanism constraint, not a policy.
     *
     * The SSRF policy (no loopback, no link-local) is deliberately NOT applied
     * here. It belongs at the boundary where untrusted input arrives, which is
     * the server endpoint, and it is already checked there. Imposing it in the
     * library would couple a crawler to the renderer's rules and make the walk
     * untestable against a local server.
     */
    if (strncasecmp(site_url, "http://", 7) != 0 && strncasecmp(site_url, "https://", 8) != 0) {
        return MI_ERR_ARG;
    }

    int max_pages = cfg->max_pages > 0 ? cfg->max_pages : CRAWL_DEFAULT_MAX_PAGES;
    if (max_pages > CRAWL_HARD_MAX_PAGES) max_pages = CRAWL_HARD_MAX_PAGES;
    int max_hits = cfg->max_hits > 0 ? cfg->max_hits : CRAWL_DEFAULT_MAX_HITS;
    double budget =
        cfg->budget_seconds > 0 ? cfg->budget_seconds : CRAWL_DEFAULT_BUDGET_SECONDS;
    double started = now_seconds();
    double deadline = started + budget;

    /*
     * The frontier: an in-process queue, or one shared through Turso when the
     * caller named a database and a run id. A shared frontier is how several
     * crawlers walk one site once rather than once each.
     */
    frontier *f = NULL;
    if (cfg->frontier_run_id && *cfg->frontier_run_id) {
        char ferr[256];
        if (cfg->frontier_db_url && *cfg->frontier_db_url) {
            /* A remote database the operator named wins over a local file. */
            f = mi_frontier_new_turso(cfg->frontier_db_url, cfg->frontier_token,
                                      cfg->frontier_run_id, ferr, sizeof ferr);
        } else if (cfg->frontier_db_path && *cfg->frontier_db_path) {
            f = mi_frontier_new_sqlite(cfg->frontier_db_path, cfg->frontier_run_id,
                                       ferr, sizeof ferr);
        }
        if (!f && (cfg->frontier_db_url || cfg->frontier_db_path)) {
            mi_logf(MI_LOG_ERROR, "crawl: shared frontier unavailable (%s); "
                                  "falling back to an in-process queue", ferr);
        }
    }
    if (!f) f = mi_frontier_new_memory();
    if (!f) return MI_ERR_NOMEM;

    /*
     * A shared frontier is swept before the walk starts.
     *
     * A worker that died mid-page left its claim in place; without this the
     * frontier slowly silts up with URLs no one will finish, and the crawl stops
     * making progress without erroring. Only *stale* claims are taken, so a
     * second worker already walking the same run keeps its in-flight pages.
     */
    if (cfg->frontier_run_id && *cfg->frontier_run_id) {
        int lease = cfg->frontier_lease_seconds > 0 ? cfg->frontier_lease_seconds
                                                     : CRAWL_DEFAULT_FRONTIER_LEASE;
        int n = mi_frontier_reclaim_stale(f, lease);
        if (n > 0) mi_logf(MI_LOG_INFO, "crawl: reclaimed %d stale claim(s)", n);
    }

    dedup_set dedup = {0};
    char *seed_norm = mi_url_normalize(site_url);
    mi_frontier_push_prio(f, seed_norm ? seed_norm : site_url, 0,
                          url_priority(seed_norm ? seed_norm : site_url, 0, NULL));
    strset_add(f, seed_norm ? seed_norm : site_url);
    free(seed_norm);

    /*
     * Seed from the host's sitemaps, before the walk. A sitemap reaches pages no
     * link does, so it is the best coverage a crawler can get; the walk then
     * follows links *and* visits what the sitemap named.
     */
    if (cfg->use_sitemap && cfg->obey_robots && cfg->politeness) {
        char *shost = mi_antibot_host_of(site_url);
        if (shost) {
            if (!mi_politeness_have_robots(cfg->politeness, shost))
                fetch_robots(cfg, shost, site_url);
            int n = seed_from_sitemaps(cfg, f, shost, max_pages, term);
            if (n > 0) mi_logf(MI_LOG_INFO, "crawl: seeded %d url(s) from sitemaps", n);
            free(shost);
        }
    }

    crawl_shared shared;
    memset(&shared, 0, sizeof shared);
    shared.cfg = cfg;
    shared.f = f;
    shared.term = term;
    shared.start_url = site_url;
    shared.res = out;
    shared.started = started;
    shared.deadline = deadline;
    shared.max_pages = max_pages;
    shared.max_hits = max_hits;
    shared.dedup = cfg->dedup ? &dedup : NULL;
    pthread_mutex_init(&shared.lock, NULL);

    /*
     * The walk.
     *
     * Several workers fetch at once. The frontier hands each one a URL, the
     * politeness state gives each host its own pace, and the result and counters
     * are guarded by the shared lock. `workers` of 1 restores the strictly
     * serial walk, which is what a deterministic test wants; the default is
     * several, because a serial walk cannot read enough of a large site inside a
     * budget to matter.
     */
    int workers = cfg->workers > 0 ? cfg->workers : CRAWL_DEFAULT_WORKERS;
    if (workers > CRAWL_MAX_WORKERS) workers = CRAWL_MAX_WORKERS;
    if (workers < 1) workers = 1;
    if (workers > max_pages) workers = max_pages;
    if (workers < 1) workers = 1;

    if (workers == 1) {
        crawl_worker(&shared);
    } else {
        pthread_t *threads = mi_calloc((size_t)workers, sizeof *threads);
        if (threads) {
            int started_n = 0;
            for (int i = 0; i < workers; i++) {
                if (pthread_create(&threads[i], NULL, crawl_worker, &shared) == 0) {
                    started_n++;
                } else {
                    break;
                }
            }
            /* If no thread could start, fall back to walking in this thread so a
             * crawl still happens. */
            if (started_n == 0) crawl_worker(&shared);
            for (int i = 0; i < started_n; i++) pthread_join(threads[i], NULL);
            free(threads);
        } else {
            crawl_worker(&shared);
        }
    }

    /* Best match first, and among equals the file rather than a page about it —
     * the caller asked for a file by name, so the closest name that is also the
     * bytes is the answer. */
    for (size_t i = 0; i + 1 < out->hit_count; i++) {
        for (size_t j = i + 1; j < out->hit_count; j++) {
            if (prefers_over(&out->hits[j], &out->hits[i])) {
                mi_crawl_hit tmp = out->hits[i];
                out->hits[i] = out->hits[j];
                out->hits[j] = tmp;
            }
        }
    }

    free(dedup.items);
    mi_frontier_free(f);
    return MI_OK;
}
