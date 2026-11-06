#define _GNU_SOURCE
#include "mi_linkcheck.h"
#include "mi_browse.h"
#include "mi_html.h"
#include "mi_json.h"
#include "mi_util.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

const char *mi_link_status_name(mi_link_status s) {
    switch (s) {
        case MI_LINK_OK: return "ok";
        case MI_LINK_REDIRECT: return "redirect";
        case MI_LINK_BROKEN: return "broken";
        case MI_LINK_UNREACHABLE: return "unreachable";
        case MI_LINK_SKIPPED: return "skipped";
    }
    return "unknown";
}

void mi_link_report_free(mi_link_report *r) {
    if (!r) return;
    for (size_t i = 0; i < r->count; i++) {
        free(r->results[i].url);
        free(r->results[i].found_on);
        free(r->results[i].final_url);
        free(r->results[i].detail);
        free(r->results[i].title);
    }
    free(r->results);
    memset(r, 0, sizeof *r);
}

/* Whether a URL is one to fetch: an http(s) reference. */
static int is_checkable(const char *u) {
    return u && (strncasecmp(u, "http://", 7) == 0 || strncasecmp(u, "https://", 8) == 0);
}


/* A work item: one link to check, and where to write the answer. */
typedef struct {
    const mi_download_config *cfg;
    const mi_linkcheck_options *opts;
    char *url;
    mi_link_result *out;
    pthread_mutex_t *next_lock;
    size_t *next;
    size_t count;
    char **urls; /* parallel array, for the direct-URL case */
} work;

static void check_one(work *w, size_t i) {
    mi_link_result *r = &w->out[i];
    const char *url = w->urls[i];
    r->url = mi_strdup(url);

    if (!is_checkable(url)) {
        r->status = MI_LINK_SKIPPED;
        r->detail = mi_strdup("not an http(s) reference");
        return;
    }

    mi_download_config c = *w->cfg;
    c.require_media = 0;
    if (w->opts && w->opts->timeout_seconds > 0) c.timeout_seconds = w->opts->timeout_seconds;
    /* A link check reads only the status, so it never wants the body. */
    c.max_bytes = 0;

    mi_fetch_info info;
    memset(&info, 0, sizeof info);
    /*
     * A HEAD would be cheaper, but many servers answer a HEAD differently from
     * a GET (or not at all), so a GET of the first byte is the honest check. The
     * fetch layer stops at max_bytes, so the body is not pulled.
     */
    char *body = mi_download_fetch_document(&c, url, 0, &info);
    r->http_status = info.status;
    if (info.final_url) r->final_url = mi_strdup(info.final_url);

    if (!body && info.status == 0) {
        r->status = MI_LINK_UNREACHABLE;
        r->detail = mi_strdup("no response");
    } else if (info.status >= 200 && info.status < 300) {
        r->status = MI_LINK_OK;
    } else if (info.status >= 300 && info.status < 400) {
        /* A redirect that the fetch did not follow is still a resolution; the
         * link is not dead. */
        r->status = MI_LINK_REDIRECT;
    } else if (info.status >= 400) {
        r->status = MI_LINK_BROKEN;
        r->detail = mi_asprintf("HTTP %d", info.status);
    } else {
        r->status = MI_LINK_UNREACHABLE;
        r->detail = mi_strdup("no usable response");
    }
    free(body);
    mi_fetch_info_free(&info);
}

static void *worker(void *arg) {
    work *w = arg;
    for (;;) {
        pthread_mutex_lock(w->next_lock);
        size_t i = (*w->next)++;
        pthread_mutex_unlock(w->next_lock);
        if (i >= w->count) break;
        check_one(w, i);
    }
    return NULL;
}

/* Run the checks with `concurrency` threads. */
static int run_checks(const mi_download_config *cfg, char **urls, size_t count,
                      const mi_linkcheck_options *opts, mi_link_report *out) {
    out->results = mi_calloc(count ? count : 1, sizeof *out->results);
    if (!out->results) return -1;
    out->count = count;
    for (size_t i = 0; i < count; i++) out->results[i].http_status = 0;

    int conc = opts && opts->concurrency > 0 ? opts->concurrency : 8;
    if (conc > (int)count) conc = (int)count;
    if (conc < 1) conc = 1;

    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    size_t next = 0;
    work w = {.cfg = cfg, .opts = opts, .out = out->results,
              .next_lock = &lock, .next = &next, .count = count, .urls = urls};
    (void)w.url;

    pthread_t *threads = mi_calloc((size_t)conc, sizeof *threads);
    for (int i = 0; i < conc; i++) pthread_create(&threads[i], NULL, worker, &w);
    for (int i = 0; i < conc; i++) pthread_join(threads[i], NULL);
    free(threads);

    for (size_t i = 0; i < count; i++) {
        switch (out->results[i].status) {
            case MI_LINK_OK: out->ok++; break;
            case MI_LINK_REDIRECT: out->redirect++; break;
            case MI_LINK_BROKEN: out->broken++; break;
            case MI_LINK_UNREACHABLE: out->unreachable++; break;
            case MI_LINK_SKIPPED: out->skipped++; break;
        }
    }
    return 0;
}

int mi_linkcheck_urls(const mi_download_config *cfg, const char *const *urls,
                      size_t count, const mi_linkcheck_options *opts,
                      mi_link_report *out) {
    memset(out, 0, sizeof *out);
    if (!cfg || !urls) return -1;
    /* Own copies, because the work items are handed to threads. */
    char **copy = mi_calloc(count ? count : 1, sizeof *copy);
    for (size_t i = 0; i < count; i++) copy[i] = mi_strdup(urls[i]);
    int rc = run_checks(cfg, copy, count, opts, out);
    for (size_t i = 0; i < count; i++) free(copy[i]);
    free(copy);
    return rc;
}

int mi_linkcheck_page(const mi_download_config *cfg, const char *page_url,
                      const mi_linkcheck_options *opts, mi_link_report *out) {
    memset(out, 0, sizeof *out);
    if (!cfg || !page_url) return -1;

    mi_download_config c = *cfg;
    c.require_media = 0;
    mi_fetch_info info;
    char *page = mi_download_fetch_document(&c, page_url, 4 * 1024 * 1024, &info);
    char *base = info.final_url ? mi_strdup(info.final_url) : mi_strdup(page_url);
    mi_fetch_info_free(&info);
    if (!page) { free(base); return -1; }

    /* Every reference on the page, media and non-media alike: a link check is
     * about links, not files. */
    mi_browse_result br;
    if (mi_browse_parse(page, strlen(page), base, MI_BROWSE_ALL, &br) != MI_OK) {
        free(page);
        free(base);
        return -1;
    }
    free(page);

    size_t cap = br.count ? br.count : 1;
    char **urls = mi_calloc(cap, sizeof *urls);
    size_t n = 0;
    int max_links = opts && opts->max_links > 0 ? opts->max_links : 500;
    for (size_t i = 0; i < br.count && (int)n < max_links; i++) {
        if (!br.items[i].url) continue;
        /* Deduplicate: the same link twice is one check. */
        int dup = 0;
        for (size_t j = 0; j < n; j++)
            if (!strcmp(urls[j], br.items[i].url)) { dup = 1; break; }
        if (dup) continue;
        urls[n++] = mi_strdup(br.items[i].url);
    }
    mi_browse_result_free(&br);
    free(base);

    int rc = run_checks(cfg, urls, n, opts, out);
    for (size_t i = 0; i < n; i++) free(urls[i]);
    free(urls);
    /* Record where each link was found. */
    for (size_t i = 0; i < out->count; i++) out->results[i].found_on = mi_strdup(page_url);
    return rc;
}

char *mi_link_report_to_json(const mi_link_report *r) {
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"summary\":{\"ok\":");
    mi_json_int(&b, (int64_t)r->ok);
    mi_json_raw(&b, ",\"redirect\":");
    mi_json_int(&b, (int64_t)r->redirect);
    mi_json_raw(&b, ",\"broken\":");
    mi_json_int(&b, (int64_t)r->broken);
    mi_json_raw(&b, ",\"unreachable\":");
    mi_json_int(&b, (int64_t)r->unreachable);
    mi_json_raw(&b, ",\"skipped\":");
    mi_json_int(&b, (int64_t)r->skipped);
    mi_json_raw(&b, "},\"links\":[");
    for (size_t i = 0; i < r->count; i++) {
        const mi_link_result *l = &r->results[i];
        if (i) mi_json_raw(&b, ",");
        mi_json_raw(&b, "{\"url\":");
        mi_json_string(&b, l->url);
        mi_json_raw(&b, ",\"status\":");
        mi_json_string(&b, mi_link_status_name(l->status));
        mi_json_raw(&b, ",\"http_status\":");
        mi_json_int(&b, l->http_status);
        if (l->final_url) {
            mi_json_raw(&b, ",\"final_url\":");
            mi_json_string(&b, l->final_url);
        }
        if (l->detail) {
            mi_json_raw(&b, ",\"detail\":");
            mi_json_string(&b, l->detail);
        }
        mi_json_raw(&b, "}");
    }
    mi_json_raw(&b, "]}");
    char *out = mi_strdup(mi_json_data(&b));
    mi_json_free(&b);
    return out;
}
