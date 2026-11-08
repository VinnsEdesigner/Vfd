#define _GNU_SOURCE
#include "mi_download.h"

#include <ctype.h>
#include <curl/curl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "mi_charset.h"
#include "mi_cookiejar.h"
#include "mi_browser_headers.h"
#include "mi_extract.h"
#include "mi_identity.h"
#include "mi_impersonate.h"
#include "mi_json.h"
#include "mi_browser_headers.h"
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

/*
 * Running an external tool with its output *relayed into Vyzor's log*.
 *
 * Every fetcher here shells out — ffmpeg for a manifest, yt-dlp for a page,
 * aria2 for a torrent — and every one of them used to have its output sent to
 * `/dev/null`. That made the log useless for exactly the case it is needed for:
 * a transfer that produces no bytes prints its reason on the tool's output, so
 * discarding it left "0 bytes for 100 seconds" with no explanation, and a stalled
 * transfer was indistinguishable from a slow one.
 *
 * **Both** streams are captured, on one pipe. That is not belt-and-braces: the
 * tools disagree about which stream they use. ffmpeg and yt-dlp write their
 * diagnostics to *stderr*, while **aria2 writes its notices and progress to
 * stdout** — measured: `aria2c ... 2>err` leaves `err` empty and puts "[NOTICE]
 * Downloading 1 item(s)", "[ERROR] Download aborted" and the progress summary on
 * stdout. Capturing only stderr relayed nothing at all from aria2, which is why
 * this takes both.
 *
 * A redrawn progress bar is the one thing worth avoiding, and it is avoided at
 * the source instead: aria2 is told `--summary-interval` so it prints a line
 * periodically rather than repainting, and ffmpeg is told `-loglevel error`.
 */
#define RELAY_PREFIX "tool: "

/** Spawn `argv`, with the child's stdout *and* stderr on `*out_fd`
 *  (non-blocking). Returns the pid, or -1. The caller drains and closes it. */
static pid_t spawn_relayed(const char *const *argv, int *out_fd) {
    int p[2];
    if (out_fd) *out_fd = -1;
    if (pipe(p) != 0) return -1;
    /* Non-blocking, so a drain never stalls the poll loop that reports progress
     * — a silent child is exactly the case the relay exists to notice. */
    int fl = fcntl(p[0], F_GETFL, 0);
    if (fl >= 0) fcntl(p[0], F_SETFL, fl | O_NONBLOCK);

    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(p[1], STDOUT_FILENO);
        dup2(p[1], STDERR_FILENO);
        close(p[0]);
        close(p[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(p[1]);
    if (out_fd) *out_fd = p[0];
    else close(p[0]);
    return pid;
}

/** Map a tool's own severity tag onto Vyzor's level. */
static mi_log_level relay_level_for(const char *line) {
    if (strstr(line, "[ERROR]") || strstr(line, "ERROR:") ||
        strstr(line, "Exception:") || strstr(line, "errorCode=")) {
        return MI_LOG_ERROR;
    }
    if (strstr(line, "[WARN") || strstr(line, "WARNING:")) return MI_LOG_WARN;
    return MI_LOG_INFO;
}

/** Emit one complete line, with the tool's severity preserved. */
static void relay_emit(const char *line) {
    if (!*line) return;
    /*
     * Keep the tool's own severity rather than flattening it.
     *
     * The tools tag their lines ("[ERROR] ...", "WARNING: ..."), and mapping
     * that onto Vyzor's levels is what lets the console's level filter work on
     * them: an aria2 "[ERROR] Download aborted" should be an error, not
     * something hidden among the notices.
     */
    mi_logf(relay_level_for(line), RELAY_PREFIX "%s", line);
}

/*
 * Drain whatever the child has said, one log line per message.
 *
 * The partial tail of a line is *carried over*, not emitted. A pipe delivers
 * bytes, not messages: a long line arrives in two reads, and logging each read
 * would split one message into two entries — the tail losing the "[ERROR]" tag
 * that made it an error. The carry buffer is per-call and `carry`/`carrylen`
 * belong to the caller, so successive drains reassemble the line. Anything still
 * buffered when the child exits is flushed by passing `flush = 1`.
 */
static size_t relay_drain(int fd, char *carry, size_t *carrylen, size_t carrycap,
                          int flush) {
    if (fd < 0) return 0;
    size_t total = 0;
    char chunk[512];
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof chunk - 1);
        if (n <= 0) break;
        total += (size_t)n;
        size_t off = 0;
        while (off < (size_t)n) {
            /* Take up to the next newline, or the rest of the chunk. */
            char *nl = memchr(chunk + off, '\n', (size_t)n - off);
            size_t take = nl ? (size_t)(nl - (chunk + off)) : (size_t)n - off;
            /* Append to the carry, bounded so a pathological line cannot grow
             * without limit; a line longer than the cap is logged truncated. */
            size_t room = carrycap - 1 - *carrylen;
            size_t put = take < room ? take : room;
            memcpy(carry + *carrylen, chunk + off, put);
            *carrylen += put;
            if (nl) {
                carry[*carrylen] = '\0';
                relay_emit(carry);
                *carrylen = 0;
            }
            off += take + (nl ? 1 : 0);
            if (!nl) break;
        }
    }
    if (flush && *carrylen > 0) {
        carry[*carrylen] = '\0';
        relay_emit(carry);
        *carrylen = 0;
    }
    return total;
}

/*
 * The simple form, for callers that do not care about line boundaries: drains
 * and emits each read as its own line. Used where the output is short and
 * already newline-terminated (a probe, a `--version`).
 */
static size_t relay_child_output(int fd) {
    char carry[MI_LOG_LINE_CAP];
    size_t carrylen = 0;
    return relay_drain(fd, carry, &carrylen, sizeof carry, 1);
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

/* The host of a URL, lowercased, without userinfo or port. */
static char *mi_download_host_of(const char *url) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *at = strchr(p, '@');
    const char *slash = strchr(p, '/');
    if (at && (!slash || at < slash)) p = at + 1;
    const char *end = p;
    while (*end && *end != '/' && *end != ':' && *end != '?' && *end != '#') end++;
    if (end == p) return NULL;
    char *h = mi_strndup(p, (size_t)(end - p));
    for (char *q = h; q && *q; q++) *q = (char)tolower((unsigned char)*q);
    return h;
}

/* The path of a URL, defaulting to "/". */
static char *mi_download_path_of(const char *url) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *slash = strchr(p, '/');
    if (!slash) return mi_strdup("/");
    const char *end = slash;
    while (*end && *end != '?' && *end != '#') end++;
    if (end == slash) return mi_strdup("/");
    return mi_strndup(slash, (size_t)(end - slash));
}

static struct curl_slist *build_headers(const mi_download_config *cfg,
                                        const char *url, const char *range) {
    struct curl_slist *list = NULL;

    /*
     * A browser, when one was named.
     *
     * A site that checks header *coherence* — a UA claiming Chrome with no
     * `sec-ch-ua`, or a client hint that disagrees with the UA — blocks the
     * request however good the TLS fingerprint is. When the caller named a
     * browser to imitate, the same coherent header set a real one sends is used,
     * derived from the same family and version so the two cannot drift apart.
     * The caller's own headers are appended after and win, so this is a default,
     * not an override.
     */
    if (cfg->impersonate && *cfg->impersonate) {
        char *bh[16];
        size_t bn = mi_browser_headers(mi_browser_family_of(cfg->impersonate), NULL,
                                       cfg->user_agent, bh, 16);
        for (size_t i = 0; i < bn; i++) list = curl_slist_append(list, bh[i]);
        mi_browser_headers_free(bh, bn);
    } else {
        list = curl_slist_append(list, "Accept: */*");
    }

    /* The caller's language preference, so a negotiating site serves it. */
    if (cfg->accept_language && *cfg->accept_language) {
        char *h = mi_asprintf("Accept-Language: %s", cfg->accept_language);
        if (h) { list = curl_slist_append(list, h); free(h); }
    }

    /*
     * The session cookies, from the live jar. This is what carries a login from
     * request to request: the login set the cookie, and every fetch after sends
     * it. Built per request because the jar changes.
     */
    if (cfg->jar) {
        char *cookie = mi_cookiejar_header_for_url(cfg->jar, url);
        if (cookie) {
            char *h = mi_asprintf("Cookie: %s", cookie);
            if (h) { list = curl_slist_append(list, h); free(h); }
            free(cookie);
        }
    }

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

/*
 * Probe in one request.
 *
 * This used to be two: a HEAD for the size and type, then a ranged GET for the
 * bytes to sniff. That is two DNS lookups, two TCP handshakes and two TLS
 * handshakes — measured at 0.5-0.7s against a well-connected host, and far
 * worse against a slow one, because each request carried the full 30s timeout
 * and they ran in sequence.
 *
 * A single ranged GET answers everything a HEAD did and more:
 *
 *   Content-Range: bytes 0-8191/3000000   -> total size AND range support
 *   Content-Type                          -> the declared type
 *   the body itself                       -> what the detector actually reads
 *
 * It is also *more* correct. The old code set `accept_ranges = 1` whenever the
 * ranged GET returned any bytes at all — but a server that ignores `Range`
 * answers 200 with the whole object, which also returns bytes. The status code
 * and `Content-Range` are what actually say whether ranges are honoured.
 *
 * The body is bounded by the range request, but a server may ignore it and
 * start streaming the whole file, so the write callback stops early once it has
 * what it needs. Without that, probing a 4 GB object that ignores ranges would
 * try to buffer the lot.
 */
typedef struct {
    unsigned char *out;
    size_t cap;
    size_t len;
} sniff_sink;

static size_t write_sniff(char *ptr, size_t size, size_t nmemb, void *userdata) {
    sniff_sink *s = userdata;
    size_t n = size * nmemb;
    if (s->len < s->cap) {
        size_t take = n < (s->cap - s->len) ? n : (s->cap - s->len);
        memcpy(s->out + s->len, ptr, take);
        s->len += take;
    }
    /* Report the whole length as consumed: returning less than n makes libcurl
     * treat it as a write error, and the point here is to stop reading without
     * failing the transfer. `CURLOPT_MAXFILESIZE`-style aborting is done by
     * returning 0 from the callback, which would lose the response code. */
    return n;
}

/*
 * How long to wait for a probe to *connect*, as opposed to complete.
 *
 * The overall timeout stops a slow transfer hanging the UI; this stops a
 * black-holed address consuming the whole of it before a single byte has moved.
 * A quarter of the overall budget, capped at 10s: long enough for a slow mobile
 * network, short enough that a dead host is reported quickly.
 */
static long probe_connect_timeout(const mi_download_config *cfg) {
    long t = cfg->timeout_seconds / 4;
    if (t < 3) t = 3;
    if (t > 10) t = 10;
    return t;
}

/** Parse "bytes 0-8191/3000000" for the total, and note that ranges work. */
static void parse_content_range(const char *v, int64_t *total, int *ranged) {
    if (!v) return;
    if (strncmp(v, "bytes", 5) != 0) return;
    const char *slash = strchr(v, '/');
    if (!slash || !slash[1]) return;
    if (slash[1] == '*') return; /* total unknown, but the range was honoured */
    char *endp = NULL;
    long long n = strtoll(slash + 1, &endp, 10);
    if (endp != slash + 1 && n >= 0) *total = (int64_t)n;
    *ranged = 1;
}

int mi_probe(const mi_download_config *cfg, const char *url,
             mi_download_plan *out) {
    if (ensure_curl() != 0) return MI_ERR;
    plan_init(out, url);

    CURL *curl = curl_easy_init();
    if (!curl) return MI_ERR;

    unsigned char head[MI_HEADER_SNIFF];
    sniff_sink sink = {.out = head, .cap = sizeof head, .len = 0};

    char range[64];
    snprintf(range, sizeof range, "0-%zu", sizeof head - 1);
    struct curl_slist *headers = build_headers(cfg, url, range);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (cfg->proxy && *cfg->proxy) curl_easy_setopt(curl, CURLOPT_PROXY, cfg->proxy);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_sniff);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    /*
     * A probe is interactive: someone is waiting for the result. The overall
     * timeout is what stops a slow host hanging the UI, and the connect timeout
     * is what stops a black-holed address consuming the whole of it before the
     * request has even started. The old code had neither, so a dead host meant
     * 30s, then another 30s.
     */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, cfg->timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, probe_connect_timeout(cfg));
    /* A probe must not be tricked into reading a huge object by a server that
     * ignores the range. Two requests' worth of bytes is ample for sniffing. */
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)(16 * 1024 * 1024));

    CURLcode rc = curl_easy_perform(curl);

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    /*
     * Name the refusal here, while the body is still in hand.
     *
     * The probe already read the first bytes of the response, which is exactly
     * what tells a challenge wall from a plain ban — so the classification costs
     * nothing extra and is what lets the caller react correctly instead of
     * blindly retrying.
     */
    out->status = (int)code;
    out->block = mi_antibot_classify((int)code, NULL, (const char *)head, sink.len);

    if (rc == CURLE_OK && code > 0) {
        char *ct = NULL, *eff = NULL, *cr = NULL;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
        if (ct) out->content_type = mi_strdup(ct);
        if (eff) out->final_url = mi_strdup(eff);

        /*
         * Size and range support, from the response rather than assumed.
         *
         * A 206 with Content-Range is the unambiguous "ranges work, here is the
         * total". A 200 means the server ignored the Range and is sending the
         * whole object, so ranges are not available and the size is whatever
         * Content-Length said — which is the total, since it is not a partial
         * response.
         */
        curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
                          (curl_off_t *)&out->total_bytes);
        struct curl_header *h = NULL;
        if (curl_easy_header(curl, "Content-Range", 0, CURLH_HEADER, -1, &h) == CURLHE_OK) {
            cr = h->value;
        }
        int ranged = 0;
        int64_t total = -1;
        parse_content_range(cr, &total, &ranged);

        if (code == 206 || ranged) {
            out->accept_ranges = 1;
            if (total >= 0) out->total_bytes = total;
        } else {
            /* 200: the whole object was offered, so no range support. */
            out->accept_ranges = 0;
            if (code != 200) out->total_bytes = -1;
        }
        /* A 405/501/403 answer means the method or the resource refused us, and
         * any length it reported is not the object's. */
        if (code == 405 || code == 501 || code == 403) {
            out->total_bytes = -1;
            out->accept_ranges = 0;
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    /*
     * Detect from whatever arrived, even on a non-OK result: a 200 that
     * returned bytes is still worth identifying, and a server that refuses
     * HEAD but serves GET would previously have been reported as having no
     * media at all.
     */
    if (sink.len > 0) {
        mi_detect(head, sink.len, &out->media);
        out->have_media = 1;
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
    struct curl_slist *headers = build_headers(cfg, url, range);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (cfg->proxy && *cfg->proxy) curl_easy_setopt(curl, CURLOPT_PROXY, cfg->proxy);
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

    /*
     * Say what the transfer actually is, up front.
     *
     * Without this the log held nothing between "download started" and the
     * outcome, so a slow transfer and a stalled one looked identical. The line
     * names the shape of the work: how many connections, how much is left, and
     * whether any of it was resumed from a partial.
     */
    mi_logf(MI_LOG_INFO,
            "download: ranged transfer — %d connection%s, %d of %d chunk%s left, "
            "%lld of %lld bytes already on disk%s",
            pending, pending == 1 ? "" : "s",
            pending, count, count == 1 ? "" : "s",
            (long long)base, (long long)total,
            resumed ? " (resuming)" : "");

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
    struct curl_slist *headers = build_headers(cfg, url, NULL);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (cfg->proxy && *cfg->proxy) curl_easy_setopt(curl, CURLOPT_PROXY, cfg->proxy);
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
    free(info->etag);
    free(info->last_modified);
    free(info->encoding);
    free(info->location);
    memset(info, 0, sizeof *info);
}

/* Gathers the response headers a page view needs. */
typedef struct {
    mi_fetch_info *info;
    char *csp; /* Content-Security-Policy, when the response carries one */
    /* The live jar, when one is set, and the request's host/path — so a
     * `Set-Cookie` can be scoped and stored as the browser would store it. */
    struct mi_cookiejar *jar;
    char *host;
    char *path;
} doc_header_ctx;

/*
 * Apply one response header line to the info.
 *
 * Shared by the libcurl path and the impersonation helper's path, so the rule
 * for "may this page be framed" lives in exactly one place — a page routed
 * through the helper must get the same framing answer as one fetched by curl.
 */
static void apply_header_line(mi_fetch_info *info, char **csp, const char *ptr,
                              size_t n) {
    if (!info) return;

    /* X-Frame-Options: DENY or SAMEORIGIN both forbid another origin framing
     * this page. A value that merely whitelists some other origin also blocks
     * us, so anything other than an absent header counts as "not framable"
     * unless it explicitly allows everything. */
    if (n > 17 && !strncasecmp(ptr, "X-Frame-Options:", 16)) {
        const char *v = ptr + 16;
        while (*v == ' ' || *v == '\t') v++;
        if (!strncasecmp(v, "DENY", 4) || !strncasecmp(v, "SAMEORIGIN", 10) ||
            !strncasecmp(v, "ALLOW-FROM", 10)) {
            info->framable = 0;
            if (!info->frame_blocker) {
                info->frame_blocker = mi_strndup("X-Frame-Options",
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
        free(*csp);
        *csp = mi_strndup(v, len);
    }

    if (n > 13 && !strncasecmp(ptr, "Content-Type:", 13)) {
        const char *v = ptr + 13;
        while (*v == ' ' || *v == '\t') v++;
        size_t len = n - (size_t)(v - ptr);
        while (len > 0 && (v[len - 1] == '\r' || v[len - 1] == '\n')) len--;
        free(info->content_type);
        info->content_type = mi_strndup(v, len);
    }

    if (n > 9 && !strncasecmp(ptr, "Location:", 9)) {
        const char *v = ptr + 9;
        while (*v == ' ' || *v == '\t') v++;
        size_t len = n - (size_t)(v - ptr);
        while (len > 0 && (v[len - 1] == '\r' || v[len - 1] == '\n')) len--;
        free(info->location);
        info->location = mi_strndup(v, len);
    }

    /* The validators, so a caller can ask "has this changed?" next time. */
    if (n > 5 && !strncasecmp(ptr, "ETag:", 5)) {
        const char *v = ptr + 5;
        while (*v == ' ' || *v == '\t') v++;
        size_t len = n - (size_t)(v - ptr);
        while (len > 0 && (v[len - 1] == '\r' || v[len - 1] == '\n')) len--;
        free(info->etag);
        info->etag = mi_strndup(v, len);
    }
    if (n > 14 && !strncasecmp(ptr, "Last-Modified:", 14)) {
        const char *v = ptr + 14;
        while (*v == ' ' || *v == '\t') v++;
        size_t len = n - (size_t)(v - ptr);
        while (len > 0 && (v[len - 1] == '\r' || v[len - 1] == '\n')) len--;
        free(info->last_modified);
        info->last_modified = mi_strndup(v, len);
    }
}

static size_t header_document(char *ptr, size_t size, size_t nmemb, void *userdata) {
    doc_header_ctx *ctx = userdata;
    size_t n = size * nmemb;
    if (!ctx || !ctx->info) return n;
    apply_header_line(ctx->info, &ctx->csp, ptr, n);
    /* A Set-Cookie is stored in the live jar, so a session survives the request
     * that set it. */
    if (ctx->jar && n > 11 && !strncasecmp(ptr, "Set-Cookie:", 11)) {
        const char *v = ptr + 11;
        while (*v == ' ' || *v == '\t') v++;
        char *line = mi_strndup(v, n - (size_t)(v - ptr));
        if (line) {
            /* Strip the trailing CRLF the header carries. */
            size_t l = strlen(line);
            while (l > 0 && (line[l - 1] == '\r' || line[l - 1] == '\n')) line[--l] = 0;
            mi_cookiejar_set(ctx->jar, line, ctx->host, ctx->path);
            free(line);
        }
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

    /*
     * A site that fingerprints the TLS handshake refuses libcurl however it is
     * dressed, and no header can pass that check. When the caller named a browser
     * to imitate, the fetch goes through the helper that can wear its handshake.
     *
     * This is the plain page fetch (`info == NULL`, the crawler's path). The page
     * endpoint keeps libcurl because it also reads the framing headers, which the
     * helper does not return. A helper that is missing or fails falls through to
     * libcurl, so a plain fetch is still better than none.
     */
    if (cfg->impersonate && *cfg->impersonate) {
        mi_impersonate_config icfg;
        memset(&icfg, 0, sizeof icfg);
        icfg.impersonate = cfg->impersonate;
        icfg.proxy = cfg->proxy;
        icfg.user_agent = cfg->user_agent;
        icfg.cookies_text = cfg->cookies_text;
        icfg.timeout_seconds = (int)cfg->timeout_seconds;
        char ierr[200] = {0};
        char *meta = NULL;
        char *body = mi_impersonate_fetch(&icfg, url, max_bytes, &meta, ierr, sizeof ierr);
        if (body) {
            /*
             * The helper returns the response headers too, and they go through
             * the *same* rule libcurl's path uses — so a page fetched with a
             * browser's handshake gets the same framing answer, and the console
             * shows the same refusal.
             */
            if (info) {
                info->framable = 1;
                char *csp = NULL;
                if (meta) {
                    mi_json *m = mi_json_parse(meta);
                    const char *final_url = mi_json_get_string(m, "final_url", NULL);
                    if (final_url && strcmp(final_url, url) != 0) {
                        info->final_url = mi_strdup(final_url);
                    }
                    const mi_json *hdrs = mi_json_get(m, "headers");
                    if (hdrs && hdrs->type == MI_JSON_ARRAY) {
                        for (size_t i = 0; i < hdrs->count; i++) {
                            const mi_json *h = hdrs->items[i];
                            if (h && h->type == MI_JSON_STRING && h->string) {
                                apply_header_line(info, &csp, h->string, strlen(h->string));
                            }
                        }
                    }
                    mi_json_value_free(m);
                }
                if (csp_forbids_framing(csp)) {
                    info->framable = 0;
                    if (!info->frame_blocker) {
                        info->frame_blocker = mi_strdup("Content-Security-Policy");
                    }
                }
                free(csp);
            }
            free(meta);
            return body;
        }
        free(meta);
        mi_logf(MI_LOG_WARN, "impersonated fetch failed (%s); falling back to libcurl",
                ierr);
    }

    if (ensure_curl() != 0 || !url) return NULL;
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    doc_header_ctx hctx = {.info = info, .csp = NULL};
    if (info) info->framable = 1;
    /* The jar, when there is one, and where this request is going — a cookie is
     * scoped to the request that set it. */
    hctx.jar = cfg->jar;
    if (cfg->jar) {
        hctx.host = mi_download_host_of(url);
        hctx.path = mi_download_path_of(url);
    }

    membuf b = {0};
    struct curl_slist *headers = build_headers(cfg, url, NULL);
    /* The conditional-GET conditions, when the caller has them. A server that
     * still has the same version answers 304 and sends no body, which is how an
     * incremental crawl avoids re-reading an unchanged page. */
    char *inm = NULL, *ims = NULL;
    if (cfg->if_none_match && *cfg->if_none_match) {
        inm = mi_asprintf("If-None-Match: %s", cfg->if_none_match);
        if (inm) headers = curl_slist_append(headers, inm);
    }
    if (cfg->if_modified_since && *cfg->if_modified_since) {
        ims = mi_asprintf("If-Modified-Since: %s", cfg->if_modified_since);
        if (ims) headers = curl_slist_append(headers, ims);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_membuf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    /* Read the headers even when the caller did not ask for the framing info,
     * so the validators are always available to a crawl. */
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_document);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hctx);
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
        /* The status is reported for every document fetch, not just a POST: a
         * link check and a login both judge the response by it. */
        info->status = (int)code;
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
    free(hctx.host);
    free(hctx.path);
    free(inm);
    free(ims);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    /*
     * 304 is a success, not a failure: the caller's copy is current and there is
     * nothing to transfer. It is reported so the caller can tell "unchanged"
     * from "empty".
     */
    if (code == 304) {
        if (info) info->not_modified = 1;
        free(b.data);
        return mi_strdup("");
    }
    if (rc != CURLE_OK || code < 200 || code >= 300 || !b.data) {
        free(b.data);
        return NULL;
    }
    if (b.len > max_bytes) {
        b.data[max_bytes] = '\0';
        b.len = max_bytes;
    }

    /*
     * Decode to UTF-8 before returning.
     *
     * The parser assumes UTF-8, so a page in windows-1252, Shift-JIS or GBK
     * would otherwise come out as mojibake — and, worse, the *extracted data*
     * would be wrong, because a selector runs over the mis-decoded bytes. The
     * encoding is sniffed from the BOM, the Content-Type and a `<meta>` prescan,
     * in the order the HTML Standard prescribes, and transcoded here so every
     * consumer sees correct text. The chosen encoding is reported on `info`.
     */
    {
        const char *ct = info && info->content_type ? info->content_type : NULL;
        mi_charset chosen = MI_CS_UNKNOWN;
        char *utf8 = mi_charset_decode_document(ct, (const unsigned char *)b.data,
                                                b.len, MI_CS_UTF8, &chosen);
        if (utf8) {
            free(b.data);
            b.data = utf8;
            if (info) {
                free(info->encoding);
                info->encoding = mi_strdup(mi_charset_name(chosen));
            }
        }
    }
    return b.data;
}

char *mi_download_fetch_text(const mi_download_config *cfg, const char *url,
                             size_t max_bytes) {
    return mi_download_fetch_document(cfg, url, max_bytes, NULL);
}

/*
 * POST a body, and return the response.
 *
 * The same shape as the document fetch — same headers, same jar, same decode —
 * with a method and a body. It is what a login needs: the credentials go in the
 * body, the site answers with a `Set-Cookie`, and the jar stores it so the next
 * request is authenticated. A redirect is followed, because a successful login
 * is usually a 302 to the account page.
 */
char *mi_download_post(const mi_download_config *cfg, const char *url,
                       const char *body, const char *content_type,
                       size_t max_bytes, mi_fetch_info *info) {
    if (info) memset(info, 0, sizeof *info);
    if (ensure_curl() != 0 || !url || !body) return NULL;
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    doc_header_ctx hctx = {.info = info, .csp = NULL};
    if (info) info->framable = 1;
    hctx.jar = cfg->jar;
    if (cfg->jar) {
        hctx.host = mi_download_host_of(url);
        hctx.path = mi_download_path_of(url);
    }

    membuf b = {0};
    struct curl_slist *headers = build_headers(cfg, url, NULL);
    char *ct = mi_asprintf("Content-Type: %s",
                           content_type ? content_type
                                        : "application/x-www-form-urlencoded");
    if (ct) headers = curl_slist_append(headers, ct);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_membuf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_document);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hctx);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (cfg->proxy && *cfg->proxy) curl_easy_setopt(curl, CURLOPT_PROXY, cfg->proxy);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, cfg->timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (info) {
        char *eff = NULL;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
        if (eff && strcmp(eff, url) != 0) info->final_url = mi_strdup(eff);
        if (csp_forbids_framing(hctx.csp)) {
            info->framable = 0;
            if (!info->frame_blocker) info->frame_blocker = mi_strdup("Content-Security-Policy");
        }
    }
    free(hctx.csp);
    free(hctx.host);
    free(hctx.path);
    free(ct);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    /*
     * Any HTTP status is a valid answer here: a login commonly ends on a 302 to
     * a page, and the caller judges success from the content and the cookies,
     * not the status. Only a transport failure is an error.
     */
    if (rc != CURLE_OK || code == 0) {
        free(b.data);
        return NULL;
    }
    if (info) info->status = (int)code;
    /* A 302 carries no body, so b.data is NULL — which is a valid response, not
     * a failure. A login is answered with one. */
    if (!b.data) { b.data = mi_strdup(""); b.len = 0; }
    if (b.len > max_bytes) { b.data[max_bytes] = 0; b.len = max_bytes; }

    /* Decode the response to UTF-8, the same as a document fetch. */
    {
        const char *cct = info && info->content_type ? info->content_type : NULL;
        mi_charset chosen = MI_CS_UNKNOWN;
        char *utf8 = mi_charset_decode_document(cct, (const unsigned char *)b.data,
                                                b.len, MI_CS_UTF8, &chosen);
        if (utf8) {
            free(b.data);
            b.data = utf8;
            if (info) {
                free(info->encoding);
                info->encoding = mi_strdup(mi_charset_name(chosen));
            }
        }
    }
    return b.data;
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
    cfg->accept_language = NULL;
    cfg->control = NULL;
    cfg->ffmpeg_path = NULL;
    cfg->extract_pages = 0;
    cfg->extractor_path = NULL;
    cfg->gallery_extractor_path = NULL;
    cfg->torrent_extractor_path = NULL;
    cfg->torrent_site = NULL;
    cfg->magnet_downloader_path = NULL;
    cfg->torrent_bytes = NULL;
    cfg->torrent_len = 0;
    cfg->torrent_timeout_seconds = 0;
    cfg->magnet_files = NULL;
    cfg->magnet_file_count = 0;
    cfg->cookies_text = NULL;
    cfg->cookies_from_browser = NULL;
    cfg->jar = NULL;
    cfg->proxy = NULL;
    cfg->impersonate = NULL;
    cfg->identity_retries = 0;
    cfg->rotate_identity = 1;
    cfg->preferred_height = 0;
}

void mi_download_result_free(mi_download_result *res) {
    if (!res) return;
    free(res->url);
    free(res->path);
    mi_media_info_free(&res->media);
    memset(res, 0, sizeof *res);
}

/* ------------------------------------------------------------------ */
/* Fetching a manifest                                                 */
/* ------------------------------------------------------------------ */

/*
 * A playlist is not the video.
 *
 * An HLS or DASH URL answers with a text playlist — measured on one YouTube
 * stream: 48,305 bytes of `#EXTINF` lines where the video is hundreds of
 * megabytes. The detector correctly calls that `hls`, but the transfer paths
 * treat it as an ordinary object with a size and range support, so the playlist
 * itself was saved as the "download". It looked like a success: state `done`,
 * a file on disk, the right name.
 *
 * A manifest needs a *reader* that understands it, and ffmpeg is the one already
 * in this pipeline. It fetches the playlist, follows every segment, and muxes
 * them into a single file — which is exactly what "download this stream" means.
 *
 * The arguments are passed as an argv array through fork/execvp, never through a
 * shell: the URL comes from a caller and would otherwise be a command injection.
 */
static int download_manifest(const mi_download_config *cfg, const char *url,
                             const char *dest, mi_progress_fn on_progress, void *user,
                             mi_download_result *out) {
    const char *ffmpeg = cfg->ffmpeg_path ? cfg->ffmpeg_path : "ffmpeg";

    const char *argv[24];
    int a = 0;
    argv[a++] = ffmpeg;
    argv[a++] = "-nostdin";
    argv[a++] = "-loglevel";
    argv[a++] = "error";
    /* Overwrite: the destination is Vyzor's own file, and a resumed run must
     * replace a partial rather than stop to ask. */
    argv[a++] = "-y";
    /*
     * A manifest is a live-shaped thing; without a limit ffmpeg would happily
     * follow a live stream forever. `-t` is not used because the duration is not
     * known here — the crawl's own budget is what bounds the caller instead.
     */
    argv[a++] = "-i";
    argv[a++] = url;
    /* Remux, never re-encode: the segments are already encoded, and copying
     * them is both faster and lossless. */
    argv[a++] = "-c";
    argv[a++] = "copy";
    /*
     * No `-bsf:a aac_adtstoasc` here.
     *
     * It was added to turn HLS AAC (ADTS) into MP4 AAC, but it is applied
     * unconditionally, so any stream whose audio is not AAC makes ffmpeg abort:
     * "Codec 'ac3' is not supported by the bitstream filter 'aac_adtstoasc'",
     * exit 234, no output file — measured on Apple's fMP4 example, whose default
     * variant carries ac-3. ffmpeg's mp4 muxer already applies that conversion
     * itself when it is needed, so the flag buys nothing and breaks a whole class
     * of streams. Verified: with it removed the Apple stream muxes to a valid
     * 1920x1080 h264+ac3 file, and an AAC HLS stream is byte-for-byte the same
     * size with and without it.
     */
    /*
     * The output format has to be stated, because the destination's name is not
     * a clue: Vyzor saves the transfer as `source.bin` and identifies it by
     * content afterwards. ffmpeg refuses to mux into a name it cannot read a
     * container from — measured: "Unable to choose an output format for
     * 'source.bin'", exit 234, with a valid stream and a working command line.
     */
    argv[a++] = "-f";
    argv[a++] = "mp4";
    argv[a++] = dest;
    argv[a] = NULL;

    mi_logf(MI_LOG_INFO, "download: reading the manifest with %s", ffmpeg);
    int child_err = -1;
    pid_t pid = spawn_relayed((const char *const *)argv, &child_err);
    if (pid < 0) return MI_ERR_IO;

    int status = 0;
    /* Poll rather than wait, so progress is reported while it runs: a stream
     * takes minutes and a frozen bar reads as a hang. */
    int64_t last_reported = -1;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            if (child_err >= 0) close(child_err);
            return MI_ERR_IO;
        }
        /* The tool's own words, so a failure to read the stream is explained
         * rather than only counted. */
        relay_child_output(child_err);
        int64_t on_disk = mi_file_size(dest);
        /* Only log when it moves; a line every half-second for an unchanged
         * figure is noise, and the change is the interesting event. */
        if (on_disk > 0 && on_disk != last_reported) {
            mi_logf(MI_LOG_INFO, "download: manifest %lld bytes written so far",
                    (long long)on_disk);
            last_reported = on_disk;
        }
        if (on_progress) {
            mi_progress p;
            memset(&p, 0, sizeof p);
            p.total_bytes = -1;
            p.downloaded_bytes = mi_file_size(dest);
            on_progress(&p, user);
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 500 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    /* The last of the child's output, before its exit is judged. */
    relay_child_output(child_err);
    if (child_err >= 0) close(child_err);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        mi_logf(MI_LOG_ERROR, "ffmpeg could not read the stream (exit %d)",
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return MI_ERR_NET;
    }

    int64_t written = mi_file_size(dest);
    if (written <= 0) return MI_ERR_IO;
    out->bytes_written = written;
    mi_logf(MI_LOG_INFO, "download: the manifest produced %lld bytes", (long long)written);

    /* Identify what was produced, so the caller knows it has real video rather
     * than another playlist. */
    mi_detect_file(dest, MI_HEADER_SNIFF, &out->media);
    return MI_OK;
}

/*
 * Copy a file's bytes, for the one case `rename` cannot handle: a destination on
 * a different filesystem. The extractor's temp file is written beside the
 * destination, so this is a safety net rather than the common path.
 *
 * Returns 0 on success, non-zero on failure.
 */
static int copy_file_bytes(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return -1; }
    char buf[65536];
    int rc = 0;
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0) { if (errno == EINTR) continue; rc = -1; break; }
        if (n == 0) break;
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) { if (errno == EINTR) continue; rc = -1; break; }
            off += w;
        }
        if (rc != 0) break;
    }
    close(in);
    if (close(out) != 0) rc = -1;
    return rc;
}

/*
 * Fetch a streaming page with the extractor's own downloader.
 *
 * Why this exists, and why it is tried first.
 *
 * Resolving a page and then handing the resulting URLs to ffmpeg works only when
 * those URLs are plain files the host can fetch. On YouTube they are not: the
 * media URLs are signed and bound to the request that resolved them, so ffmpeg's
 * separate fetch is refused — measured, `Server returned 403 Forbidden` on the
 * googlevideo URL, for every format, even with `--force-ipv4` and with no IPv6 on
 * the host. yt-dlp's own downloader does not have that problem, because it makes
 * the request the URL was signed for.
 *
 * So the extractor is asked to download, not just to resolve. yt-dlp already
 * understands every site it can resolve — the formats, the audio/video pairing,
 * HLS and DASH — so this replaces a hand-rolled mux with the tool built for it,
 * and carries the cookies, proxy and impersonation the resolve used.
 *
 * The one thing it cannot do is write to Vyzor's `source.bin` and be identified
 * afterwards: yt-dlp writes a real container. So the destination is a temp file
 * with the container's extension, and the result is moved onto `dest` — which is
 * also what makes the content-detection below see the true bytes rather than a
 * name.
 *
 * Returns MI_OK, or MI_ERR_NET when the extractor could not do it (so the caller
 * falls back to the ffmpeg mux, which still works for the sites ffmpeg can
 * reach).
 */
static int download_via_extractor(const mi_download_config *cfg, const char *page_url,
                                  const char *dest, mi_progress_fn on_progress, void *user,
                                  mi_download_result *out) {
    const char *binary = (cfg->extractor_path && *cfg->extractor_path)
                             ? cfg->extractor_path
                             : "yt-dlp";

    /* A temp path the extractor can write, with an extension it recognises as a
     * container, in the destination's own directory so the move is a rename. */
    char *tmp = mi_asprintf("%s.extract.%%(ext)s", dest);
    if (!tmp) return MI_ERR_NOMEM;

    const char *argv[40];
    int a = 0;
    argv[a++] = binary;
    argv[a++] = "--no-playlist";
    argv[a++] = "--no-warnings";
    argv[a++] = "--no-progress";
    /* Never resume a stale partial from a previous attempt: the destination is
     * Vyzor's own file and a resumed one would be stitched to the wrong bytes. */
    argv[a++] = "--no-continue";
    argv[a++] = "-o";
    argv[a++] = tmp;
    /*
     * The chosen quality, when the caller named one. yt-dlp's own selector
     * language, so "360p" becomes a height ceiling the tool understands. Left
     * unset, yt-dlp takes its own default (best video + best audio).
     */
    char *fmt = NULL;
    if (cfg->preferred_height > 0) {
        fmt = mi_asprintf("bv*[height<=%d]+ba/b[height<=%d]/bv*+ba/b",
                          cfg->preferred_height, cfg->preferred_height);
        if (fmt) {
            argv[a++] = "-f";
            argv[a++] = fmt;
        }
    }
    /* Cookies: a path, or the inline text written to a temp file, exactly as the
     * resolve path does it. */
    char *cookie_tmp = NULL;
    if (cfg->cookies_text && *cfg->cookies_text) {
        if (mi_extract_write_cookies_file(cfg->cookies_text, &cookie_tmp) == 0) {
            argv[a++] = "--cookies";
            argv[a++] = cookie_tmp;
        }
    } else if (cfg->cookies_from_browser && *cfg->cookies_from_browser) {
        argv[a++] = "--cookies-from-browser";
        argv[a++] = cfg->cookies_from_browser;
    }
    if (cfg->proxy && *cfg->proxy) {
        argv[a++] = "--proxy";
        argv[a++] = cfg->proxy;
    }
    char *imp = mi_extract_normalize_impersonate(cfg->impersonate);
    if (imp) {
        argv[a++] = "--impersonate";
        argv[a++] = imp;
    }
    argv[a++] = "--";
    argv[a++] = page_url;
    argv[a] = NULL;

    mi_logf(MI_LOG_INFO, "download: fetching the page with %s", binary);

    int child_err = -1;
    pid_t pid = spawn_relayed((const char *const *)argv, &child_err);
    if (pid < 0) {
        free(fmt);
        free(imp);
        if (cookie_tmp) { unlink(cookie_tmp); free(cookie_tmp); }
        free(tmp);
        return MI_ERR_IO;
    }

    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            free(fmt); free(imp);
            if (child_err >= 0) close(child_err);
            if (cookie_tmp) { unlink(cookie_tmp); free(cookie_tmp); }
            free(tmp);
            return MI_ERR_IO;
        }
        /* The downloader's own progress and errors: yt-dlp prints "has already
         * been downloaded", the destination it chose, and any refusal here. */
        relay_child_output(child_err);
        /* Honour a pause/cancel the same way the ffmpeg path does. */
        if (cfg->control && cfg->control->stop && cfg->control->stop(cfg->control->user)) {
            kill(pid, SIGTERM);
            struct timespec grace = {.tv_sec = 2, .tv_nsec = 0};
            nanosleep(&grace, NULL);
            if (waitpid(pid, &status, WNOHANG) != pid) kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            free(fmt); free(imp);
            if (child_err >= 0) close(child_err);
            if (cookie_tmp) { unlink(cookie_tmp); free(cookie_tmp); }
            free(tmp);
            return MI_ERR_ABORTED;
        }
        if (on_progress) {
            mi_progress p;
            memset(&p, 0, sizeof p);
            p.total_bytes = -1;
            p.downloaded_bytes = 0;
            on_progress(&p, user);
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 500 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    relay_child_output(child_err);
    if (child_err >= 0) close(child_err);
    free(fmt);
    free(imp);
    if (cookie_tmp) { unlink(cookie_tmp); free(cookie_tmp); }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        mi_logf(MI_LOG_INFO, "the extractor downloader failed (exit %d)",
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        free(tmp);
        return MI_ERR_NET;
    }

    /*
     * The extractor wrote `<dest>.extract.<ext>`, possibly with a second file
     * (a separate audio track it muxed). Find the largest such file, which is
     * the result, and move it onto `dest`.
     *
     * The directory is the destination's own — Vyzor always writes into a
     * per-download directory, so a separator is present — and the prefix is the
     * destination's basename plus the marker.
     */
    char *produced = NULL;
    {
        char *slash = strrchr(dest, '/');
        char *dir = slash ? mi_strndup(dest, (size_t)(slash - dest)) : mi_strdup(".");
        char *base = mi_strdup(slash ? slash + 1 : dest);
        if (dir && base) {
            char *prefix = mi_asprintf("%s.extract.", base);
            if (prefix) {
                DIR *d = opendir(dir);
                if (d) {
                    int64_t best_size = -1;
                    struct dirent *ent;
                    while ((ent = readdir(d)) != NULL) {
                        if (strncmp(ent->d_name, prefix, strlen(prefix)) != 0) continue;
                        char *full = mi_path_join(dir, ent->d_name);
                        if (!full) continue;
                        int64_t sz = mi_file_size(full);
                        if (sz > best_size) {
                            best_size = sz;
                            free(produced);
                            produced = full;
                        } else {
                            free(full);
                        }
                    }
                    closedir(d);
                }
                free(prefix);
            }
        }
        free(dir);
        free(base);
    }
    free(tmp);

    if (!produced) {
        mi_logf(MI_LOG_INFO, "the extractor reported success but wrote no file");
        return MI_ERR_NET;
    }
    if (rename(produced, dest) != 0) {
        /* A cross-device rename fails; fall back to copying the bytes. */
        int rc = copy_file_bytes(produced, dest);
        unlink(produced);
        if (rc != 0) {
            free(produced);
            return MI_ERR_IO;
        }
    }
    free(produced);

    int64_t written = mi_file_size(dest);
    if (written <= 0) return MI_ERR_IO;
    out->bytes_written = written;
    mi_detect_file(dest, MI_HEADER_SNIFF, &out->media);
    return MI_OK;
}

/*
 * Which item of a playlist to fetch when the caller asked for "the page".
 *
 * Prefers an entry the extractor marks as a size, else the last usable one:
 * a title-then-quality listing puts its highest rendition last (measured on
 * archive.org's ElephantsDream, whose entries are an SD `ed_1024.avi` then an
 * HD `ed_hd.avi`). Entries that are themselves playlists, or that carry no URL,
 * are skipped — otherwise an archive.org "download all" index page, which links
 * only other pages, would shadow the files it lists.
 */
static const char *pick_entry_url(const mi_extract_result *er) {
    const char *best = NULL;
    for (size_t i = 0; i < er->entry_count; i++) {
        const mi_extract_entry *e = &er->entries[i];
        if (!e->url || !*e->url || e->is_playlist) continue;
        best = e->url;
    }
    return best;
}

/*
 * A streaming *page*, resolved to its streams and muxed.
 *
 * A video the extractor can read is not one URL. Measured on a YouTube watch
 * page: the video and the audio arrive as two separate streams — zero formats
 * carried both — so neither alone is a playable file. ffmpeg muxes them, and
 * this is where that happens.
 *
 * The page is resolved *here*, at download time, rather than when the crawl
 * found it. A resolved stream URL is signed and bound to the session that asked,
 * and expires within hours, so a URL recorded by a crawl would be dead by the
 * time anyone downloaded it.
 */
static int download_stream_page(const mi_download_config *cfg, const char *page_url,
                                const char *dest, mi_progress_fn on_progress, void *user,
                                mi_download_result *out) {
    /*
     * First attempt: let the extractor download the page itself.
     *
     * It is the only path that works when the media URLs are bound to the request
     * that resolved them (YouTube), and it handles HLS/DASH and the video/audio
     * pairing the ffmpeg path below reimplements. So it is tried first, and the
     * resolve-then-mux path is the fallback for the sites it cannot serve.
     *
     * Only for a video site: a gallery host is gallery-dl's job and the ffmpeg
     * path does not apply to it either, but yt-dlp has no downloader for it.
     */
    mi_extract_config pick_cfg;
    mi_extract_config_init(&pick_cfg);
    pick_cfg.binary = cfg->extractor_path;
    pick_cfg.gallery_binary = cfg->gallery_extractor_path;
    if (mi_extract_pick(&pick_cfg, page_url) != MI_EXTRACT_GALLERY) {
        int via = download_via_extractor(cfg, page_url, dest, on_progress, user, out);
        if (via == MI_OK) return MI_OK;
        if (via == MI_ERR_ABORTED) return MI_ERR_ABORTED; /* a pause/cancel, not a failure */
    }

    mi_extract_config ecfg;
    mi_extract_config_init(&ecfg);
    ecfg.binary = cfg->extractor_path;
    ecfg.gallery_binary = cfg->gallery_extractor_path;
    ecfg.torrent_binary = cfg->torrent_extractor_path;
    ecfg.torrent_site = cfg->torrent_site;
    /* The same cookies the download was given: resolving a page and fetching its
     * media are two requests to the same host, and a site that needs a sign-in
     * needs it for both. */
    ecfg.cookies_text = cfg->cookies_text;
    ecfg.cookies_from_browser = cfg->cookies_from_browser;
    /* The same egress the transfer uses: the resolve is a request to the same
     * host, so a proxy that got the fetch past a refusal must carry it too. */
    ecfg.proxy = cfg->proxy;
    ecfg.impersonate = cfg->impersonate;

    mi_extract_result er;
    if (mi_extract(&ecfg, page_url, &er) != MI_OK) return MI_ERR_NET;

    /*
     * A page that is a *listing* rather than a video.
     *
     * archive.org's `/details/` page is the example: some resolve to a single
     * video (`BigBuckBunny_124` — `_type: video`, three formats), others to a
     * playlist of separate files (`ElephantsDream` — `_type: playlist`, two
     * entries, zero formats), and the second shape reported "the extractor found
     * no video stream" for a page that plainly has one. When there is no format
     * but there is a usable entry, resolve that entry instead: it answers with
     * its own formats, and the rest of this function is unchanged.
     *
     * Deliberately one level deep and only when the caller gave no preference:
     * "the last usable entry" is a defensible default, not a search.
     */
    mi_extract_result er2;
    memset(&er2, 0, sizeof er2);
    if (er.format_count == 0 && er.entry_count > 0) {
        const char *entry = pick_entry_url(&er);
        if (entry && mi_extract(&ecfg, entry, &er2) == MI_OK && er2.format_count > 0) {
            mi_logf(MI_LOG_INFO, "download: page is a playlist; resolved entry %s", entry);
            mi_extract_result_free(&er);
            er = er2; /* the entry's formats replace the listing's, so the rest is unchanged */
        } else {
            mi_extract_result_free(&er2);
        }
    }

    /*
     * The best video and the best audio, chosen by resolution and then bitrate.
     * A muxed format is preferred when one exists, because it needs no second
     * input.
     */
    const mi_extract_format *video = NULL;
    const mi_extract_format *audio = NULL;
    const mi_extract_format *muxed = NULL;
    /*
     * The choice, in order of what the viewer asked for.
     *
     * `preferred_height` is a *ceiling*: a viewer asking for 720 wants to avoid
     * the larger file, so 1080 would defeat the point. The nearest height at or
     * below the request wins, and when nothing is that small the smallest
     * offered is taken rather than nothing.
     */
    int want = cfg->preferred_height;
    /*
     * The pick, with a tie-break: at the same height, a format that can actually
     * be fetched beats one that cannot.
     *
     * YouTube lists its highest renditions as HLS variants (protocol
     * `m3u8_native`), and choosing by height alone picked exactly those — a URL
     * that is a playlist, not media. Preferring a direct format at equal height
     * costs nothing where both exist and avoids the unfetchable one where they
     * do not. It only decides a *tie*; a taller direct format still wins on
     * height, and a direct format is never chosen over a taller one.
     */
    for (size_t i = 0; i < er.format_count; i++) {
        const mi_extract_format *f = &er.formats[i];
        if (f->has_video && f->has_audio) {
            if (want > 0) {
                /* A muxed format is only usable if it fits the ceiling, or is
                 * the best there is. */
                if (!muxed) muxed = f;
                else if (f->height <= want && f->height > muxed->height) muxed = f;
                else if (muxed->height > want && f->height < muxed->height) muxed = f;
                else if (f->height == muxed->height && f->direct && !muxed->direct) muxed = f;
            } else if (!muxed || f->height > muxed->height) {
                muxed = f;
            } else if (f->height == muxed->height && f->direct && !muxed->direct) {
                muxed = f;
            }
        } else if (f->has_video) {
            if (want > 0) {
                if (!video) video = f;
                /* At or below the request, the largest such. */
                else if (f->height <= want && f->height > video->height) video = f;
                /* Nothing small enough yet: take the smallest above it. */
                else if (video->height > want && f->height < video->height) video = f;
                else if (f->height == video->height && f->direct && !video->direct) video = f;
            } else if (!video || f->height > video->height) {
                video = f;
            } else if (f->height == video->height && f->direct && !video->direct) {
                video = f;
            }
        } else if (f->has_audio) {
            if (!audio || f->bitrate > audio->bitrate) audio = f;
        }
    }
    /* A muxed format that misses the ceiling is only worth taking when there is
     * no separate video to pair with the audio. */
    if (muxed && want > 0 && muxed->height > want && video) muxed = NULL;

    if (!muxed && !video) {
        mi_extract_result_free(&er);
        mi_logf(MI_LOG_ERROR, "the extractor found no video stream");
        return MI_ERR_ARG;
    }

    const char *ffmpeg = cfg->ffmpeg_path ? cfg->ffmpeg_path : "ffmpeg";
    const char *argv[32];
    int a = 0;
    argv[a++] = ffmpeg;
    argv[a++] = "-nostdin";
    argv[a++] = "-loglevel";
    argv[a++] = "error";
    /*
     * Cookies, when the site needs a sign-in.
     *
     * ffmpeg does the fetching, so it is ffmpeg that must send them — the
     * extractor's cookies only got the *resolve* past the sign-in wall. ffmpeg
     * wants newline-delimited `Set-Cookie` values, not a Netscape file, so the
     * conversion is made here from the same input.
     */
    char *ff_cookies = mi_extract_cookies_for_ffmpeg(cfg->cookies_text);
    if (ff_cookies) {
        argv[a++] = "-cookies";
        argv[a++] = ff_cookies;
    }
    /* The same egress the resolve used: ffmpeg does the fetch, so a proxy that
     * got the resolve past a refusal must carry the transfer too. */
    if (cfg->proxy && *cfg->proxy) {
        argv[a++] = "-http_proxy";
        argv[a++] = cfg->proxy;
    }
    argv[a++] = "-y";
    argv[a++] = "-i";
    argv[a++] = muxed ? muxed->url : video->url;
    /* The second input is the audio, when the video did not carry it. */
    if (!muxed && audio) {
        argv[a++] = "-i";
        argv[a++] = audio->url;
    }
    /* Copy both streams: they are already encoded, so this is a mux, not an
     * encode — which is why it takes seconds rather than the length of the
     * video. */
    argv[a++] = "-c";
    argv[a++] = "copy";
    /* Map explicitly: with two inputs, ffmpeg's default picks one stream from
     * each, and being explicit is what stops a video-only file being produced. */
    if (!muxed && audio) {
        argv[a++] = "-map";
        argv[a++] = "0:v:0";
        argv[a++] = "-map";
        argv[a++] = "1:a:0";
    }
    argv[a++] = "-f";
    argv[a++] = "mp4";
    argv[a++] = dest;
    argv[a] = NULL;

    mi_logf(MI_LOG_INFO, "download: muxing %zu stream%s with ffmpeg",
            er.format_count, er.format_count == 1 ? "" : "s");
    int child_err = -1;
    pid_t pid = spawn_relayed((const char *const *)argv, &child_err);
    if (pid < 0) {
        mi_extract_result_free(&er);
        return MI_ERR_IO;
    }

    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            if (child_err >= 0) close(child_err);
            mi_extract_result_free(&er);
            return MI_ERR_IO;
        }
        /* ffmpeg's own reading of the stream — the muxer's warnings and the
         * reason a copy failed ("Codec 'ac3' is not supported by the bitstream
         * filter", "Invalid data found when processing input") are all here. */
        relay_child_output(child_err);
        /* A pause must be honoured here too: ffmpeg is doing the transfer, so
         * the downloader's own stop callback never runs. The child is killed and
         * the partial file discarded — a muxed stream cannot resume. */
        if (cfg->control && cfg->control->stop && cfg->control->stop(cfg->control->user)) {
            kill(pid, SIGTERM);
            struct timespec grace = {.tv_sec = 2, .tv_nsec = 0};
            nanosleep(&grace, NULL);
            if (waitpid(pid, &status, WNOHANG) != pid) kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            if (child_err >= 0) close(child_err);
            mi_extract_result_free(&er);
            unlink(dest);
            return MI_ERR_ABORTED;
        }
        if (on_progress) {
            mi_progress p;
            memset(&p, 0, sizeof p);
            p.total_bytes = -1;
            p.downloaded_bytes = mi_file_size(dest);
            on_progress(&p, user);
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 500 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    relay_child_output(child_err);
    if (child_err >= 0) close(child_err);
    mi_extract_result_free(&er);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        mi_logf(MI_LOG_ERROR, "ffmpeg could not mux the streams (exit %d)",
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return MI_ERR_NET;
    }
    int64_t written = mi_file_size(dest);
    if (written <= 0) return MI_ERR_IO;
    out->bytes_written = written;
    mi_detect_file(dest, MI_HEADER_SNIFF, &out->media);
    return MI_OK;
}

/*
 * Sum the bytes of every regular file under `dir`, recursively, and remember the
 * largest.
 *
 * A torrent is written into a *directory named after the torrent* (multi-file)
 * or as a single file beside the work dir, so the scan must descend — a flat
 * readdir of the work dir sees one subdirectory and reports zero bytes, which is
 * exactly the bug this fixes.
 *
 * `largest` is the biggest file found, which is the media for a media torrent.
 * `*.aria2` control files are skipped: they are the client's bookkeeping, not
 * content.
 */
static int64_t scan_torrent_dir(const char *dir, char **largest) {
    int64_t total = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        size_t n = strlen(ent->d_name);
        if (n > 6 && strcmp(ent->d_name + n - 6, ".aria2") == 0) continue;
        char *full = mi_path_join(dir, ent->d_name);
        if (!full) continue;
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            int64_t sub = scan_torrent_dir(full, largest);
            total += sub;
            free(full);
            continue;
        }
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            total += st.st_size;
            if (largest && st.st_size > mi_file_size(*largest ? *largest : "")) {
                free(*largest);
                *largest = full;
                continue;
            }
        }
        free(full);
    }
    closedir(d);
    return total;
}

/*
 * Download a magnet link with a BitTorrent client.
 *
 * A magnet names a swarm, not a file: there is no HTTP object to range and no
 * manifest to mux, so the bytes come from peers and the only tool that can fetch
 * them is a BitTorrent client. This is the same division of labour as ffmpeg for
 * a manifest — Vyzor decides *what* to fetch, the specialist tool does the
 * fetching.
 *
 * The swarm is allowed to settle briefly before the transfer is judged, because
 * a magnet has no metadata until a peer supplies it: a client that gives up in
 * the first seconds reports failure for a torrent that is merely slow to find a
 * peer. It is stopped once the *first* file is complete, so the caller gets a
 * playable file rather than waiting for a multi-gigabyte whole-swarm download.
 *
 * The completed file is the largest one the client wrote, which is the video for
 * a media torrent and the only sensible default when it is a set.
 */
static int download_magnet(const mi_download_config *cfg, const char *magnet,
                           const char *dest, mi_progress_fn on_progress, void *user,
                           mi_download_result *out) {
    const char *aria2 = (cfg->magnet_downloader_path && *cfg->magnet_downloader_path)
                            ? cfg->magnet_downloader_path
                            : "aria2c";

    /* A private work directory, so the files the client writes are unambiguous
     * and can be swept afterwards. Removed on every exit path below. */
    char *work = mi_asprintf("%s.magnet", dest);
    if (!work) return MI_ERR_NOMEM;
    mi_rm_rf(work);
    if (mi_mkdir_p(work) != 0) {
        free(work);
        return MI_ERR_IO;
    }

    const char *argv[32];
    int a = 0;
    argv[a++] = aria2;
    /*
     * aria2's own events are *relayed into Vyzor's log*, not discarded.
     *
     * Silencing the client completely made a stalled torrent indistinguishable
     * from a working one: the only evidence was a byte count that never moved,
     * with nothing saying whether peers had been found, whether metadata had
     * arrived, or whether the tracker had refused. So the notices are kept and
     * read off the relay pipe below.
     *
     * `--quiet` is deliberately NOT set: it suppresses the console output
     * entirely (measured — with it, nothing at all is emitted), which would leave
     * the relay with nothing to relay. `--summary-interval` gives a progress line
     * every few seconds instead of a repainted bar, and the log level keeps it to
     * lines worth reading.
     */
    argv[a++] = "--summary-interval=5";
    argv[a++] = "--console-log-level=notice";
    /* Stop as soon as the first file is complete, then seed nothing. A media
     * torrent's first file is the video; waiting for the rest is minutes of
     * upload for no benefit to the caller. */
    argv[a++] = "--seed-time=0";
    argv[a++] = "--bt-stop-timeout=0";
    /* A short listen port window; the client may not be able to accept inbound
     * connections in a container, which is fine for a well-seeded swarm. */
    argv[a++] = "--enable-dht=true";
    argv[a++] = "--bt-enable-lpd=true";
    argv[a++] = "--follow-torrent=true";
    if (cfg->proxy && *cfg->proxy) {
        argv[a++] = "--all-proxy";
        argv[a++] = cfg->proxy;
    }
    /*
     * Which files to take, when the caller chose. aria2 takes a comma-separated
     * list of 1-based indices; a caller that opened a `.torrent` knows the file
     * list and can ask for one film rather than the whole season pack beside it.
     */
    char select_buf[256];
    if (cfg->magnet_files && cfg->magnet_file_count > 0) {
        size_t o = 0;
        for (size_t i = 0; i < cfg->magnet_file_count && o < sizeof select_buf - 12; i++) {
            if (cfg->magnet_files[i] <= 0) continue;
            o += (size_t)snprintf(select_buf + o, sizeof select_buf - o, "%s%d",
                                  o ? "," : "", cfg->magnet_files[i]);
        }
        if (o > 0) {
            argv[a++] = "--select-file";
            argv[a++] = select_buf;
        }
    }
    /*
     * The `.torrent` bytes, written to a file for the client.
     *
     * A `.torrent` is handed over as a *file*, not as a magnet, because it
     * carries the webseeds (`url-list`) and the piece hashes — and an archive.org
     * torrent, for one, has no peers at all and downloads only from its webseed.
     * Converting it to a magnet (which has neither) fetched zero bytes while the
     * file itself fetched the whole thing. So the file is what the client gets;
     * the magnet is only what the record shows.
     */
    char *torrent_path = NULL;
    if (cfg->torrent_bytes && cfg->torrent_len > 0) {
        torrent_path = mi_asprintf("%s/in.torrent", work);
        if (torrent_path) {
            FILE *tf = fopen(torrent_path, "wb");
            if (tf) {
                if (fwrite(cfg->torrent_bytes, 1, cfg->torrent_len, tf) != cfg->torrent_len) {
                    fclose(tf);
                    free(torrent_path);
                    torrent_path = NULL;
                } else {
                    fclose(tf);
                }
            } else {
                free(torrent_path);
                torrent_path = NULL;
            }
        }
    }

    argv[a++] = "--dir";
    argv[a++] = work;
    argv[a++] = torrent_path ? torrent_path : magnet;
    argv[a] = NULL;

    mi_logf(MI_LOG_INFO, "download: fetching the torrent with %s (%s)", aria2,
            torrent_path ? "from the .torrent file" : "from the magnet");
    mi_logf(MI_LOG_INFO,
            "download: %s; %s; budget %.0fs; work dir %s",
            cfg->torrent_bytes && cfg->torrent_len > 0
                ? "the .torrent file was handed to the client (webseeds included)"
                : "a magnet was handed to the client (no webseeds in a magnet)",
            cfg->magnet_files && cfg->magnet_file_count > 0
                ? "a file selection was requested"
                : "the whole torrent was requested",
            cfg->torrent_timeout_seconds > 0 ? cfg->torrent_timeout_seconds : 900.0,
            work);

    /*
     * A pipe on the child's stderr, so aria2's own events reach Vyzor's log.
     *
     * Its stdout is dropped (it prints nothing useful there) but stderr carries
     * the lines that explain a stall — "download aborted", "no peer", a tracker
     * error. Reading them is what turns "0 bytes for 100 seconds" into a reason.
     */
    int errpipe = -1;
    pid_t pid = spawn_relayed((const char *const *)argv, &errpipe);
    if (pid < 0) {
        mi_rm_rf(work);
        free(work);
        free(torrent_path);
        return MI_ERR_IO;
    }

    /*
     * Poll until the first file is complete, the client exits, or the deadline
     * passes. The deadline is generous because a swarm can be slow to hand over
     * metadata, but it is a deadline: a magnet with no peers must fail rather
     * than hold a worker forever.
     */
    /*
     * The swarm deadline, which is its own clock.
     *
     * A torrent may spend a minute finding peers before a byte arrives, so the
     * HTTP timeout (30 s) is wrong here — reusing it made every torrent fail at
     * 30 seconds. A torrent gets its own, generous by default.
     */
    double torrent_budget = cfg->torrent_timeout_seconds > 0
                                ? cfg->torrent_timeout_seconds
                                : 900.0;
    double deadline = mi_now() + torrent_budget;
    int64_t seen_bytes = 0;
    /* Carries a partial line between drains, so a long aria2 line is not split
     * into two log entries (see relay_drain). */
    char relay_carry[MI_LOG_LINE_CAP];
    size_t relay_carrylen = 0;
    int status = 0;
    int rc = MI_OK;
    int finished = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            finished = 1;
            break;
        }
        if (r < 0 && errno != EINTR) break;

        if (cfg->control && cfg->control->stop && cfg->control->stop(cfg->control->user)) {
            kill(pid, SIGTERM);
            struct timespec grace = {.tv_sec = 2, .tv_nsec = 0};
            nanosleep(&grace, NULL);
            if (waitpid(pid, &status, WNOHANG) != pid) kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            rc = MI_ERR_ABORTED;
            break;
        }
        if (mi_now() > deadline) {
            mi_logf(MI_LOG_INFO, "download: the torrent did not finish in %.0fs",
                    torrent_budget);
            kill(pid, SIGTERM);
            struct timespec grace = {.tv_sec = 2, .tv_nsec = 0};
            nanosleep(&grace, NULL);
            if (waitpid(pid, &status, WNOHANG) != pid) kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            rc = MI_ERR_NET;
            break;
        }

        /*
         * Relay whatever aria2 has said since the last turn.
         *
         * This is the whole point: a torrent that finds no peers prints "no peer
         * found" and a tracker error, and those lines are the difference between
         * "0 bytes" and a reason. Read non-blocking so the loop keeps its cadence.
         */
        relay_drain(errpipe, relay_carry, &relay_carrylen, sizeof relay_carry, 0);

        /*
         * Report progress from the bytes on disk. The scan descends, because a
         * multi-file torrent is written into a subdirectory named after it, and
         * only logs when the figure *changes* — a line a second for an unchanged
         * count is noise, and the interesting event is the change.
         */
        int64_t now_bytes = scan_torrent_dir(work, NULL);
        if (now_bytes != seen_bytes) {
            mi_logf(MI_LOG_INFO, "download: torrent %lld bytes on disk so far",
                    (long long)now_bytes);
            seen_bytes = now_bytes;
        }
        if (on_progress) {
            mi_progress p;
            memset(&p, 0, sizeof p);
            p.total_bytes = -1;
            p.downloaded_bytes = seen_bytes;
            on_progress(&p, user);
        }
        struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
        nanosleep(&ts, NULL);
    }

    /* Flush: the child has exited, so a partial line is complete now. */
    relay_drain(errpipe, relay_carry, &relay_carrylen, sizeof relay_carry, 1);
    if (errpipe >= 0) close(errpipe);

    if (rc == MI_OK && finished) {
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            mi_logf(MI_LOG_INFO, "download: %s exited %d on the torrent", aria2,
                    WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            rc = MI_ERR_NET;
        }
    }

    /*
     * How complete is it? aria2 writes **one** control file per *torrent*, named
     * `<torrent-name>.aria2` and placed *inside* the download directory — not one
     * per file, and not beside the directory. So completeness is a torrent-level
     * fact, and the test is: is there any `*.aria2` file still in the work dir?
     */
    int control_gone = 1;
    {
        DIR *d = opendir(work);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                size_t n = strlen(ent->d_name);
                if (n > 6 && strcmp(ent->d_name + n - 6, ".aria2") == 0) {
                    control_gone = 0;
                    break;
                }
            }
            closedir(d);
        }
    }

    /*
     * The largest file the client wrote, which is the media for a media torrent.
     * Found by a *recursive* scan: a multi-file torrent is written into a
     * subdirectory named after the torrent, so a flat readdir of the work dir
     * would see only that directory and pick nothing.
     */
    char *produced = NULL;
    scan_torrent_dir(work, &produced);

    if (!produced) {
        mi_logf(MI_LOG_ERROR, "download: the magnet produced no file "
                              "(no peers, or the torrent is not a single file)");
        mi_rm_rf(work);
        free(work);
        free(torrent_path);
        return rc == MI_OK ? MI_ERR_NET : rc;
    }

    /*
     * A partial file is not a result.
     *
     * Completeness is judged by the *control file*, not by the exit code: aria2
     * removes `<torrent>.aria2` only once every wanted piece has arrived and
     * verified, so its presence means the bytes on disk are incomplete. This is
     * checked whatever `rc` is — a client killed at the deadline exits non-zero
     * *and* leaves the control file, and delivering its partial bytes as success
     * is the failure this guards against. The partial data is left in place so a
     * retry resumes rather than restarts.
     */
    if (!control_gone) {
        mi_logf(MI_LOG_ERROR,
                "download: the torrent is still incomplete (%lld bytes so far, "
                "control file present) — no peers, or the swarm is slow",
                (long long)mi_file_size(produced));
        free(produced);
        mi_rm_rf(work);
        free(work);
        free(torrent_path);
        return MI_ERR_NET;
    }


    if (rename(produced, dest) != 0) {
        int crc = copy_file_bytes(produced, dest);
        unlink(produced);
        if (crc != 0) {
            free(produced);
            mi_rm_rf(work);
            free(work);
            free(torrent_path);
            return MI_ERR_IO;
        }
    }
    free(produced);
    mi_rm_rf(work);
    free(work);
    free(torrent_path);

    int64_t written = mi_file_size(dest);
    if (written <= 0) return MI_ERR_IO;
    out->bytes_written = written;
    mi_detect_file(dest, MI_HEADER_SNIFF, &out->media);
    return MI_OK;
}

static int mi_download_once(const mi_download_config *cfg, const char *url,
                            const char *dest, mi_progress_fn on_progress, void *user,
                            mi_download_result *out, int *status_out,
                            mi_block_kind *block_out) {
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

    /*
     * A magnet link is not an HTTP object, so it is handed to the BitTorrent
     * client *before* the probe. Probing it would ask libcurl to open a scheme it
     * does not speak and fail with a network error that names neither the tool
     * nor the reason.
     */
    if (strncasecmp(url, "magnet:?", 8) == 0) {
        const char *downloader = (cfg->magnet_downloader_path && *cfg->magnet_downloader_path)
                                     ? cfg->magnet_downloader_path
                                     : "aria2c";
        if (!mi_which(downloader)) {
            mi_logf(MI_LOG_ERROR,
                    "download: %s is not installed, so a magnet link cannot be fetched",
                    downloader);
            mi_download_result_free(out);
            return MI_ERR;
        }
        return download_magnet(cfg, url, dest, on_progress, user, out);
    }

    mi_download_plan plan;
    int rc = mi_probe(cfg, url, &plan);
    if (status_out) *status_out = plan.status;
    if (block_out) *block_out = plan.block;
    if (rc != MI_OK) {
        mi_download_result_free(out);
        return rc;
    }

    int recognised = plan.media.is_video || plan.media.is_image || plan.media.is_audio;
    /*
     * A page the extractor knows is asked *before* the payload is judged.
     *
     * The media check refuses anything the detector does not recognise, and a
     * streaming page is an HTML document — so a watch page was refused here,
     * before the extractor was ever consulted, and the download failed with no
     * log line and no explanation. The check is right for a URL that is *meant*
     * to be a file; it must not pre-empt the case where the URL is a page whose
     * media lives somewhere else.
     */
    if (cfg->extract_pages && !recognised) {
        /*
         * Which tool will be asked, logged because the choice is by host and a
         * wrong one is otherwise invisible — the user sees "download failed" and
         * nothing says the gallery extractor was handed a video site.
         */
        mi_extract_config xcfg;
        mi_extract_config_init(&xcfg);
        xcfg.binary = cfg->extractor_path;
        xcfg.gallery_binary = cfg->gallery_extractor_path;
        xcfg.torrent_binary = cfg->torrent_extractor_path;
        mi_logf(MI_LOG_INFO, "download: extractor %s for %s",
                mi_extract_kind_name(mi_extract_pick(&xcfg, url)), url);
        int src = download_stream_page(cfg, plan.final_url ? plan.final_url : url, dest,
                                       on_progress, user, out);
        mi_logf(MI_LOG_INFO, "download: the extractor returned %d", src);
        if (src == MI_OK) {
            mi_download_plan_free(&plan);
            return MI_OK;
        }
        /*
         * The extractor failed, and the URL is a *page*.
         *
         * Falling through here would copy the HTML and report success — a
         * 3,271-byte `<!DOCTYPE html>` saved as the video, `state: done`. That is
         * the worst shape of failure: it looks right, and the user only finds out
         * when they open the file. A URL that probed as a page, that the
         * extractor was asked about and could not resolve, is a URL whose media
         * is not available — so it is reported as a failure with the reason.
         *
         * A URL with real bytes still falls through: the extractor not knowing a
         * site says nothing about a URL that is plainly a file.
         */
        if (!recognised && plan.total_bytes <= 0) {
            mi_logf(MI_LOG_ERROR,
                    "the page could not be resolved to media, and it is not a file "
                    "(the extractor may be rate-limited or out of date)");
            mi_download_plan_free(&plan);
            mi_download_result_free(out);
            return MI_ERR_NET;
        }
        /* Not a page the extractor knows. Fall through, so a refusal below is
         * reported with its own reason rather than as a failed extraction. */
    }

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

    /*
     * A manifest is read by ffmpeg, not by the ranged or streaming paths.
     *
     * Those two treat the URL as an object to copy, and an HLS/DASH URL is a
     * *description* of many objects — so copying it saved the playlist. This
     * branch is what turns "download this stream" into the video.
     */
    if (plan.have_media &&
        (plan.media.kind == MI_KIND_HLS || plan.media.kind == MI_KIND_DASH)) {
        rc = download_manifest(cfg, plan.final_url ? plan.final_url : url, dest,
                               on_progress, user, out);
    } else if (plan.accept_ranges && plan.total_bytes > 0) {
        rc = download_ranged(cfg, plan.final_url ? plan.final_url : url,
                             plan.total_bytes, dest, on_progress, user, out,
                             plan.have_media ? &plan.media : NULL);
    } else {
        rc = download_stream(cfg, plan.final_url ? plan.final_url : url, dest,
                             on_progress, user, out,
                             plan.have_media ? &plan.media : NULL);
    }
    mi_download_plan_free(&plan);
    if (out->block == MI_BLOCK_NONE) out->block = (block_out ? *block_out : MI_BLOCK_NONE);
    if (rc != MI_OK) mi_download_result_free(out);
    return rc;
}

/* The identity a strategy asks for, applied to `c`. */
static void apply_strategy(mi_strategy strat, const mi_download_config *cfg,
                           mi_identity_pool *pool, mi_antibot *brain, int attempt,
                           mi_download_config *c) {
    *c = *cfg;
    switch (strat) {
        case MI_STRATEGY_IMPERSONATE:
            /* Wear a browser's TLS fingerprint and the coherent headers that go
             * with it. */
            c->impersonate = (cfg->impersonate && *cfg->impersonate)
                                 ? cfg->impersonate : "chrome";
            break;
        case MI_STRATEGY_ROTATE_UA: {
            mi_identity id = mi_identity_for_attempt(pool, attempt);
            if (id.user_agent) c->user_agent = id.user_agent;
            break;
        }
        case MI_STRATEGY_ROTATE_PROXY: {
            const char *px = mi_antibot_next_proxy(brain);
            if (px) c->proxy = px;
            break;
        }
        case MI_STRATEGY_COOKIES:
            /* The cookies are already in cfg; the move is to *use* them, which
             * they are by default. Nothing extra to set. */
            break;
        case MI_STRATEGY_WAIT:
        default:
            /* Same configuration, retried. */
            break;
    }
}

int mi_download(const mi_download_config *cfg, const char *url, const char *dest,
                mi_progress_fn on_progress, void *user,
                mi_download_result *out) {
    if (!cfg || !url || !dest || !out) return MI_ERR_ARG;

    /*
     * A refusal is answered with the move its *kind* calls for.
     *
     * The old loop rotated a User-Agent on every failure, whatever the failure
     * was — which is wasted work against a TLS-fingerprint block and makes a
     * rate limit worse. The brain names the block and picks the strategy, and
     * remembers the identity that worked so a later request to the same host
     * starts from it rather than rediscovering it.
     */
    mi_identity_pool *pool = mi_identity_pool_new(cfg->user_agent, cfg->proxy,
                                                  cfg->impersonate, cfg->rotate_identity);
    mi_antibot *brain = cfg->brain;
    int own_brain = 0;
    if (!brain) {
        brain = mi_antibot_new();
        own_brain = 1;
    }
    char *host = mi_antibot_host_of(url);

    /* What this Vyzor can actually do. The download path has no browser and no
     * solver, so those rungs are skipped rather than attempted. */
    mi_antibot_caps caps = {0};
    {
        char *helper = NULL;
        caps.can_impersonate = mi_impersonate_available(NULL, &helper);
        free(helper);
    }
    caps.can_rotate_ua = 1;
    caps.proxy_count = (int)mi_identity_proxy_count(pool);
    caps.have_cookies = cfg->cookies_text && *cfg->cookies_text;

    /* The identity that worked for this host before, reused first. */
    char *sticky_proxy = NULL, *sticky_ua = NULL, *sticky_imp = NULL;
    if (host) {
        mi_site_identity id;
        if (mi_antibot_recall_identity(brain, host, &id)) {
            sticky_proxy = id.proxy;
            sticky_ua = id.user_agent;
            sticky_imp = id.impersonate;
        }
    }

    int attempts = cfg->identity_retries > 0 ? cfg->identity_retries + 1 : 1;
    int rc = MI_ERR_NET;
    mi_block_kind block = MI_BLOCK_NONE;
    mi_strategy used = MI_STRATEGY_DIRECT;

    for (int attempt = 0; attempt < attempts; attempt++) {
        mi_download_config c = *cfg;
        if (attempt == 0) {
            /* The caller's own configuration, or the identity that already
             * worked for this host. */
            if (sticky_proxy) c.proxy = sticky_proxy;
            if (sticky_imp) c.impersonate = sticky_imp;
            if (sticky_ua) c.user_agent = sticky_ua;
        } else {
            int wait = 0;
            mi_strategy strat = mi_antibot_next(brain, host, block, &caps, &wait);
            if (strat == MI_STRATEGY_GIVE_UP) {
                mi_logf(MI_LOG_INFO, "download: no move left for a %s block",
                        mi_antibot_kind_name(block));
                break;
            }
            used = strat;
            mi_logf(MI_LOG_INFO, "download: %s block; trying %s",
                    mi_antibot_kind_name(block), mi_antibot_strategy_name(strat));
            if (strat == MI_STRATEGY_WAIT && wait > 0) {
                mi_logf(MI_LOG_INFO, "download: waiting %ds for %s", wait,
                        host ? host : "the host");
                usleep((useconds_t)(wait * 1000000));
            }
            apply_strategy(strat, cfg, pool, brain, attempt, &c);
        }

        int status = 0;
        mi_block_kind got = MI_BLOCK_NONE;
        rc = mi_download_once(&c, url, dest, on_progress, user, out, &status, &got);
        if (rc == MI_OK) {
            if (host) {
                mi_antibot_note_success(brain, host);
                /* Pin the winning identity so the next request reuses it. */
                mi_antibot_remember_identity(brain, host, used, c.proxy, c.user_agent,
                                             c.impersonate);
            }
            break;
        }
        if (rc == MI_ERR_ABORTED || rc == MI_ERR_ARG) break;

        /* A failure with no classified block is a plain network error; treat it
         * as unknown so the ladder still offers a gentle escalation. */
        block = (got == MI_BLOCK_NONE) ? MI_BLOCK_UNKNOWN : got;
        if (host) mi_antibot_note_block(brain, host, block);
        /* A proxy that was refused is parked, so the next rotation does not
         * immediately reuse it. */
        if (c.proxy && block != MI_BLOCK_NONE) mi_antibot_park_proxy(brain, c.proxy, 60);
    }

    free(sticky_proxy);
    free(sticky_ua);
    free(sticky_imp);
    free(host);
    mi_identity_pool_free(pool);
    if (own_brain) mi_antibot_free(brain);
    return rc;
}
