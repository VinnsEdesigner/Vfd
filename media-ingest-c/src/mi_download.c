#define _GNU_SOURCE
#include "mi_download.h"

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mi_util.h"

#define MI_HEADER_SNIFF 8192

/* ------------------------------------------------------------------ */
/* libcurl plumbing                                                    */
/* ------------------------------------------------------------------ */

static int g_curl_ready = 0;
static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;
static int g_curl_init_rc = -1;

static void curl_global_setup(void) {
    g_curl_init_rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    g_curl_ready = (g_curl_init_rc == 0);
}

static int ensure_curl(void) {
    pthread_once(&g_curl_once, curl_global_setup);
    return g_curl_ready ? 0 : -1;
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} membuf;

static size_t write_membuf(char *ptr, size_t size, size_t nmemb, void *userdata) {
    membuf *b = userdata;
    size_t n = size * nmemb;
    if (b->len + n + 1 > b->cap) {
        size_t want = (b->len + n + 1) * 2;
        char *grown = mi_realloc(b->data, want);
        if (!grown) return 0;
        b->data = grown;
        b->cap = want;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

typedef struct {
    FILE *fp;
    mi_progress_fn on_progress;
    void *user;
    mi_progress *progress;
    pthread_mutex_t *lock;
    int64_t *shared_bytes;
    /* Set when the server asks us to back off (429 / Retry-After). */
    long retry_after_seconds;
    /* Non-NULL when the caller wants to be able to stop the transfer. */
    const mi_download_control *control;
} file_writer;

/* Read Retry-After (seconds form) off a response header block. */
static size_t header_retry_after(char *ptr, size_t size, size_t nmemb, void *userdata) {
    file_writer *w = userdata;
    size_t n = size * nmemb;
    if (n > 12 && !strncasecmp(ptr, "Retry-After:", 12)) {
        long v = strtol(ptr + 12, NULL, 10);
        if (v > 0 && v < 3600) w->retry_after_seconds = v;
    }
    return n;
}

static size_t write_file(char *ptr, size_t size, size_t nmemb, void *userdata) {
    file_writer *w = userdata;
    size_t n = size * nmemb;
    /* Checked before every chunk so a pause takes effect promptly, and the
     * partial file is left intact for the resume. */
    if (w->control && w->control->stop && w->control->stop(w->control->user)) {
        return 0;
    }
    size_t wrote = fwrite(ptr, 1, n, w->fp);
    if (wrote != n) return 0;
    if (w->shared_bytes && w->lock) {
        pthread_mutex_lock(w->lock);
        *w->shared_bytes += (int64_t)n;
        w->progress->downloaded_bytes = *w->shared_bytes;
        if (w->on_progress) w->on_progress(w->progress, w->user);
        pthread_mutex_unlock(w->lock);
    }
    return n;
}

/* The User-Agent sent when the caller has not named one. */
#define MI_DEFAULT_USER_AGENT "media-ingest-c/" MI_VYZOR_VERSION

static struct curl_slist *build_headers(const mi_download_config *cfg,
                                        const char *range) {
    struct curl_slist *list = NULL;
    list = curl_slist_append(list, "Accept: */*");
    /* The caller's agent when it named one, the built-in otherwise. A caller
     * header list can still override this, because it is appended after. */
    char *ua = mi_asprintf("User-Agent: %s",
                           (cfg->user_agent && *cfg->user_agent) ? cfg->user_agent
                                                                 : MI_DEFAULT_USER_AGENT);
    if (ua) {
        list = curl_slist_append(list, ua);
        free(ua);
    }
    for (size_t i = 0; i < cfg->header_count; i++) {
        list = curl_slist_append(list, cfg->headers[i]);
    }
    if (range) {
        char *h = mi_asprintf("Range: bytes=%s", range);
        if (h) {
            list = curl_slist_append(list, h);
            free(h);
        }
    }
    return list;
}

/* ------------------------------------------------------------------ */
/* Probing                                                             */
/* ------------------------------------------------------------------ */

static void plan_init(mi_download_plan *plan, const char *url) {
    memset(plan, 0, sizeof *plan);
    plan->url = mi_strdup(url);
    plan->total_bytes = -1;
    plan->accept_ranges = 0;
}

void mi_download_plan_free(mi_download_plan *plan) {
    if (!plan) return;
    free(plan->url);
    free(plan->content_type);
    free(plan->final_url);
    if (plan->have_media) mi_media_info_free(&plan->media);
    memset(plan, 0, sizeof *plan);
}

/* Fetch the first N bytes to sniff the payload. */
static int sniff_head(const mi_download_config *cfg, const char *url,
                      unsigned char *out, size_t n, size_t *got) {
    CURL *curl = curl_easy_init();
    if (!curl) return MI_ERR;
    membuf b = {0};
    char range[64];
    snprintf(range, sizeof range, "0-%zu", n - 1);
    struct curl_slist *headers = build_headers(cfg, range);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_membuf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, cfg->timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(curl);
    int ret = MI_ERR_NET;
    if (rc == CURLE_OK && b.len > 0) {
        size_t take = b.len < n ? b.len : n;
        memcpy(out, b.data, take);
        *got = take;
        ret = MI_OK;
    }
    free(b.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ret;
}

int mi_probe(const mi_download_config *cfg, const char *url,
             mi_download_plan *out) {
    if (ensure_curl() != 0) return MI_ERR;
    plan_init(out, url);

    CURL *curl = curl_easy_init();
    if (!curl) return MI_ERR;
    struct curl_slist *headers = build_headers(cfg, NULL);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L); /* HEAD */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, cfg->timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        curl_off_t clen = -1;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &clen);
        out->total_bytes = (int64_t)clen;
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        if (code == 405 || code == 501 || code == 403) out->total_bytes = -1;
        char *ct = NULL, *eff = NULL;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
        if (ct) out->content_type = mi_strdup(ct);
        if (eff) out->final_url = mi_strdup(eff);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    /* Detect range support + size via a ranged GET. */
    unsigned char head[MI_HEADER_SNIFF];
    size_t got = 0;
    if (sniff_head(cfg, url, head, sizeof head, &got) == MI_OK) {
        mi_detect(head, got, &out->media);
        out->have_media = 1;
        out->accept_ranges = 1; /* server honoured a range request */
    }
    if (!out->final_url) out->final_url = mi_strdup(url);
    return MI_OK;
}

/* ------------------------------------------------------------------ */
/* Ranged parallel download                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    int index;
    int64_t start;
    int64_t end; /* inclusive */
    char *part_path;
    int done;
} mi_range;

typedef struct {
    const mi_download_config *cfg;
    const char *url;
    mi_range *rng;
    pthread_mutex_t *lock;
    int64_t *shared_bytes;
    mi_progress *progress;
    mi_progress_fn on_progress;
    void *user;
    int rc;
    char *err;
} range_job;

static int fetch_range_once(const mi_download_config *cfg, const char *url,
                            const mi_range *rng, file_writer *w) {
    CURL *curl = curl_easy_init();
    if (!curl) return MI_ERR;
    char range[64];
    snprintf(range, sizeof range, "%lld-%lld", (long long)rng->start,
             (long long)rng->end);
    struct curl_slist *headers = build_headers(cfg, range);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, w);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_retry_after);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, w);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, cfg->timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    /* A write error while the caller has asked to stop is a pause, not a
     * failure: it must not be retried and must not be reported as an error. */
    if (cfg->control && cfg->control->stop && cfg->control->stop(cfg->control->user)) {
        return MI_ERR_ABORTED;
    }
    if (rc != CURLE_OK) return MI_ERR_NET;
    if (code != 200 && code != 206) return MI_ERR_NET;
    return MI_OK;
}

/* Honour Retry-After when the server rate-limits us, so the next attempt waits
 * as long as we were asked to rather than hammering a short backoff. */
static double retry_delay_for(const mi_download_config *cfg, int attempt,
                              long retry_after) {
    if (retry_after > 0) return (double)retry_after;
    return cfg->backoff * (1 << attempt);
}

static void *range_worker(void *arg) {
    range_job *job = arg;
    int64_t expected = job->rng->end - job->rng->start + 1;
    for (int attempt = 0; attempt <= job->cfg->retries; attempt++) {
        FILE *fp = fopen(job->rng->part_path, "wb");
        if (!fp) {
            job->rc = MI_ERR_IO;
            job->err = mi_asprintf("cannot open %s", job->rng->part_path);
            return NULL;
        }
        file_writer w = {.fp = fp,
                         .on_progress = job->on_progress,
                         .user = job->user,
                         .progress = job->progress,
                         .lock = job->lock,
                         .shared_bytes = job->shared_bytes,
                         .retry_after_seconds = 0,
                         .control = job->cfg->control};
        int rc = fetch_range_once(job->cfg, job->url, job->rng, &w);
        fclose(fp);
        if (rc == MI_ERR_ABORTED) {
            job->rc = MI_ERR_ABORTED;
            return NULL;
        }
        if (rc == MI_OK) {
            int64_t actual = mi_file_size(job->rng->part_path);
            if (actual == expected) {
                job->rc = MI_OK;
                return NULL;
            }
            rc = MI_ERR_NET; /* short read -> retry */
        }
        if (attempt < job->cfg->retries) {
            double sleep_s = retry_delay_for(job->cfg, attempt, w.retry_after_seconds);
            usleep((useconds_t)(sleep_s * 1e6));
        }
    }
    job->rc = MI_ERR_NET;
    job->err = mi_asprintf("range %d failed after %d attempts", job->rng->index,
                           job->cfg->retries + 1);
    return NULL;
}

static mi_range *plan_ranges(int64_t total, int connections, int64_t chunk_size,
                             const char *work_dir, int *count) {
    int64_t by_chunk = (total + chunk_size - 1) / chunk_size;
    int n = (int)(by_chunk < connections ? by_chunk : connections);
    if (n < 1) n = 1;
    int64_t size = (total + n - 1) / n;
    mi_range *ranges = mi_calloc((size_t)n, sizeof(mi_range));
    if (!ranges) return NULL;
    int64_t start = 0;
    int i = 0;
    while (start < total && i < n) {
        int64_t end = start + size - 1;
        if (end > total - 1) end = total - 1;
        ranges[i].index = i;
        ranges[i].start = start;
        ranges[i].end = end;
        char *name = mi_asprintf("part-%05d", i);
        ranges[i].part_path = mi_path_join(work_dir, name);
        free(name);
        start = end + 1;
        i++;
    }
    *count = i;
    return ranges;
}

/* --- resumable state file (simple line-oriented format) --- */

static void state_path_for(const char *work_dir, char **out) {
    *out = mi_path_join(work_dir, "state.txt");
}

static void load_state(const char *path, int64_t total, mi_range *ranges,
                       int count) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    int64_t file_total = -1;
    if (fgets(line, sizeof line, f)) sscanf(line, "total=%lld", (long long *)&file_total);
    if (file_total != total) {
        fclose(f);
        return; /* different object: start over */
    }
    while (fgets(line, sizeof line, f)) {
        int idx = -1;
        if (sscanf(line, "done=%d", &idx) == 1 && idx >= 0 && idx < count) {
            ranges[idx].done = mi_path_exists(ranges[idx].part_path);
        }
    }
    fclose(f);
}

static void save_state(const char *path, int64_t total, const mi_range *ranges,
                       int count) {
    char *tmp = mi_asprintf("%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        free(tmp);
        return;
    }
    fprintf(f, "total=%lld\n", (long long)total);
    for (int i = 0; i < count; i++) {
        if (ranges[i].done) fprintf(f, "done=%d\n", ranges[i].index);
    }
    fclose(f);
    rename(tmp, path);
    free(tmp);
}

static int assemble(const char *dest, mi_range *ranges, int count) {
    char *tmp = mi_asprintf("%s.assembling", dest);
    FILE *out = fopen(tmp, "wb");
    if (!out) {
        free(tmp);
        return MI_ERR_IO;
    }
    unsigned char buf[1 << 20];
    for (int i = 0; i < count; i++) {
        FILE *part = fopen(ranges[i].part_path, "rb");
        if (!part) {
            fclose(out);
            free(tmp);
            return MI_ERR_IO;
        }
        size_t got;
        while ((got = fread(buf, 1, sizeof buf, part)) > 0) {
            if (fwrite(buf, 1, got, out) != got) {
                fclose(part);
                fclose(out);
                free(tmp);
                return MI_ERR_IO;
            }
        }
        fclose(part);
    }
    fclose(out);
    int rc = rename(tmp, dest) == 0 ? MI_OK : MI_ERR_IO;
    free(tmp);
    return rc;
}

static int download_ranged(const mi_download_config *cfg, const char *url,
                           int64_t total, const char *dest,
                           mi_progress_fn on_progress, void *user,
                           mi_download_result *out, mi_media_info *media) {
    char *work_dir = mi_asprintf("%s.parts", dest);
    if (!work_dir || mi_mkdir_p(work_dir) != 0) {
        free(work_dir);
        return MI_ERR_IO;
    }
    int count = 0;
    mi_range *ranges = plan_ranges(total, cfg->connections, cfg->chunk_size,
                                   work_dir, &count);
    if (!ranges) {
        free(work_dir);
        return MI_ERR_NOMEM;
    }
    char *state_path = NULL;
    state_path_for(work_dir, &state_path);
    load_state(state_path, total, ranges, count);

    int resumed = 0;
    int64_t base = 0;
    for (int i = 0; i < count; i++) {
        if (ranges[i].done) {
            resumed = 1;
            base += ranges[i].end - ranges[i].start + 1;
        }
    }

    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    int64_t shared = base;
    mi_progress progress = {.downloaded_bytes = base,
                            .total_bytes = total,
                            .started_at = mi_now(),
                            .finished = 0};
    if (on_progress) on_progress(&progress, user);

    int pending = 0;
    for (int i = 0; i < count; i++) if (!ranges[i].done) pending++;

    range_job *jobs = mi_calloc((size_t)(pending ? pending : 1), sizeof(range_job));
    pthread_t *threads = mi_calloc((size_t)(pending ? pending : 1), sizeof(pthread_t));
    if (!jobs || !threads) {
        free(jobs); free(threads); free(ranges); free(state_path); free(work_dir);
        return MI_ERR_NOMEM;
    }

    int j = 0;
    for (int i = 0; i < count; i++) {
        if (ranges[i].done) continue;
        jobs[j] = (range_job){.cfg = cfg,
                              .url = url,
                              .rng = &ranges[i],
                              .lock = &lock,
                              .shared_bytes = &shared,
                              .progress = &progress,
                              .on_progress = on_progress,
                              .user = user,
                              .rc = MI_OK,
                              .err = NULL};
        pthread_create(&threads[j], NULL, range_worker, &jobs[j]);
        j++;
    }

    int rc = MI_OK;
    int aborted = 0;
    for (j = 0; j < pending; j++) {
        pthread_join(threads[j], NULL);
        if (jobs[j].rc == MI_ERR_ABORTED) {
            aborted = 1;
            continue;
        }
        if (jobs[j].rc != MI_OK && rc == MI_OK) {
            rc = jobs[j].rc;
            mi_logf(MI_LOG_ERROR, "%s", jobs[j].err ? jobs[j].err : "range failed");
        }
        free(jobs[j].err);
    }
    if (aborted) rc = MI_ERR_ABORTED;

    if (rc == MI_OK) {
        for (int i = 0; i < count; i++) ranges[i].done = 1;
        save_state(state_path, total, ranges, count);
        rc = assemble(dest, ranges, count);
        if (rc == MI_OK) mi_rm_rf(work_dir);
    } else {
        /* Persist whatever finished so the next run resumes. */
        for (int i = 0; i < count; i++) {
            if (!ranges[i].done) {
                int64_t want = ranges[i].end - ranges[i].start + 1;
                ranges[i].done = (mi_file_size(ranges[i].part_path) == want);
            }
        }
        save_state(state_path, total, ranges, count);
    }

    progress.downloaded_bytes = rc == MI_OK ? total : shared;
    progress.total_bytes = total;
    progress.finished = (rc == MI_OK);
    if (on_progress) on_progress(&progress, user);
    if (rc == MI_ERR_ABORTED) {
        /* Leave the parts and the state file exactly as they are: the next
         * call resumes from here. */
        free(jobs);
        free(threads);
        for (int i = 0; i < count; i++) free(ranges[i].part_path);
        free(ranges);
        free(state_path);
        free(work_dir);
        return MI_ERR_ABORTED;
    }

    if (rc == MI_OK) {
        out->bytes_written = mi_file_size(dest);
        out->resumed = resumed;
        out->elapsed_seconds = mi_now() - progress.started_at;
        /* Detect from the file we actually wrote. A sniff of the first bytes
         * over the network can miss a track header that sits further in, so the
         * local file is the authoritative source for things like frame size. */
        mi_detect_file(dest, MI_HEADER_SNIFF, &out->media);
        if (out->media.kind == MI_KIND_UNKNOWN && media && media->kind != MI_KIND_UNKNOWN) {
            mi_media_info_free(&out->media);
            memset(&out->media, 0, sizeof out->media);
            mi_media_info_copy(&out->media, media);
        }
    }

    free(jobs);
    free(threads);
    for (int i = 0; i < count; i++) free(ranges[i].part_path);
    free(ranges);
    free(state_path);
    free(work_dir);
    return rc;
}

static int download_stream(const mi_download_config *cfg, const char *url,
                           const char *dest, mi_progress_fn on_progress,
                           void *user, mi_download_result *out,
                           mi_media_info *media) {
    CURL *curl = curl_easy_init();
    if (!curl) return MI_ERR;
    FILE *fp = fopen(dest, "wb");
    if (!fp) {
        curl_easy_cleanup(curl);
        return MI_ERR_IO;
    }
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    int64_t shared = 0;
    mi_progress progress = {.downloaded_bytes = 0,
                            .total_bytes = -1,
                            .started_at = mi_now(),
                            .finished = 0};
    file_writer w = {.fp = fp,
                     .on_progress = on_progress,
                     .user = user,
                     .progress = &progress,
                     .lock = &lock,
                     .shared_bytes = &shared,
                     .control = cfg->control};
    struct curl_slist *headers = build_headers(cfg, NULL);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &w);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, cfg->timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(curl);
    fclose(fp);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (cfg->control && cfg->control->stop && cfg->control->stop(cfg->control->user)) {
        /* A streamed download cannot resume; the partial file is discarded so
         * the next attempt starts clean rather than keeping a truncated file. */
        unlink(dest);
        return MI_ERR_ABORTED;
    }
    if (rc != CURLE_OK) return MI_ERR_NET;

    progress.downloaded_bytes = shared;
    progress.total_bytes = shared;
    progress.finished = 1;
    if (on_progress) on_progress(&progress, user);

    out->bytes_written = mi_file_size(dest);
    out->resumed = 0;
    out->elapsed_seconds = mi_now() - progress.started_at;
    mi_detect_file(dest, MI_HEADER_SNIFF, &out->media);
    if (out->media.kind == MI_KIND_UNKNOWN && media && media->kind != MI_KIND_UNKNOWN) {
        mi_media_info_free(&out->media);
        memset(&out->media, 0, sizeof out->media);
        mi_media_info_copy(&out->media, media);
    }
    return MI_OK;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */

void mi_fetch_info_free(mi_fetch_info *info) {
    if (!info) return;
    free(info->final_url);
    free(info->content_type);
    free(info->frame_blocker);
    memset(info, 0, sizeof *info);
}

/* Gathers the response headers a page view needs. */
typedef struct {
    mi_fetch_info *info;
    char *csp; /* Content-Security-Policy, when the response carries one */
} doc_header_ctx;

static size_t header_document(char *ptr, size_t size, size_t nmemb, void *userdata) {
    doc_header_ctx *ctx = userdata;
    size_t n = size * nmemb;
    if (!ctx || !ctx->info) return n;

    /* X-Frame-Options: DENY or SAMEORIGIN both forbid another origin framing
     * this page. A value that merely whitelists some other origin also blocks
     * us, so anything other than an absent header counts as "not framable"
     * unless it explicitly allows everything. */
    if (n > 17 && !strncasecmp(ptr, "X-Frame-Options:", 16)) {
        const char *v = ptr + 16;
        while (*v == ' ' || *v == '\t') v++;
        if (!strncasecmp(v, "DENY", 4) || !strncasecmp(v, "SAMEORIGIN", 10) ||
            !strncasecmp(v, "ALLOW-FROM", 10)) {
            ctx->info->framable = 0;
            if (!ctx->info->frame_blocker) {
                ctx->info->frame_blocker = mi_strndup("X-Frame-Options",
                                                      strlen("X-Frame-Options"));
            }
        }
    }

    /* A CSP frame-ancestors directive is the modern equivalent. Kept so the
     * policy can be scanned once the header block ends. */
    if (n > 25 && !strncasecmp(ptr, "Content-Security-Policy:", 24)) {
        const char *v = ptr + 24;
        while (*v == ' ' || *v == '\t') v++;
        size_t len = n - (size_t)(v - ptr);
        while (len > 0 && (v[len - 1] == '\r' || v[len - 1] == '\n')) len--;
        free(ctx->csp);
        ctx->csp = mi_strndup(v, len);
    }

    if (n > 13 && !strncasecmp(ptr, "Content-Type:", 13)) {
        const char *v = ptr + 13;
        while (*v == ' ' || *v == '\t') v++;
        size_t len = n - (size_t)(v - ptr);
        while (len > 0 && (v[len - 1] == '\r' || v[len - 1] == '\n')) len--;
        free(ctx->info->content_type);
        ctx->info->content_type = mi_strndup(v, len);
    }

    return n;
}

/* A CSP that names no frame-ancestors does not restrict framing; one that does
 * and does not allow this app's origin blocks it. Only "none" and "self" are
 * treated as a refusal, because a whitelist naming some other host would also
 * block us and there is no way to know that host list is ours. */
static int csp_forbids_framing(const char *csp) {
    if (!csp) return 0;
    const char *p = csp;
    while ((p = strcasestr(p, "frame-ancestors")) != NULL) {
        /* Ensure it is the directive, not part of a longer word. */
        if (p > csp && (isalnum((unsigned char)p[-1]) || p[-1] == '-')) {
            p += 15;
            continue;
        }
        const char *v = p + 15;
        while (*v == ' ' || *v == '\t') v++;
        if (!strncasecmp(v, "'none'", 6) || !strncasecmp(v, "none", 4) ||
            !strncasecmp(v, "'self'", 6)) {
            return 1;
        }
        return 0;
    }
    return 0;
}

char *mi_download_fetch_document(const mi_download_config *cfg, const char *url,
                                 size_t max_bytes, mi_fetch_info *info) {
    if (info) memset(info, 0, sizeof *info);
    if (ensure_curl() != 0 || !url) return NULL;
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    doc_header_ctx hctx = {.info = info, .csp = NULL};
    if (info) info->framable = 1;

    membuf b = {0};
    struct curl_slist *headers = build_headers(cfg, NULL);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_membuf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    if (info) {
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_document);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hctx);
    }
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, cfg->timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    /* Ask for identity: the bytes are parsed and shown, so they must be what
     * the site actually sent. */
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (info) {
        char *final_url = NULL;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &final_url);
        if (final_url && strcmp(final_url, url) != 0) {
            info->final_url = mi_strdup(final_url);
        }
        if (csp_forbids_framing(hctx.csp)) {
            info->framable = 0;
            if (!info->frame_blocker) {
                info->frame_blocker = mi_strdup("Content-Security-Policy");
            }
        }
    }
    free(hctx.csp);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK || code < 200 || code >= 300 || !b.data) {
        free(b.data);
        return NULL;
    }
    if (b.len > max_bytes) b.data[max_bytes] = '\0';
    return b.data;
}

char *mi_download_fetch_text(const mi_download_config *cfg, const char *url,
                             size_t max_bytes) {
    return mi_download_fetch_document(cfg, url, max_bytes, NULL);
}

void mi_download_config_init(mi_download_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->connections = 8;
    cfg->chunk_size = 4 * 1024 * 1024;
    cfg->retries = 4;
    cfg->backoff = 0.5;
    cfg->timeout_seconds = 30;
    cfg->require_media = 1;
    cfg->max_bytes = 0;
    cfg->headers = NULL;
    cfg->header_count = 0;
    cfg->user_agent = NULL;
    cfg->control = NULL;
}

void mi_download_result_free(mi_download_result *res) {
    if (!res) return;
    free(res->url);
    free(res->path);
    mi_media_info_free(&res->media);
    memset(res, 0, sizeof *res);
}

int mi_download(const mi_download_config *cfg, const char *url, const char *dest,
                mi_progress_fn on_progress, void *user,
                mi_download_result *out) {
    if (ensure_curl() != 0) return MI_ERR;
    memset(out, 0, sizeof *out);
    out->url = mi_strdup(url);
    out->path = mi_strdup(dest);

    /* Already-complete file: honour it unless it is not video and we require it. */
    if (mi_path_exists(dest)) {
        mi_media_info existing;
        mi_detect_file(dest, MI_HEADER_SNIFF, &existing);
        if (existing.is_video || existing.is_image || existing.is_audio ||
            !cfg->require_media) {
            out->bytes_written = mi_file_size(dest);
            out->resumed = 1;
            out->media = existing;
            return MI_OK;
        }
        mi_media_info_free(&existing);
    }

    mi_download_plan plan;
    int rc = mi_probe(cfg, url, &plan);
    if (rc != MI_OK) {
        mi_download_result_free(out);
        return rc;
    }

    int recognised = plan.media.is_video || plan.media.is_image || plan.media.is_audio;
    if (plan.have_media && cfg->require_media && !recognised) {
        mi_logf(MI_LOG_ERROR, "refusing unrecognised payload (detected %s)",
                mi_kind_name(plan.media.kind));
        mi_download_plan_free(&plan);
        mi_download_result_free(out);
        return MI_ERR_ARG;
    }
    if (cfg->max_bytes > 0 && plan.total_bytes > cfg->max_bytes) {
        mi_logf(MI_LOG_ERROR, "object is %lld bytes, over max_bytes=%lld",
                (long long)plan.total_bytes, (long long)cfg->max_bytes);
        mi_download_plan_free(&plan);
        mi_download_result_free(out);
        return MI_ERR_ARG;
    }

    if (plan.accept_ranges && plan.total_bytes > 0) {
        rc = download_ranged(cfg, plan.final_url ? plan.final_url : url,
                             plan.total_bytes, dest, on_progress, user, out,
                             plan.have_media ? &plan.media : NULL);
    } else {
        rc = download_stream(cfg, plan.final_url ? plan.final_url : url, dest,
                             on_progress, user, out,
                             plan.have_media ? &plan.media : NULL);
    }
    mi_download_plan_free(&plan);
    if (rc != MI_OK) mi_download_result_free(out);
    return rc;
}
