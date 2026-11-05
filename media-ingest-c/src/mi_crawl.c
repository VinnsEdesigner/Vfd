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
#include "mi_json.h"
#include "mi_render.h"
#include "mi_util.h"

/* A page's HTML, capped. A listing page is small; anything past this is not a
 * listing and reading it all would only cost memory. */
#define CRAWL_MAX_BODY (2 * 1024 * 1024)
/* The most pages one walk may read, whatever the config says, so a mistake in a
 * caller cannot turn into an unbounded crawl. */
#define CRAWL_HARD_MAX_PAGES 200

void mi_crawl_config_init(mi_crawl_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->max_depth = 2;
    cfg->max_pages = 40;
    cfg->max_hits = 50;
    cfg->budget_seconds = 45.0;
    cfg->same_host_only = 0;
    cfg->use_browser_fallback = 1;
    cfg->user_agent = NULL;
    cfg->render_pool = NULL;
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

static mi_crawl_hit *add_hit(mi_crawl_result *res, const mi_browse_item *item,
                             int score, const char *found_on, int via_browser) {
    mi_crawl_hit *grown = mi_realloc(res->hits, (res->hit_count + 1) * sizeof *grown);
    if (!grown) return NULL;
    res->hits = grown;
    mi_crawl_hit *h = &res->hits[res->hit_count++];
    memset(h, 0, sizeof *h);
    h->url = mi_strdup(item->url);
    h->filename = mi_strdup(item->filename);
    h->title = item->title ? mi_strdup(item->title) : NULL;
    h->mime = mi_strdup(item->mime ? item->mime : "");
    h->score = score;
    h->found_on = mi_strdup(found_on);
    h->via_browser = via_browser;
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

/** Is this URL a page to follow, rather than something to hand to the caller? */
static int looks_like_page(const char *url) {
    const char *q = strchr(url, '?');
    size_t n = q ? (size_t)(q - url) : strlen(url);
    /* A path with no extension is a page. So is one ending in a known page
     * extension. Anything else is a file the caller may want. */
    const char *slash = NULL;
    for (size_t i = 0; i < n; i++) {
        if (url[i] == '/') slash = url + i;
    }
    const char *name = slash ? slash + 1 : url;
    const char *dot = strrchr(name, '.');
    if (!dot || dot >= url + n) return 1;
    static const char *PAGE_EXT[] = {".html", ".htm", ".php", ".asp", ".aspx", ".jsp",
                                     ".shtml", ".cgi", ".pl", NULL};
    for (size_t i = 0; PAGE_EXT[i]; i++) {
        if (!strncasecmp(dot, PAGE_EXT[i], strlen(PAGE_EXT[i]))) return 1;
    }
    return 0;
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
                if (looks_like_page(item->url)) continue;
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
                    if (!u || !looks_like_page(u)) continue;
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
     * Only when the static pass found nothing matching on this page, and only
     * when the pool can take it. Rendering every page would be far more
     * expensive than the crawl is worth; rendering the pages that look empty is
     * what recovers a JavaScript-built site.
     */
    if (hits == 0 && cfg->use_browser_fallback && cfg->render_pool &&
        mi_render_pool_available(cfg->render_pool) && now_seconds() < deadline) {
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
                    if (!item->url || looks_like_page(item->url)) continue;
                    int score = mi_crawl_score(item->filename, item->title, term);
                    if (score > 0) {
                        if (hits >= room) break;
                        if (add_hit(res, item, score, url, 1)) hits++;
                    }
                }
                if (depth < cfg->max_depth) {
                    for (size_t i = 0; i < br.count; i++) {
                        const char *u = br.items[i].url;
                        if (!u || !looks_like_page(u)) continue;
                        if (strset_has(visited, u)) continue;
                        frontier_push(f, u, depth + 1);
                    }
                }
                mi_browse_result_free(&br);
            }
        }
        mi_render_result_free(&rr);
    }

    (void)started;
    add_page(res, url, links, hits, rendered, body ? 200 : 0);
    return hits;
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

    /* Best match first: the caller asked for a file by name, so the closest
     * name is the answer and the rest are alternatives. */
    for (size_t i = 0; i + 1 < out->hit_count; i++) {
        for (size_t j = i + 1; j < out->hit_count; j++) {
            if (out->hits[j].score > out->hits[i].score) {
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
