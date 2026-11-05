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
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "mi_browse.h"
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
#define CRAWL_HARD_MAX_PAGES 200
/* How long to let a click's effect arrive before reading, and how long to let a
 * page settle after load. Short on purpose: a crawl drives many pages, and a
 * per-page cost of ten seconds makes the walk useless even when it is right. */
#define CLICK_SETTLE_SECONDS 0.6
#define READ_SETTLE_SECONDS 2.0

void mi_crawl_config_init(mi_crawl_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->max_depth = 2;
    cfg->max_pages = 40;
    cfg->max_hits = 50;
    cfg->budget_seconds = 45.0;
    cfg->same_host_only = 0;
    cfg->use_browser_fallback = 1;
    cfg->max_browser_pages = 3;
    cfg->extract_enabled = 1;
    cfg->extractor_path = NULL;
    cfg->user_agent = NULL;
    cfg->render_pool = NULL;
    cfg->interactive_pool = NULL;
    cfg->click_selectors = NULL;
    cfg->click_selector_count = 0;
    cfg->settle_seconds = 2.0;
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------ */
/* The frontier                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    char *url;
    int depth;
} frontier_entry;

typedef struct {
    frontier_entry *items;
    size_t head; /* the next to take */
    size_t count;
    size_t cap;
} frontier;

static int frontier_push(frontier *f, const char *url, int depth) {
    if (f->count == f->cap) {
        size_t want = f->cap ? f->cap * 2 : 64;
        frontier_entry *grown = mi_realloc(f->items, want * sizeof *grown);
        if (!grown) return -1;
        f->items = grown;
        f->cap = want;
    }
    f->items[f->count].url = mi_strdup(url);
    if (!f->items[f->count].url) return -1;
    f->items[f->count].depth = depth;
    f->count++;
    return 0;
}

static int frontier_empty(const frontier *f) { return f->head >= f->count; }

/** Take the next entry. The frontier keeps ownership of the string; the caller
 *  must not free it. */
static frontier_entry frontier_take(frontier *f) {
    frontier_entry e = f->items[f->head];
    f->head++;
    return e;
}

static void frontier_free(frontier *f) {
    for (size_t i = 0; i < f->count; i++) free(f->items[i].url);
    free(f->items);
    memset(f, 0, sizeof *f);
}

/* ------------------------------------------------------------------ */
/* The visited set                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char **items;
    size_t count, cap;
} strset;

static int strset_add(strset *s, const char *v) {
    for (size_t i = 0; i < s->count; i++) {
        if (!strcmp(s->items[i], v)) return 0; /* already there */
    }
    if (s->count == s->cap) {
        size_t want = s->cap ? s->cap * 2 : 128;
        char **grown = mi_realloc(s->items, want * sizeof *grown);
        if (!grown) return -1;
        s->items = grown;
        s->cap = want;
    }
    s->items[s->count] = mi_strdup(v);
    if (!s->items[s->count]) return -1;
    s->count++;
    return 1;
}

static int strset_has(const strset *s, const char *v) {
    for (size_t i = 0; i < s->count; i++) {
        if (!strcmp(s->items[i], v)) return 1;
    }
    return 0;
}

static void strset_free(strset *s) {
    for (size_t i = 0; i < s->count; i++) free(s->items[i]);
    free(s->items);
    memset(s, 0, sizeof *s);
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
         * Fall back to "every word appears somewhere", which catches a term
         * typed in a different order from the filename ("datasheet stm32").
         */
        char *copy = mi_strdup(term);
        if (copy) {
            int words = 0, found = 0;
            char *save = NULL;
            for (char *w = strtok_r(copy, " -_./+", &save); w; w = strtok_r(NULL, " -_./+", &save)) {
                words++;
                char *wf = fold(w);
                if (wf) {
                    if ((f && strstr(f, wf)) || (ti && strstr(ti, wf))) found++;
                    free(wf);
                }
            }
            free(copy);
            if (words > 0 && found == words) score = 45;
            else if (words > 0 && found > 0) score = 20;
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
                                     ".shtml", ".cgi", ".pl", NULL};
    for (size_t i = 0; PAGE_EXT[i]; i++) {
        if (!strncasecmp(dot, PAGE_EXT[i], strlen(PAGE_EXT[i]))) return 1;
    }
    return 0;
}

/** Certainly not content: a page's own scripts, styles, fonts and icons. */
static int is_asset(const char *url) {
    const char *q = strchr(url, '?');
    size_t n = q ? (size_t)(q - url) : strlen(url);
    static const char *ASSET_EXT[] = {".css",  ".js",   ".mjs",  ".map",  ".woff",
                                      ".woff2", ".ttf", ".eot",  ".ico",  ".svg",
                                      ".png",  ".webp", ".avif", NULL};
    for (size_t i = 0; ASSET_EXT[i]; i++) {
        size_t elen = strlen(ASSET_EXT[i]);
        if (n >= elen && !strncasecmp(url + n - elen, ASSET_EXT[i], elen)) return 1;
    }
    return 0;
}

/**
 * Read a page by *driving* a browser, rather than only looking at it.
 *
 * A render loads a page, waits, and dumps the DOM. That is enough for a page
 * that builds its list on load, and useless for one that fetches the list by XHR
 * after settling, or hides it behind a tab, a consent button or a "show all".
 *
 * A session can do both: wait for the network to go quiet, click whatever the
 * caller says needs clicking, and then read the links the browser actually has —
 * including ones the DOM dump misses because they were attached after it was
 * taken.
 *
 * Returns the number of hits added. `*rendered` is set when the browser was
 * actually used, so the caller can report that the page needed it.
 */
static int read_page_driven(mi_crawl_result *res, const mi_crawl_config *cfg,
                            frontier *f, strset *visited, const char *url, int depth,
                            const char *term, double deadline, int *rendered) {
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
    res->rendered++;

    int max_hits = cfg->max_hits > 0 ? cfg->max_hits : 50;
    int hits = 0;
    int links = 0;

    /*
     * Settle, then click, then read.
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
        int room = max_hits - (int)res->hit_count;
        if (room < 0) room = 0;
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
                                if (add_hit(res, &item, score, url, 1)) hits++;
                            }
                            free(fname);
                        }
                        /* Follow it if it looks like a page to walk into. */
                        if (is_page(u) && depth < cfg->max_depth && !strset_has(visited, u)) {
                            frontier_push(f, u, depth + 1);
                        }
                    }
                    free(u);
                }
            }
            if (!nl) break;
            p = nl + 1;
        }
    }

    mi_interactive_close(it);
    add_page(res, url, links, hits, 1, 200);
    return hits;
}

/**
 * Read one page and record what it yields.
 *
 * The static fetch is tried first. If it finds no matching file and the browser
 * fallback is allowed, the page is read again through the browser — which is
 * what makes a JavaScript-built listing work.
 */
static int read_page(mi_crawl_result *res, const mi_crawl_config *cfg,
                     frontier *f, strset *visited, const char *url, int depth,
                     const char *term, double deadline, double started) {
    mi_download_config dcfg;
    mi_download_config_init(&dcfg);
    dcfg.require_media = 0;
    dcfg.user_agent = cfg->user_agent;
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
    int max_hits = cfg->max_hits > 0 ? cfg->max_hits : 50;
    int room = max_hits - (int)res->hit_count;
    if (room < 0) room = 0;

    char *body = mi_download_fetch_text(&dcfg, url, CRAWL_MAX_BODY);
    res->fetched++;

    int links = 0, hits = 0, rendered = 0;
    if (body) {
        size_t blen = strlen(body);

        /* First: what does this page reference? */
        mi_browse_result br;
        if (mi_browse_parse(body, blen, url, MI_BROWSE_ALL, &br) == MI_OK) {
            links = (int)br.count;
            for (size_t i = 0; i < br.count; i++) {
                const mi_browse_item *item = &br.items[i];
                if (!item->url) continue;
                if (is_asset(item->url)) continue;
                int score = mi_crawl_score(item->filename, item->title, term);
                if (score > 0) {
                    if (hits >= room) break;
                    if (add_hit(res, item, score, url, 0)) hits++;
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
                        if (!same) { res->skipped_offsite++; continue; }
                    }
                    if (strset_has(visited, u)) continue;
                    frontier_push(f, u, depth + 1);
                }
            }
            mi_browse_result_free(&br);
        }
        free(body);
    } else {
        res->failed++;
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
    if (cfg->extract_enabled && now_seconds() < deadline) {
        mi_extract_config ecfg;
        mi_extract_config_init(&ecfg);
        ecfg.binary = cfg->extractor_path;
        if (mi_extract_available(&ecfg)) {
            mi_extract_result er;
            if (mi_extract(&ecfg, url, &er) == MI_OK && er.format_count > 0) {
                /*
                 * Scored on the page's title, because that is what the caller
                 * searched for — the format list is a technical detail.
                 */
                int score = mi_crawl_score(er.title, er.title, term);
                if (score > 0) {
                    mi_browse_item item;
                    memset(&item, 0, sizeof item);
                    item.url = (char *)url;
                    item.filename = er.title ? er.title : (char *)url;
                    item.title = er.title;
                    item.mime = (char *)"";
                    item.bytes = -1;
                    if (add_hit(res, &item, score, url, 0)) {
                        hits++;
                        /* Marked so it ranks above the page's own links at an
                         * equal score — a channel link matches a search just as
                         * well as the video does. */
                        res->hits[res->hit_count - 1].resolved = 1;
                    }
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
    if (hits == 0 && cfg->use_browser_fallback && now_seconds() < deadline &&
        res->rendered < (cfg->max_browser_pages > 0 ? cfg->max_browser_pages : 3)) {
        if (cfg->interactive_pool && mi_interactive_pool_available(cfg->interactive_pool)) {
            int was_rendered = 0;
            hits += read_page_driven(res, cfg, f, visited, url, depth, term, deadline,
                                     &was_rendered);
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
        if (cfg->render_pool && mi_render_pool_available(cfg->render_pool)) {
            mi_render_result rr;
            memset(&rr, 0, sizeof rr);
            if (mi_render_run(cfg->render_pool, url, MI_RENDER_DOM, &rr) == MI_RENDER_OK &&
                rr.data && rr.len > 0) {
                rendered = 1;
                res->rendered++;
                mi_browse_result br;
                if (mi_browse_parse((const char *)rr.data, rr.len, url, MI_BROWSE_ALL, &br) == MI_OK) {
                    if ((int)br.count > links) links = (int)br.count;
                    for (size_t i = 0; i < br.count; i++) {
                        const mi_browse_item *item = &br.items[i];
                        if (!item->url || is_asset(item->url)) continue;
                        int score = mi_crawl_score(item->filename, item->title, term);
                        if (score > 0) {
                            if (hits >= room) break;
                            if (add_hit(res, item, score, url, 1)) hits++;
                        }
                    }
                    if (depth < cfg->max_depth) {
                        for (size_t i = 0; i < br.count; i++) {
                            const char *u = br.items[i].url;
                            if (!u || !is_page(u)) continue;
                            if (strset_has(visited, u)) continue;
                            frontier_push(f, u, depth + 1);
                        }
                    }
                    mi_browse_result_free(&br);
                }
            }
            mi_render_result_free(&rr);
        }
    }

    (void)started;
    add_page(res, url, links, hits, rendered, body ? 200 : 0);
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

    int max_pages = cfg->max_pages > 0 ? cfg->max_pages : 40;
    if (max_pages > CRAWL_HARD_MAX_PAGES) max_pages = CRAWL_HARD_MAX_PAGES;
    int max_hits = cfg->max_hits > 0 ? cfg->max_hits : 50;
    double budget = cfg->budget_seconds > 0 ? cfg->budget_seconds : 45.0;
    double started = now_seconds();
    double deadline = started + budget;

    frontier f = {0};
    strset visited = {0};
    frontier_push(&f, site_url, 0);
    strset_add(&visited, site_url);

    int pages_read = 0;
    while (!frontier_empty(&f)) {
        if (pages_read >= max_pages) break;
        if ((int)out->hit_count >= max_hits) break;
        if (now_seconds() >= deadline) break;

        /*
         * The frontier owns the URL, so it is *not* freed here — `frontier_free`
         * frees every entry it still holds, and freeing one here as well is a
         * double free. The entry stays in the array; only `head` moves.
         */
        frontier_entry e = frontier_take(&f);
        read_page(out, cfg, &f, &visited, e.url, e.depth, term, deadline, started);
        pages_read++;

        /* Mark everything now queued as visited, so two pages linking the same
         * third do not both enqueue it. */
        for (size_t i = f.head; i < f.count; i++) strset_add(&visited, f.items[i].url);
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

    frontier_free(&f);
    strset_free(&visited);
    return MI_OK;
}
