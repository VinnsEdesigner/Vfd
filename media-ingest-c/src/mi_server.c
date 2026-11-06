#define _GNU_SOURCE
#include "mi_server.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "mi_browse.h"
#include "mi_crawl.h"
#include "mi_detect.h"
#include "mi_extract.h"
#include "mi_download.h"
#include "mi_hls.h"
#include "mi_identify.h"
#include "mi_outbox.h"
#include "mi_interactive.h"
#include "mi_json.h"
#include "mi_render.h"
#include "mi_rtc.h"
#include "mi_sanitize.h"
#include "mi_util.h"
#include "mi_ws.h"

/* ------------------------------------------------------------------ */
/* download model                                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    DOWNLOAD_QUEUED,
    DOWNLOAD_RUNNING,
    DOWNLOAD_DONE,
    DOWNLOAD_FAILED,
    DOWNLOAD_CANCELLED,
    /* Paused: the transfer was stopped deliberately and can be resumed from
     * what is already on disk. Distinct from cancelled, which is final. */
    DOWNLOAD_PAUSED
} download_state;

typedef struct {
    char id[40];
    char *url;
    char *out_dir;
    char *hls_dir;
    char *prefix;
    int do_hls;
    int do_upload;
    int do_purge;
    int connections;
    /* The options the job was created with, echoed back so the UI can say which
     * stages this download actually runs instead of assuming. */
    char *requested_quality;

    download_state state;
    double created_at;
    double started_at;
    double finished_at;

    int64_t bytes;
    int64_t total_bytes;
    int64_t downloaded;
    /* Live transfer rate, refreshed by the progress callback. */
    double bytes_per_second;
    double rate_sample_at;
    int64_t rate_sample_bytes;

    char *media_kind;
    char *container;
    char *source_path; /* the file as written, extension included */
    /* What the item is called in the UI and on disk. NULL means "derive it from
     * the URL", which keeps the record honest for records created before
     * renaming existed. */
    char *display_name;
    int media_is_video;
    int media_is_image;
    int media_is_audio;
    int media_width;
    int media_height;
    /* Set by a pause request; read by the running worker's control callback. */
    volatile int pause_requested;
    /* 1 while a worker owns the record, so the server never starts a second. */
    int worker_active;
    /* Accept a payload the detector does not recognise (a PDF, a text file).
     * Off by default: the downloader is for media, and refusing an unexpected
     * payload early is the safer default. The API lets a caller opt in. */
    int allow_non_media;
    char *sha256;
    char *master_playlist;
    /* Conversion of a container the browser cannot play (MKV, AVI) into MP4.
     * `converting` is 1 while a job runs, `converted` is 1 once the MP4 is on
     * disk, and `convert_error` explains a failure. */
    volatile int converting;
    volatile int converted;
    /* Counts conversion threads still holding this record. A delete waits on it
     * the same way it waits on `worker_active`, so a converter can never write
     * through a freed record. */
    volatile int convert_active;
    char *converted_path;
    char *convert_error;
    char *error;
    char **artifact_urls;
    size_t artifact_count;
} mi_download_rec;

/* The download's storage settings are snapshotted at creation so a later config
 * change cannot alter an in-flight download. */
typedef struct {
    char *endpoint;
    char *region;
    char *bucket;
    char *access_key;
    char *secret_key;
    char *session_token;
    char *cdn_base;
    char *cf_token;
    char *cf_zone;
    int path_style;
} storage_snapshot;

struct mi_server {
    mi_server_config cfg;
    /* The directory downloads are written to. Defaults to cfg.work_dir but can
     * be changed at runtime (PUT /api/v1/storage/location), so every path that
     * derives an item's directory reads this rather than cfg.work_dir. */
    char *download_dir;
    int listen_fd;
    volatile sig_atomic_t stop;
    pthread_mutex_t lock;
    mi_download_rec **downloads;
    size_t download_count;
    pthread_t *threads;
    size_t thread_count;
    /* The page renderer and its concurrency gate, or NULL when rendering is
     * off. Owned here because the limit is a property of the process: the
     * server spawns a thread per connection with no cap of its own, so an
     * expensive endpoint has to bring its own. */
    mi_render_pool *renderer;
    /* Live, driveable page sessions. Same reasoning: its own pool, its own
     * gate, and NULL when the feature is off. */
    mi_interactive_pool *driver;
    /* Live video over WebRTC: raw capture from an X display instead of a
     * stream of JPEG frames. Its own pool and gate again, because it holds a
     * browser *and* an X server for as long as it runs. */
    mi_rtc_pool *rtc;
};

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void gen_id(char out[40]) {
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[16];
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(raw, 1, sizeof raw, f) != sizeof raw) {
            for (size_t i = 0; i < sizeof raw; i++) raw[i] = (unsigned char)rand();
        }
        fclose(f);
    } else {
        for (size_t i = 0; i < sizeof raw; i++) raw[i] = (unsigned char)rand();
    }
    for (int i = 0; i < 16; i++) {
        out[i * 2] = hex[raw[i] >> 4];
        out[i * 2 + 1] = hex[raw[i] & 0xF];
    }
    out[32] = '\0';
}

static const char *download_state_name(download_state s) {
    switch (s) {
        case DOWNLOAD_QUEUED: return "queued";
        case DOWNLOAD_RUNNING: return "running";
        case DOWNLOAD_DONE: return "done";
        case DOWNLOAD_FAILED: return "failed";
        case DOWNLOAD_CANCELLED: return "cancelled";
        case DOWNLOAD_PAUSED: return "paused";
    }
    return "unknown";
}

/* ------------------------------------------------------------------ */
/* config                                                              */
/* ------------------------------------------------------------------ */

void mi_server_config_init(mi_server_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->host = "127.0.0.1";
    cfg->port = 8787;
    cfg->workers = 16;
    cfg->max_body_bytes = 64 * 1024 * 1024;
    cfg->work_dir = "./downloads";
    /*
     * Default to same-origin only. A server that answers every origin lets any
     * page a user visits read their library and start downloads, so allowing
     * one is something an operator opts into with --allow-origin.
     */
    cfg->allow_origins = NULL;
    cfg->auth_token = NULL;
    cfg->max_downloads = 200;
    /* NULL means Vyzor's own default agent. */
    cfg->user_agent = NULL;
    /*
     * Rendering is opt-in.
     *
     * It costs roughly half a gigabyte per render and needs a browser on the
     * host, so a deployment turns it on deliberately rather than discovering it
     * by accident. The numbers here are the measured defaults; the endpoint
     * reports them so a console can say what it is talking to.
     */
    cfg->render_enabled = 0;
    cfg->render_timeout_seconds = 20;
    cfg->render_max_concurrent = 1;
    cfg->render_v8_heap_mb = 256;
    cfg->render_viewport_width = 1280;
    cfg->render_viewport_height = 900;
    cfg->render_browser = NULL;
    cfg->render_profile_root = NULL;
    /* Caching on by default: a render is expensive, and the same page is often
     * looked at twice. 0 means "the built-in default" for each bound. */
    cfg->render_cache_dir = NULL; /* filled from the work dir when serving */
    cfg->render_cache_max_bytes = 0;
    cfg->render_cache_max_entries = 0;
    cfg->render_cache_ttl_seconds = 0;
    /*
     * Live sessions are opt-in and separate from rendering. A render is a
     * second-long subprocess; a live session is a browser held open for as long
     * as it is used, so it is a bigger commitment and a different decision.
     */
    cfg->interactive_enabled = 0;
    cfg->interactive_max_concurrent = 1;
    cfg->interactive_idle_seconds = 300;
    cfg->interactive_v8_heap_mb = 256;
    /*
     * The keepalive window, and its own knobs. The console sends an
     * application-level heartbeat, so this is the backstop for a client that has
     * genuinely gone rather than the only way to notice one. The built-in
     * defaults live in mi_interactive.c; these are 0 so they apply.
     */
    cfg->interactive_keepalive_ping_seconds = 0;
    cfg->interactive_keepalive_timeout_seconds = 0;
    /*
     * Live video over WebRTC. A third opt-in, and the most demanding: it needs
     * a browser on a real X display, so it costs an Xvfb and a Chromium per
     * session, plus the encode. Off unless asked for. 0 means the built-in
     * default in each case.
     */
    cfg->rtc_enabled = 0;
    cfg->rtc_max_concurrent = 1;
    cfg->rtc_idle_seconds = 180;
    cfg->rtc_v8_heap_mb = 256;
    cfg->rtc_width = 0;
    cfg->rtc_height = 0;
    cfg->rtc_fps = 0;
    cfg->rtc_bitrate_kbps = 0;
    cfg->rtc_stun_server = NULL;
    /*
     * The media extractor, for a site whose stream URL is not in the page.
     *
     * NULL means "yt-dlp" from PATH. Named rather than hardcoded so an operator
     * can point at their own build; a distribution's copy can lag a site's
     * changes by months, and this is the one component whose whole job is to
     * keep up with them.
     */
    cfg->extractor_path = NULL;
    cfg->gallery_extractor_path = NULL;
    /* NULL means "ffmpeg" from PATH. Named so an operator can point at a build
     * of their own; a distribution's copy can lag. */
    cfg->ffmpeg_path = NULL;
}

/* ------------------------------------------------------------------ */
/* HTTP plumbing                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char *name;
    char *value;
} http_header;

typedef struct {
    char method[8];
    char path[512];
    char query[512];
    char *body;
    size_t body_len;
    http_header *headers;
    size_t header_count;
} http_request;

static void request_free(http_request *req) {
    for (size_t i = 0; i < req->header_count; i++) {
        free(req->headers[i].name);
        free(req->headers[i].value);
    }
    free(req->headers);
    free(req->body);
    memset(req, 0, sizeof *req);
}

static const char *request_header(const http_request *req, const char *name) {
    for (size_t i = 0; i < req->header_count; i++) {
        if (!strcasecmp(req->headers[i].name, name)) return req->headers[i].value;
    }
    return NULL;
}

/* Does `origin` appear in the comma-separated allow list? "*" matches anything.
 * Comparison is case-insensitive and ignores surrounding spaces, because an
 * origin is a URL scheme+host and browsers differ on case. */
static int origin_allowed(const char *list, const char *origin) {
    if (!list || !origin) return 0;
    size_t olen = strlen(origin);
    for (const char *p = list; *p;) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && end[-1] == ' ') end--;
        size_t len = (size_t)(end - start);
        if (len == 1 && start[0] == '*') return 1;
        if (len == olen && !strncasecmp(start, origin, len)) return 1;
    }
    return 0;
}

/*
 * The CORS origin to echo for this request, or NULL to send no CORS headers.
 *
 * Echoing the request's own Origin (rather than "*") is required once a request
 * carries credentials: a browser rejects the wildcard for those. It also means
 * an origin that is not on the list is simply not echoed, so the browser blocks
 * the response.
 */
static const char *cors_origin_for(const mi_server *srv, const http_request *req) {
    const char *origin = request_header(req, "Origin");
    if (!origin || !*origin) return NULL;
    return origin_allowed(srv->cfg.allow_origins, origin) ? origin : NULL;
}

/* Read until we have the full head, then the declared body. Returns 0/-1. */
static int read_request(int fd, mi_server_config *cfg, http_request *req) {
    char buf[8192];
    size_t used = 0;
    char *head = NULL;
    size_t head_cap = 0;
    char *sep = NULL;

    memset(req, 0, sizeof *req);
    /* Accumulate until "\r\n\r\n". */
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) return -1;
        if (used + (size_t)n + 1 > head_cap) {
            head_cap = (used + (size_t)n + 1) * 2;
            char *grown = mi_realloc(head, head_cap);
            if (!grown) {
                free(head);
                return -1;
            }
            head = grown;
        }
        memcpy(head + used, buf, (size_t)n);
        used += (size_t)n;
        head[used] = '\0';
        sep = strstr(head, "\r\n\r\n");
        if (sep) break;
        if (used > 64 * 1024) { /* absurd header block */
            free(head);
            return -1;
        }
    }

    /* Request line. */
    char *line_end = strstr(head, "\r\n");
    if (!line_end) {
        free(head);
        return -1;
    }
    char line[1024];
    size_t llen = (size_t)(line_end - head);
    if (llen >= sizeof line) llen = sizeof line - 1;
    memcpy(line, head, llen);
    line[llen] = '\0';

    char target[1024] = {0};
    if (sscanf(line, "%7s %1023s", req->method, target) != 2) {
        free(head);
        return -1;
    }
    char *q = strchr(target, '?');
    if (q) {
        *q = '\0';
        snprintf(req->query, sizeof req->query, "%s", q + 1);
    }
    snprintf(req->path, sizeof req->path, "%s", target);

    /* Headers. */
    char *p = line_end + 2;
    while (p && *p && strncmp(p, "\r\n", 2) != 0) {
        char *eol = strstr(p, "\r\n");
        if (!eol) break;
        char *colon = memchr(p, ':', (size_t)(eol - p));
        if (colon) {
            http_header h;
            h.name = mi_strndup(p, (size_t)(colon - p));
            const char *v = colon + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            h.value = mi_strndup(v, (size_t)(eol - v));
            http_header *grown = mi_realloc(req->headers,
                                            (req->header_count + 1) * sizeof(http_header));
            if (grown) {
                req->headers = grown;
                req->headers[req->header_count++] = h;
            } else {
                free(h.name);
                free(h.value);
            }
        }
        p = eol + 2;
    }

    /* Body. */
    const char *cl = request_header(req, "Content-Length");
    size_t body_len = cl ? (size_t)strtoull(cl, NULL, 10) : 0;
    if (body_len > (size_t)cfg->max_body_bytes) {
        free(head);
        return -2; /* too large */
    }
    if (body_len > 0) {
        req->body = mi_malloc(body_len + 1);
        if (!req->body) {
            free(head);
            return -1;
        }
        size_t already = used - (size_t)(sep + 4 - head);
        size_t copied = already < body_len ? already : body_len;
        memcpy(req->body, sep + 4, copied);
        while (copied < body_len) {
            ssize_t n = recv(fd, req->body + copied, body_len - copied, 0);
            if (n <= 0) {
                free(head);
                return -1;
            }
            copied += (size_t)n;
        }
        req->body[body_len] = '\0';
        req->body_len = body_len;
    }
    free(head);
    return 0;
}

static void send_all(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += (size_t)n;
    }
}

static void send_response(int fd, int status, const char *status_text,
                          const char *content_type, const char *body,
                          size_t body_len, const char *cors) {
    char header[1024];
    int n = snprintf(header, sizeof header,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "%s%s%s"
                     "\r\n",
                     status, status_text, content_type, body_len,
                     cors ? "Access-Control-Allow-Origin: " : "",
                     cors ? cors : "",
                     cors ? "\r\n"
                            "Access-Control-Allow-Methods: GET,POST,PUT,PATCH,DELETE,OPTIONS\r\n"
                            "Access-Control-Allow-Headers: Content-Type,Authorization\r\n"
                            "Vary: Origin\r\n"
                          : "");
    send_all(fd, header, (size_t)n);
    if (body && body_len) send_all(fd, body, body_len);
}

static void send_json(int fd, int status, const char *status_text,
                      const mi_json_buf *b, const char *cors) {
    send_response(fd, status, status_text, "application/json",
                  mi_json_data(b), b->len, cors);
}

static void send_error(int fd, int status, const char *text, const char *message,
                       const char *cors) {
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"error\":");
    mi_json_string(&b, message);
    mi_json_raw(&b, ",\"status\":");
    mi_json_int(&b, status);
    mi_json_raw(&b, "}");
    send_json(fd, status, text, &b, cors);
    mi_json_free(&b);
}

/* The same, with a Retry-After, for a refusal that is temporary — a full
 * concurrency gate, say. The header is what tells a client to come back rather
 * than retry at once and make the congestion worse. `retry_after` of 0 omits it. */
static void send_error_retry(int fd, int status, const char *text, const char *message,
                             const char *cors, int retry_after) {
    if (retry_after <= 0) {
        send_error(fd, status, text, message, cors);
        return;
    }
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"error\":");
    mi_json_string(&b, message);
    mi_json_raw(&b, ",\"status\":");
    mi_json_int(&b, status);
    mi_json_raw(&b, ",\"retry_after\":");
    mi_json_int(&b, retry_after);
    mi_json_raw(&b, "}");

    const char *body = mi_json_data(&b);
    size_t blen = strlen(body);
    char header[1024];
    int hn = snprintf(header, sizeof header,
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %zu\r\n"
                      "Retry-After: %d\r\n"
                      "Cache-Control: no-store\r\n"
                      "%s%s%s"
                      "Connection: close\r\n\r\n",
                      status, text, blen, retry_after,
                      cors ? "Access-Control-Allow-Origin: " : "",
                      cors ? cors : "",
                      cors ? "\r\nVary: Origin\r\n" : "");
    send_all(fd, header, (size_t)hn);
    send_all(fd, body, blen);
    mi_json_free(&b);
}

/* ------------------------------------------------------------------ */
/* download execution                                                       */
/* ------------------------------------------------------------------ */

static mi_download_rec *download_new(const char *url) {
    mi_download_rec *j = mi_calloc(1, sizeof *j);
    if (!j) return NULL;
    gen_id(j->id);
    j->url = mi_strdup(url);
    j->state = DOWNLOAD_QUEUED;
    j->created_at = (double)time(NULL);
    j->connections = 8;
    return j;
}

static void download_free(mi_download_rec *j) {
    if (!j) return;
    free(j->url);
    free(j->out_dir);
    free(j->hls_dir);
    free(j->prefix);
    free(j->media_kind);
    free(j->container);
    free(j->source_path);
    free(j->display_name);
    free(j->requested_quality);
    free(j->sha256);
    free(j->master_playlist);
    free(j->converted_path);
    free(j->convert_error);
    free(j->error);
    for (size_t i = 0; i < j->artifact_count; i++) free(j->artifact_urls[i]);
    free(j->artifact_urls);
    free(j);
}

/* A hex digit's value, or -1 when it is not one. */
static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decode a URL path segment. A filename in a URL is percent-encoded
 * ("(" arrives as "%28"), so the bytes have to be decoded or the file on disk is
 * literally called "cat_%28aka%29.jpg", which no person would recognise and no
 * other app would match. A '+' is left alone: in a path segment it is a literal
 * plus, not a space (only a query string treats it as a space). */
static char *percent_decode(const char *s, size_t len) {
    char *out = mi_malloc(len + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '%' && i + 2 < len) {
            int hi = hex_value(s[i + 1]);
            int lo = hex_value(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[o++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out[o++] = s[i];
    }
    out[o] = '\0';
    return out;
}

static char *basename_from_url(const char *url) {
    if (!url) return mi_strdup("download");
    const char *q = strpbrk(url, "?#");
    size_t len = q ? (size_t)(q - url) : strlen(url);
    const char *end = url + len;
    const char *start = end;
    while (start > url && start[-1] != '/') start--;
    if (start == end) return mi_strdup("download");
    return percent_decode(start, (size_t)(end - start));
}

/* The name an item is shown under. A renamed record keeps its name; everything
 * else is named after its source URL, so the list never shows a raw id. */
static char *download_display_name(const mi_download_rec *j) {
    return j->display_name ? mi_strdup(j->display_name) : basename_from_url(j->url);
}

/* The on-disk filename for a record: its name with the detected kind's
 * extension. Defined below; declared here because the JSON serialiser needs it. */
static char *source_filename(const mi_download_rec *j);

/* File extension for a detected kind. */
static const char *extension_for_kind(const char *kind);

/* Serialise one download to JSON. Caller holds the server lock. */
static void download_to_json(const mi_download_rec *j, mi_json_buf *b) {
    mi_json_raw(b, "{");
    mi_json_key(b, "id");
    mi_json_string(b, j->id);
    mi_json_raw(b, ",");
    mi_json_key(b, "name");
    char *name = download_display_name(j);
    mi_json_string(b, name);
    mi_json_raw(b, ",");
    /* The name a client should save under. `name` is the label the user sees and
     * may have no extension — saving that produces a file no OS can open. This
     * one is always the display name with an extension: the kind's when the name
     * lacks one, otherwise the name exactly as given. */
    mi_json_key(b, "filename");
    char *fname = NULL;
    {
        char *base = download_display_name(j);
        if (base && !strrchr(base, '.')) {
            fname = mi_asprintf("%s.%s", base, extension_for_kind(j->media_kind));
        }
        if (!fname) fname = base;
        else free(base);
    }
    mi_json_string(b, fname ? fname : name);
    free(fname);
    free(name);
    mi_json_raw(b, ",");
    mi_json_key(b, "url");
    mi_json_string(b, j->url);
    mi_json_raw(b, ",");
    mi_json_key(b, "state");
    mi_json_string(b, download_state_name(j->state));
    mi_json_raw(b, ",");
    mi_json_key(b, "progress");
    mi_json_raw(b, "{");
    mi_json_key(b, "downloaded_bytes");
    mi_json_int(b, j->downloaded);
    mi_json_raw(b, ",");
    mi_json_key(b, "total_bytes");
    mi_json_int(b, j->total_bytes);
    mi_json_raw(b, ",");
    mi_json_key(b, "bytes_per_second");
    mi_json_double(b, j->bytes_per_second);
    mi_json_raw(b, "}");
    mi_json_raw(b, ",");
    mi_json_key(b, "media");
    if (j->media_kind) {
        mi_json_raw(b, "{");
        mi_json_key(b, "kind");
        mi_json_string(b, j->media_kind);
        mi_json_raw(b, ",");
        mi_json_key(b, "container");
        mi_json_string(b, j->container);
        mi_json_raw(b, ",");
        mi_json_key(b, "is_video");
        mi_json_bool(b, j->media_is_video);
        mi_json_raw(b, ",");
        mi_json_key(b, "is_image");
        mi_json_bool(b, j->media_is_image);
        mi_json_raw(b, ",");
        mi_json_key(b, "is_audio");
        mi_json_bool(b, j->media_is_audio);
        mi_json_raw(b, ",");
        mi_json_key(b, "width");
        mi_json_int(b, j->media_width);
        mi_json_raw(b, ",");
        mi_json_key(b, "height");
        mi_json_int(b, j->media_height);
        mi_json_raw(b, "}");
    } else {
        mi_json_null(b);
    }
    mi_json_raw(b, ",");
    mi_json_key(b, "bytes");
    mi_json_int(b, j->bytes);
    mi_json_raw(b, ",");
    mi_json_key(b, "sha256");
    mi_json_string(b, j->sha256);
    mi_json_raw(b, ",");
    mi_json_key(b, "master_playlist");
    mi_json_string(b, j->master_playlist);
    mi_json_raw(b, ",");
    /* Conversion state, so the UI can poll it and say what happened rather than
     * guessing from a missing file. */
    mi_json_key(b, "conversion");
    mi_json_raw(b, "{");
    mi_json_key(b, "state");
    mi_json_string(b,
                   j->converting ? "converting"
                   : j->converted ? "done"
                   : j->convert_error ? "failed"
                                      : "none");
    mi_json_raw(b, ",");
    mi_json_key(b, "error");
    if (j->convert_error) mi_json_string(b, j->convert_error);
    else mi_json_null(b);
    mi_json_raw(b, "}");
    mi_json_raw(b, ",");
    mi_json_key(b, "artifacts");
    mi_json_raw(b, "[");
    for (size_t i = 0; i < j->artifact_count; i++) {
        if (i) mi_json_raw(b, ",");
        mi_json_string(b, j->artifact_urls[i]);
    }
    mi_json_raw(b, "],");
    mi_json_key(b, "error");
    mi_json_string(b, j->error);
    mi_json_raw(b, ",");
    /* The options this job actually runs, so the UI can report them rather than
     * assume the current form values. */
    mi_json_key(b, "requested_quality");
    mi_json_string(b, j->requested_quality);
    mi_json_raw(b, ",");
    mi_json_key(b, "connections");
    mi_json_int(b, j->connections);
    mi_json_raw(b, ",");
    mi_json_key(b, "hls");
    mi_json_bool(b, j->do_hls);
    mi_json_raw(b, ",");
    mi_json_key(b, "upload");
    mi_json_bool(b, j->do_upload);
    mi_json_raw(b, ",");
    mi_json_key(b, "purge");
    mi_json_bool(b, j->do_purge);
    mi_json_raw(b, ",");
    mi_json_key(b, "allow_non_media");
    mi_json_bool(b, j->allow_non_media);
    mi_json_raw(b, ",");
    mi_json_key(b, "created_at");
    mi_json_double(b, j->created_at);
    mi_json_raw(b, ",");
    mi_json_key(b, "finished_at");
    mi_json_double(b, j->finished_at);
    mi_json_raw(b, "}");
}

/* Emit a media object with its flags and any parsed qualities. */
static void media_to_json(const mi_media_info *m, mi_json_buf *b) {
    mi_json_raw(b, "{\"kind\":");
    mi_json_string(b, mi_kind_name(m->kind));
    mi_json_raw(b, ",\"container\":");
    mi_json_string(b, m->container);
    mi_json_raw(b, ",\"confidence\":");
    mi_json_double(b, m->confidence);
    mi_json_raw(b, ",\"is_video\":");
    mi_json_bool(b, m->is_video);
    mi_json_raw(b, ",\"is_image\":");
    mi_json_bool(b, m->is_image);
    mi_json_raw(b, ",\"is_audio\":");
    mi_json_bool(b, m->is_audio);
    mi_json_raw(b, ",\"width\":");
    mi_json_int(b, m->width);
    mi_json_raw(b, ",\"height\":");
    mi_json_int(b, m->height);
    mi_json_raw(b, ",\"qualities\":[");
    for (size_t i = 0; i < m->quality_count; i++) {
        const mi_quality *q = &m->qualities[i];
        if (i) mi_json_raw(b, ",");
        mi_json_raw(b, "{\"width\":");
        mi_json_int(b, q->width);
        mi_json_raw(b, ",\"height\":");
        mi_json_int(b, q->height);
        mi_json_raw(b, ",\"bandwidth\":");
        mi_json_int(b, q->bandwidth);
        mi_json_raw(b, ",\"bytes\":");
        mi_json_int(b, q->est_bytes);
        mi_json_raw(b, ",\"codecs\":");
        mi_json_string(b, q->codecs);
        mi_json_raw(b, ",\"url\":");
        mi_json_string(b, q->url);
        mi_json_raw(b, "}");
    }
    mi_json_raw(b, "]}");
}

static mi_download_rec *find_download(mi_server *srv, const char *id);

/* File extension for a detected kind. A downloaded file with no extension is
 * useless to the OS — nothing can associate a player with it. */
static const char *extension_for_kind(const char *kind) {
    if (!kind) return "bin";
    if (!strcmp(kind, "mp4")) return "mp4";
    if (!strcmp(kind, "matroska")) return "mkv";
    if (!strcmp(kind, "avi")) return "avi";
    if (!strcmp(kind, "flv")) return "flv";
    if (!strcmp(kind, "ogg")) return "ogv";
    if (!strcmp(kind, "asf")) return "wmv";
    if (!strcmp(kind, "mpeg-ts")) return "ts";
    if (!strcmp(kind, "mpeg-ps")) return "mpg";
    if (!strcmp(kind, "hls")) return "m3u8";
    if (!strcmp(kind, "dash")) return "mpd";
    if (!strcmp(kind, "jpeg")) return "jpg";
    if (!strcmp(kind, "png")) return "png";
    if (!strcmp(kind, "gif")) return "gif";
    if (!strcmp(kind, "webp")) return "webp";
    if (!strcmp(kind, "bmp")) return "bmp";
    if (!strcmp(kind, "tiff")) return "tiff";
    if (!strcmp(kind, "avif")) return "avif";
    if (!strcmp(kind, "heic")) return "heic";
    if (!strcmp(kind, "mp3")) return "mp3";
    if (!strcmp(kind, "aac")) return "aac";
    if (!strcmp(kind, "flac")) return "flac";
    if (!strcmp(kind, "wav")) return "wav";
    if (!strcmp(kind, "ogg-audio")) return "ogg";
    /* Non-media, only reachable with allow_non_media. A PDF or a text file
     * still needs the right extension or the OS cannot open it. */
    if (!strcmp(kind, "pdf")) return "pdf";
    if (!strcmp(kind, "text")) return "txt";
    if (!strcmp(kind, "zip")) return "zip";
    return "bin";
}

/* A filesystem-safe single path component. Separators and control characters
 * are dropped, leading dots/spaces are trimmed (so a name cannot escape its
 * directory or silently become a hidden file), and the result is bounded. */
static char *sanitize_component(const char *in) {
    if (!in) return NULL;
    size_t n = strlen(in);
    char *buf = mi_malloc(n + 1);
    if (!buf) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n && o < 200; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '/' || c == '\\' || c < 0x20 || c == 0x7f) continue;
        buf[o++] = (char)c;
    }
    size_t start = 0;
    while (start < o && (buf[start] == '.' || buf[start] == ' ')) start++;
    if (start == o) {
        free(buf);
        return NULL;
    }
    char *out = mi_strndup(buf + start, o - start);
    free(buf);
    return out;
}

/* The on-disk filename for a download: its display name when one is set, else
 * the name derived from the source URL, always carrying the detected extension
 * so the OS can still associate a player with it. */
static char *source_filename(const mi_download_rec *j) {
    char *base = j->display_name ? sanitize_component(j->display_name) : NULL;
    if (!base) {
        /* No explicit name: derive it from the URL, the same way the UI does, so
         * the file on disk is called what the list calls it. */
        char *urlname = basename_from_url(j->url);
        base = urlname ? sanitize_component(urlname) : NULL;
        free(urlname);
    }
    if (!base) base = mi_strdup("source");
    if (!base) return NULL;

    /*
     * The extension is corrected when the content says otherwise.
     *
     * A site that serves an MP4 as `clip.bin` — or as `download.php`, or with no
     * extension at all — otherwise leaves a file whose name lies about what it
     * is, so the OS opens it in a text editor and no player will touch it. When
     * the detector or ffprobe has named the kind, that name wins over whatever
     * the URL claimed.
     *
     * An unknown kind keeps the URL's own extension: replacing it with "bin"
     * would throw away the only hint available.
     */
    const char *want = extension_for_kind(j->media_kind);
    const int known = j->media_kind && strcmp(j->media_kind, "unknown") != 0;
    char *dot = strrchr(base, '.');
    if (dot && dot != base) {
        if (known && strcasecmp(dot + 1, want) != 0) {
            /* Replace only the extension, keeping the rest of the name. */
            char *replaced = mi_asprintf("%.*s.%s", (int)(dot - base), base, want);
            free(base);
            return replaced;
        }
        return base;
    }
    char *with_ext = mi_asprintf("%s.%s", base, want);
    free(base);
    return with_ext;
}

/* Directory part of a path, allocated. "." when there is no separator. */
static char *path_dirname(const char *path) {
    char *dir = mi_strdup(path);
    if (!dir) return NULL;
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    return dir;
}

/* Rename an item. The display name is always updated; when the bytes are on
 * disk the file is renamed too, keeping the detected extension. */
static int apply_rename(mi_download_rec *j, const char *new_name) {
    char *safe = sanitize_component(new_name);
    if (!safe) return MI_ERR_ARG;
    free(j->display_name);
    j->display_name = safe;

    if (j->source_path) {
        char *dir = path_dirname(j->source_path);
        char *fname = source_filename(j);
        char *dest = (dir && fname) ? mi_path_join(dir, fname) : NULL;
        free(dir);
        free(fname);
        if (!dest) return MI_ERR_NOMEM;
        if (strcmp(dest, j->source_path) != 0 && rename(j->source_path, dest) == 0) {
            free(j->source_path);
            j->source_path = dest;
        } else {
            free(dest);
        }
    }
    return MI_OK;
}

/* Relocate an item's directory into a named folder inside the library. The
 * directory keeps its id name, so two items moved to the same folder cannot
 * collide, and the move can never escape the work dir. */
static int apply_move(mi_server *srv, mi_download_rec *j, const char *folder) {
    char *safe = sanitize_component(folder);
    if (!safe) return MI_ERR_ARG;
    char *new_dir = mi_asprintf("%s/%s/%s", srv->download_dir, safe, j->id);
    free(safe);
    if (!new_dir) return MI_ERR_NOMEM;
    char *old_dir = j->out_dir ? mi_strdup(j->out_dir)
                               : mi_asprintf("%s/%s", srv->download_dir, j->id);
    if (!old_dir) {
        free(new_dir);
        return MI_ERR_NOMEM;
    }

    int rc = MI_OK;
    if (strcmp(old_dir, new_dir) != 0) {
        /* Create only the parent: rename(2) then moves the item's directory in
         * one step, so a failure leaves the bytes where they were. */
        char *parent = path_dirname(new_dir);
        if (parent) mi_mkdir_p(parent);
        free(parent);
        if (mi_path_exists(old_dir) && rename(old_dir, new_dir) != 0) rc = MI_ERR_IO;
        if (rc == MI_OK) {
            size_t olen = strlen(old_dir);
            if (j->source_path && !strncmp(j->source_path, old_dir, olen)) {
                const char *rel = j->source_path + olen;
                while (*rel == '/') rel++;
                char *moved = mi_path_join(new_dir, rel);
                if (moved) {
                    free(j->source_path);
                    j->source_path = moved;
                }
            }
            /* A conversion writes into the item's directory too, so its path
             * has to follow the move or /converted would start 404ing. */
            if (j->converted_path && !strncmp(j->converted_path, old_dir, olen)) {
                const char *rel = j->converted_path + olen;
                while (*rel == '/') rel++;
                char *moved = mi_path_join(new_dir, rel);
                if (moved) {
                    free(j->converted_path);
                    j->converted_path = moved;
                }
            }
            free(j->out_dir);
            j->out_dir = mi_strdup(new_dir);
            free(j->hls_dir);
            j->hls_dir = mi_asprintf("%s/hls", new_dir);
        }
    }
    free(old_dir);
    free(new_dir);
    return rc;
}

/* Content-Type for serving a downloaded file back to a browser. */
static const char *content_type_for_kind(const char *kind) {
    if (!kind) return "application/octet-stream";
    if (!strcmp(kind, "mp4")) return "video/mp4";
    if (!strcmp(kind, "matroska")) return "video/x-matroska";
    if (!strcmp(kind, "mpeg-ts")) return "video/mp2t";
    if (!strcmp(kind, "hls")) return "application/vnd.apple.mpegurl";
    if (!strcmp(kind, "dash")) return "application/dash+xml";
    if (!strcmp(kind, "jpeg")) return "image/jpeg";
    if (!strcmp(kind, "png")) return "image/png";
    if (!strcmp(kind, "gif")) return "image/gif";
    if (!strcmp(kind, "webp")) return "image/webp";
    if (!strcmp(kind, "bmp")) return "image/bmp";
    if (!strcmp(kind, "tiff")) return "image/tiff";
    if (!strcmp(kind, "avif")) return "image/avif";
    if (!strcmp(kind, "heic")) return "image/heic";
    if (!strcmp(kind, "mp3")) return "audio/mpeg";
    if (!strcmp(kind, "aac")) return "audio/aac";
    if (!strcmp(kind, "flac")) return "audio/flac";
    if (!strcmp(kind, "wav")) return "audio/wav";
    if (!strcmp(kind, "ogg-audio")) return "audio/ogg";
    return "application/octet-stream";
}

/* An item's directory. Caller holds the server lock. Defined below. */
static char *download_dir_for(const mi_server *srv, const mi_download_rec *j);

/* Stream one file, honouring a single Range request so a browser can seek.
 * Video needs ranges; without them playback of anything non-trivial fails,
 * which is why this is not simply a whole-file send. */
static void serve_file(int fd, const http_request *req, const char *path,
                       const char *ctype, const char *filename, const char *cors) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        send_error(fd, 404, "Not Found", "file is no longer on disk", cors);
        return;
    }
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (total < 0) total = 0;

    long start = 0;
    long end = total > 0 ? total - 1 : 0;
    int is_range = 0;
    const char *range = request_header(req, "Range");
    if (range && !strncmp(range, "bytes=", 6)) {
        const char *spec = range + 6;
        char *dash = strchr(spec, '-');
        if (dash) {
            start = strtol(spec, NULL, 10);
            if (dash + 1 < spec + strlen(spec)) end = strtol(dash + 1, NULL, 10);
            if (start < 0 || start > end || start >= total) {
                fclose(f);
                send_error(fd, 416, "Range Not Satisfiable", "bad range", cors);
                return;
            }
            if (end >= total) end = total - 1;
            is_range = 1;
        }
    }

    long len = end - start + 1;
    /* The download name travels in Content-Disposition so a shared or saved
     * file keeps a real name instead of the id-based path. */
    char header[1024];
    char disposition[300];
    disposition[0] = '\0';
    if (filename) {
        char *safe = sanitize_component(filename);
        if (safe) {
            snprintf(disposition, sizeof disposition,
                     "Content-Disposition: inline; filename=\"%s\"\r\n", safe);
            free(safe);
        }
    }
    if (is_range) {
        int hn = snprintf(header, sizeof header,
                          "HTTP/1.1 206 Partial Content\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %ld\r\n"
                          "Content-Range: bytes %ld-%ld/%ld\r\n"
                          "Accept-Ranges: bytes\r\n"
                          "%s"
                          "Cache-Control: no-store\r\n"
                          "%s%s%s"
                          "Connection: close\r\n\r\n",
                          ctype, len, start, end, total, disposition,
                          cors ? "Access-Control-Allow-Origin: " : "",
                          cors ? cors : "",
                          cors ? "\r\n"
                                 "Access-Control-Expose-Headers: Content-Length,Content-Range,Content-Disposition\r\n"
                                 "Vary: Origin\r\n"
                               : "");
        send_all(fd, header, (size_t)hn);
    } else {
        int hn = snprintf(header, sizeof header,
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %ld\r\n"
                          "Accept-Ranges: bytes\r\n"
                          "%s"
                          "Cache-Control: no-store\r\n"
                          "%s%s%s"
                          "Connection: close\r\n\r\n",
                          ctype, len, disposition,
                          cors ? "Access-Control-Allow-Origin: " : "",
                          cors ? cors : "",
                          cors ? "\r\n"
                                 "Access-Control-Expose-Headers: Content-Length,Content-Range,Content-Disposition\r\n"
                                 "Vary: Origin\r\n"
                               : "");
        send_all(fd, header, (size_t)hn);
    }

    if (strcmp(req->method, "HEAD") != 0) {
        fseek(f, start, SEEK_SET);
        char buf[64 * 1024];
        long remaining = len;
        while (remaining > 0) {
            size_t want = (size_t)(remaining < (long)sizeof buf ? remaining : (long)sizeof buf);
            size_t got = fread(buf, 1, want, f);
            if (got == 0) break;
            send_all(fd, buf, got);
            remaining -= (long)got;
        }
    }
    fclose(f);
}

static void serve_download_content(int fd, const http_request *req,
                                   mi_server *srv, const char *id, const char *cors) {
    pthread_mutex_lock(&srv->lock);
    mi_download_rec *j = find_download(srv, id);
    char *path = NULL;
    char *kind = NULL;
    char *filename = NULL;
    if (j && j->state == DOWNLOAD_DONE) {
        if (j->source_path) {
            path = mi_strdup(j->source_path);
        } else {
            /* Older records predate the extension rename. */
            char *dir = download_dir_for(srv, j);
            path = mi_path_join(dir, "source.bin");
            free(dir);
        }
        kind = mi_strdup(j->media_kind);
        filename = source_filename(j);
    }
    pthread_mutex_unlock(&srv->lock);

    if (!path) {
        free(filename);
        send_error(fd, 404, "Not Found", "no completed download with that id", cors);
        return;
    }
    const char *ctype = content_type_for_kind(kind);
    serve_file(fd, req, path, ctype, filename, cors);
    free(path);
    free(kind);
    free(filename);
}

/* An item's directory. `out_dir` is only set once an item has been moved, so it
 * has to be derived from the work dir otherwise — forgetting this is how a
 * conversion ends up with no input and no directory to write to. Caller holds
 * the server lock. */
static char *download_dir_for(const mi_server *srv, const mi_download_rec *j) {
    if (j->out_dir) return mi_strdup(j->out_dir);
    return mi_asprintf("%s/%s", srv->download_dir, j->id);
}

/* Content type for a transcoded HLS artifact, by extension. */
static const char *hls_content_type(const char *path) {
    size_t n = strlen(path);
    if (n > 5 && !strcmp(path + n - 5, ".m3u8")) return "application/vnd.apple.mpegurl";
    if (n > 3 && !strcmp(path + n - 3, ".ts")) return "video/mp2t";
    if (n > 4 && !strcmp(path + n - 4, ".m4s")) return "video/iso.segment";
    if (n > 4 && !strcmp(path + n - 4, ".mp4")) return "video/mp4";
    return "application/octet-stream";
}

/* True when `sub` is a safe relative path: no empty, "." or ".." segment, no
 * absolute start, no backslash, no control characters. Nothing legitimate needs
 * any of those, so they are refused rather than normalised away. */
static int is_safe_relative_path(const char *sub) {
    if (!sub || !*sub || sub[0] == '/') return 0;
    const char *seg = sub;
    while (*seg) {
        const char *end = strchr(seg, '/');
        size_t n = end ? (size_t)(end - seg) : strlen(seg);
        if (n == 0) return 0;                                  /* empty segment */
        if (n == 1 && seg[0] == '.') return 0;                 /* "." */
        if (n == 2 && seg[0] == '.' && seg[1] == '.') return 0; /* ".." */
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)seg[i];
            if (c == '\\' || c < 0x20 || c == 0x7f) return 0;
        }
        if (!end) break;
        seg = end + 1;
    }
    return 1;
}

/*
 * Serve an artifact from a download's own directory.
 *
 * `sub` is untrusted: it comes straight from the URL. It is first rejected
 * outright if it contains any traversal segment, and then the resolved path is
 * required to sit under the item's real directory — so a symlink planted in the
 * library cannot be used to read a file outside it either.
 */
static void serve_artifact(int fd, const http_request *req, mi_server *srv,
                           const char *id, const char *sub, const char *cors) {
    if (!is_safe_relative_path(sub)) {
        send_error(fd, 400, "Bad Request", "invalid artifact path", cors);
        return;
    }

    pthread_mutex_lock(&srv->lock);
    mi_download_rec *j = find_download(srv, id);
    char *dir = NULL;
    if (j && j->state == DOWNLOAD_DONE) {
        /* The URL's `hls/` prefix means the path is relative to the transcoded
         * output, which is `<item>/hls` unless the job named its own. */
        if (j->hls_dir) {
            dir = mi_strdup(j->hls_dir);
        } else {
            char *item = download_dir_for(srv, j);
            dir = item ? mi_path_join(item, "hls") : NULL;
            free(item);
        }
    }
    pthread_mutex_unlock(&srv->lock);

    if (!dir) {
        send_error(fd, 404, "Not Found", "no completed download with that id", cors);
        return;
    }
    char *path = mi_path_join(dir, sub);
    if (!path) {
        free(dir);
        send_error(fd, 500, "Internal Server Error", "out of memory", cors);
        return;
    }

    /* Both sides are resolved, so the containment test compares real paths and
     * a symlinked directory cannot slip past a string prefix. A path that does
     * not resolve yet is served by serve_file, which reports 404 on its own. */
    char real_dir[PATH_MAX];
    char real_path[PATH_MAX];
    int ok = realpath(dir, real_dir) != NULL && realpath(path, real_path) != NULL &&
             !strncmp(real_path, real_dir, strlen(real_dir)) &&
             real_path[strlen(real_dir)] == '/';
    free(dir);
    if (!ok) {
        free(path);
        send_error(fd, 404, "Not Found", "no such artifact", cors);
        return;
    }
    serve_file(fd, req, path, hls_content_type(path), NULL, cors);
    free(path);
}


static storage_snapshot *snapshot_from_headers(const http_request *req) {
    /* Storage settings travel in headers so secrets never hit the download store
     * on disk; the snapshot lives only for the life of the process. */
    storage_snapshot *s = mi_calloc(1, sizeof *s);
    if (!s) return NULL;
    const char *h;
    if ((h = request_header(req, "X-S3-Endpoint"))) s->endpoint = mi_strdup(h);
    if ((h = request_header(req, "X-S3-Region"))) s->region = mi_strdup(h);
    if ((h = request_header(req, "X-S3-Bucket"))) s->bucket = mi_strdup(h);
    if ((h = request_header(req, "X-S3-Access-Key"))) s->access_key = mi_strdup(h);
    if ((h = request_header(req, "X-S3-Secret-Key"))) s->secret_key = mi_strdup(h);
    if ((h = request_header(req, "X-S3-Session-Token"))) s->session_token = mi_strdup(h);
    if ((h = request_header(req, "X-Cdn-Base"))) s->cdn_base = mi_strdup(h);
    if ((h = request_header(req, "X-CF-Token"))) s->cf_token = mi_strdup(h);
    if ((h = request_header(req, "X-CF-Zone"))) s->cf_zone = mi_strdup(h);
    if ((h = request_header(req, "X-S3-Path-Style")))
        s->path_style = !strcmp(h, "0") ? 0 : 1;
    else
        s->path_style = 1;
    return s;
}

static void snapshot_free(storage_snapshot *s) {
    if (!s) return;
    free(s->endpoint);
    free(s->region);
    free(s->bucket);
    free(s->access_key);
    free(s->secret_key);
    free(s->session_token);
    free(s->cdn_base);
    free(s->cf_token);
    free(s->cf_zone);
    free(s);
}

/* Upload a directory tree, collecting public URLs. */
static int upload_dir(const storage_snapshot *snap, const char *dir,
                      const char *prefix, char ***urls, size_t *n) {
    mi_s3_config cfg;
    mi_s3_config_init(&cfg);
    cfg.endpoint = snap->endpoint ? snap->endpoint : "https://s3.amazonaws.com";
    cfg.region = snap->region ? snap->region : "us-east-1";
    cfg.bucket = snap->bucket;
    cfg.access_key = snap->access_key;
    cfg.secret_key = snap->secret_key;
    cfg.session_token = snap->session_token;
    cfg.path_style = snap->path_style;

    if (!cfg.bucket || !cfg.access_key || !cfg.secret_key) return MI_ERR_ARG;

    /* Walk the tree. */
    char **files = NULL;
    size_t count = 0;
    size_t cap = 0;
    /* Simple explicit stack of directories. */
    char **stack = mi_malloc(sizeof(char *));
    size_t stack_n = 0;
    stack[stack_n++] = mi_strdup(dir);
    while (stack_n > 0) {
        char *cur = stack[--stack_n];
        DIR *d = opendir(cur);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
                char *child = mi_path_join(cur, ent->d_name);
                if (mi_path_is_dir(child)) {
                    stack = mi_realloc(stack, (stack_n + 1) * sizeof(char *));
                    stack[stack_n++] = child;
                } else {
                    if (count + 1 > cap) {
                        cap = cap ? cap * 2 : 16;
                        files = mi_realloc(files, cap * sizeof(char *));
                    }
                    files[count++] = child;
                }
            }
            closedir(d);
        }
        free(cur);
    }
    free(stack);

    int rc = MI_OK;
    for (size_t i = 0; i < count; i++) {
        const char *rel = files[i] + strlen(dir);
        while (*rel == '/') rel++;
        char *key;
        if (prefix && *prefix) {
            size_t pl = strlen(prefix);
            key = mi_asprintf("%s%s%s", prefix, prefix[pl - 1] == '/' ? "" : "/", rel);
        } else {
            key = mi_strdup(rel);
        }
        mi_upload_result up;
        if (mi_s3_upload_file(&cfg, files[i], key, &up) != MI_OK) {
            rc = MI_ERR;
            free(key);
            continue;
        }
        char *public_url = mi_storage_public_url(&cfg, snap->cdn_base, key);
        if (public_url) {
            char **grown = mi_realloc(*urls, (*n + 1) * sizeof(char *));
            if (grown) {
                *urls = grown;
                (*urls)[*n] = public_url;
                (*n)++;
            } else {
                free(public_url);
            }
        }
        free(key);
        mi_upload_result_free(&up);
    }
    for (size_t i = 0; i < count; i++) free(files[i]);
    free(files);
    return rc;
}

static void *download_worker(void *arg);

/* Progress callback: records bytes done and a smoothed transfer rate on the
 * record so GET /downloads/{id} can report live speed. Called from the
 * downloader's worker threads, so it takes the server lock. */
typedef struct {
    mi_server *srv;
    mi_download_rec *rec;
} progress_ctx;

static void on_download_progress(const mi_progress *p, void *user) {
    progress_ctx *ctx = user;
    mi_server *srv = ctx->srv;
    mi_download_rec *rec = ctx->rec;

    pthread_mutex_lock(&srv->lock);
    rec->downloaded = p->downloaded_bytes;
    if (p->total_bytes > 0) rec->total_bytes = p->total_bytes;

    double t = mi_now();
    if (rec->rate_sample_at == 0) {
        rec->rate_sample_at = t;
        rec->rate_sample_bytes = p->downloaded_bytes;
    } else if (t - rec->rate_sample_at >= 0.5) {
        double dt = t - rec->rate_sample_at;
        int64_t db = p->downloaded_bytes - rec->rate_sample_bytes;
        if (dt > 0 && db >= 0) rec->bytes_per_second = (double)db / dt;
        rec->rate_sample_at = t;
        rec->rate_sample_bytes = p->downloaded_bytes;
    }
    pthread_mutex_unlock(&srv->lock);
}

typedef struct {
    mi_server *srv;
    mi_download_rec *download;
    storage_snapshot *snap;
} download_task;

/* Asked by the downloader between chunks. A pause or a cancel both stop the
 * transfer; which one it was is decided by the record's state when the worker
 * finishes. */
static int download_should_stop(void *user) {
    mi_download_rec *j = user;
    if (j->pause_requested) return 1;
    /* A cancel while the worker runs is also a stop. Read under the server lock
     * is not possible from a worker, so the state is checked as an int. */
    return 0;
}

static void snapshot_free(storage_snapshot *s);

static void *download_thread(void *arg) {
    download_task *task = arg;
    download_worker(task);
    /* The task owns the storage snapshot for the whole run; release it here so
     * every exit path is covered. */
    snapshot_free(task->snap);
    free(task);
    return NULL;
}

static void *download_worker(void *arg) {
    download_task *task = arg;
    mi_server *srv = task->srv;
    mi_download_rec *j = task->download;

    pthread_mutex_lock(&srv->lock);
    /* A pause requested before the worker got going must be honoured, not
     * cleared: the user asked for it. */
    if (j->pause_requested) {
        j->state = DOWNLOAD_PAUSED;
        j->worker_active = 0;
        pthread_mutex_unlock(&srv->lock);
        return NULL;
    }
    j->state = DOWNLOAD_RUNNING;
    j->started_at = (double)time(NULL);
    j->worker_active = 1;
    pthread_mutex_unlock(&srv->lock);

    mi_download_config dcfg;
    mi_download_config_init(&dcfg);
    dcfg.connections = j->connections;
    mi_download_control control = {.stop = download_should_stop, .user = j};
    dcfg.control = &control;
    /* Video, image and audio are all valid downloads; only payloads the
     * detector does not recognise are refused — unless the caller asked for
     * one, which is how a PDF or a text file gets through. */
    dcfg.require_media = !j->allow_non_media;
    /*
     * ffmpeg is what reads a manifest. An HLS or DASH URL is a playlist, not a
     * file, so the transfer path hands it to ffmpeg — which fetches every
     * segment and muxes them. Without this, "downloading" a stream saved the
     * playlist text and reported success.
     */
    dcfg.ffmpeg_path = srv->cfg.ffmpeg_path;
    /*
     * A URL that probes as a page may be one the extractor knows. Enabled here
     * so a streaming page downloads its streams rather than its HTML — the
     * extractor is tried only when the URL is not already recognisable media, so
     * an ordinary file download never pays for it.
     */
    dcfg.extract_pages = 1;
    dcfg.extractor_path = srv->cfg.extractor_path;
    dcfg.gallery_extractor_path = srv->cfg.gallery_extractor_path;
    /*
     * The viewer's quality choice, when they made one.
     *
     * The console sends a label ("720p"); the library wants a number, and the
     * parse is here so the domain stays a number. A label it cannot read is
     * ignored rather than guessed at, which leaves the largest — the same
     * behaviour as no choice.
     */
    if (j->requested_quality && *j->requested_quality) {
        int h = atoi(j->requested_quality);
        if (h > 0) dcfg.preferred_height = h;
    }
    /* A host that refuses a non-browser agent is a host the operator may have
     * told Vyzor how to present itself to. */
    dcfg.user_agent = srv->cfg.user_agent;

    char *out_dir = j->out_dir ? mi_strdup(j->out_dir)
                               : mi_asprintf("%s/%s", srv->download_dir, j->id);
    mi_mkdir_p(out_dir);
    char *dest = mi_path_join(out_dir, "source.bin");

    progress_ctx pctx = {.srv = srv, .rec = j};
    mi_download_result res;
    int rc = mi_download(&dcfg, j->url, dest, on_download_progress, &pctx, &res);
    if (rc == MI_ERR_ABORTED) {
        /* Stopped on purpose. The partial parts are still on disk, so this is a
         * pause the user can resume, not a failure to report. */
        pthread_mutex_lock(&srv->lock);
        j->state = DOWNLOAD_PAUSED;
        j->bytes_per_second = 0;
        j->worker_active = 0;
        pthread_mutex_unlock(&srv->lock);
        free(dest);
        free(out_dir);
        return NULL;
    }
    if (rc != MI_OK) {
        pthread_mutex_lock(&srv->lock);
        j->state = DOWNLOAD_FAILED;
        j->bytes_per_second = 0;
        j->error = mi_strdup("download failed");
        j->finished_at = (double)time(NULL);
        j->worker_active = 0;
        pthread_mutex_unlock(&srv->lock);
        free(dest);
        free(out_dir);
        return NULL;
    }

    char *sha = mi_malloc(65);
    mi_sha256_file(dest, sha);

    char *hls_dir = j->hls_dir ? mi_strdup(j->hls_dir)
                               : mi_asprintf("%s/hls", out_dir);
    char *master = NULL;
    if (j->do_hls) {
        mi_hls_config hcfg;
        mi_hls_config_init(&hcfg);
        mi_hls_result hres;
        if (mi_hls_transcode(&hcfg, dest, hls_dir, &hres) == MI_OK) {
            master = mi_strdup(hres.master_playlist);
            mi_hls_result_free(&hres);
        }
    }

    char **urls = NULL;
    size_t n_urls = 0;
    if (j->do_upload && task->snap && task->snap->bucket) {
        const char *src = j->do_hls ? hls_dir : out_dir;
        upload_dir(task->snap, src, j->prefix, &urls, &n_urls);
        if (j->do_purge && n_urls > 0 && task->snap->cf_token && task->snap->cf_zone) {
            mi_cdn_config cdn;
            mi_cdn_config_init(&cdn);
            cdn.api_token = task->snap->cf_token;
            cdn.zone_id = task->snap->cf_zone;
            mi_cdn_result cr;
            mi_cdn_purge_urls(&cdn, (const char *const *)urls, n_urls, &cr);
            mi_cdn_result_free(&cr);
        }
    }

    pthread_mutex_lock(&srv->lock);
    j->state = DOWNLOAD_DONE;
    j->bytes_per_second = 0;
    j->bytes = res.bytes_written;
    j->downloaded = res.bytes_written;
    j->total_bytes = res.bytes_written;
    j->media_kind = mi_strdup(mi_kind_name(res.media.kind));
    j->container = mi_strdup(res.media.container);

    /*
     * When the built-in detector could not name the file — a `.bin`, a URL with
     * no extension, a content type the server did not send — ask ffprobe what it
     * actually is. The detector reads magic bytes and is right about the
     * containers it knows; this is the second opinion for everything else, and it
     * is what turns an unnamed blob into a file with a real extension and a
     * correct "can the browser play this" answer.
     *
     * Only consulted when the detector was unsure: a probe is a subprocess, and
     * paying for it when the answer is already known would be waste.
     */
    if (res.media.kind == MI_KIND_UNKNOWN) {
        mi_identify id;
        if (mi_identify_probe(dest, NULL, &id) == 0 && id.probed) {
            if (id.extension && strcmp(id.extension, "bin") != 0) {
                free(j->media_kind);
                j->media_kind = mi_strdup(id.extension);
            }
            if (id.format[0]) {
                free(j->container);
                j->container = mi_strdup(id.format);
            }
            if (id.width > 0) j->media_width = id.width;
            if (id.height > 0) j->media_height = id.height;
            if (id.has_video) j->media_is_video = 1;
            if (id.has_audio && !id.has_video) j->media_is_audio = 1;
            /* An image reports as a video stream in ffprobe, so it is recognised
             * by its codec and marked as one here. */
            if (!strcmp(id.video_codec, "png") || !strcmp(id.video_codec, "mjpeg") ||
                !strcmp(id.video_codec, "gif") || !strcmp(id.video_codec, "webp") ||
                !strcmp(id.video_codec, "bmp") || !strcmp(id.video_codec, "tiff")) {
                j->media_is_image = 1;
                j->media_is_video = 0;
            }
        }
    }

    /* Rename source.bin to a real extension so the OS can associate a player
     * with it when the user picks "Open with", and to the item's own name so
     * the library on disk reads like the library in the app. */
    char *fname = source_filename(j);
    char *renamed = fname ? mi_path_join(out_dir, fname) : NULL;
    free(fname);
    if (renamed) {
        if (rename(dest, renamed) == 0) {
            j->source_path = renamed;
        } else {
            free(renamed);
        }
    }
    if (res.media.kind != MI_KIND_UNKNOWN) {
        j->media_is_video = res.media.is_video;
        j->media_is_image = res.media.is_image;
        j->media_is_audio = res.media.is_audio;
        j->media_width = res.media.width;
        j->media_height = res.media.height;
    }
    j->sha256 = sha;
    j->master_playlist = master;
    j->artifact_urls = urls;
    j->artifact_count = n_urls;
    j->finished_at = (double)time(NULL);
    j->worker_active = 0;
    pthread_mutex_unlock(&srv->lock);

    mi_download_result_free(&res);
    free(dest);
    free(out_dir);
    free(hls_dir);
    return NULL;
}

/* Spawn a detached thread for a download. */
static int start_download(mi_server *srv, mi_download_rec *j, storage_snapshot *snap) {
    download_task *task = mi_calloc(1, sizeof *task);
    if (!task) return MI_ERR_NOMEM;
    task->srv = srv;
    task->download = j;
    task->snap = snap;
    pthread_t th;
    if (pthread_create(&th, NULL, download_thread, task) != 0) {
        free(task);
        return MI_ERR;
    }
    pthread_detach(th);
    return MI_OK;
}

/* ------------------------------------------------------------------ */
/* routing                                                             */
/* ------------------------------------------------------------------ */

static mi_download_rec *find_download(mi_server *srv, const char *id) {
    for (size_t i = 0; i < srv->download_count; i++) {
        if (!strcmp(srv->downloads[i]->id, id)) return srv->downloads[i];
    }
    return NULL;
}

static void add_download(mi_server *srv, mi_download_rec *j) {
    /* Trim oldest finished downloads if over the cap. */
    while (srv->download_count >= (size_t)srv->cfg.max_downloads) {
        size_t idx = 0;
        for (size_t i = 0; i < srv->download_count; i++) {
            if (srv->downloads[i]->state == DOWNLOAD_DONE || srv->downloads[i]->state == DOWNLOAD_FAILED) {
                idx = i;
                break;
            }
        }
        download_free(srv->downloads[idx]);
        memmove(&srv->downloads[idx], &srv->downloads[idx + 1],
                (srv->download_count - idx - 1) * sizeof(mi_download_rec *));
        srv->download_count--;
    }
    mi_download_rec **grown = mi_realloc(srv->downloads, (srv->download_count + 1) * sizeof(mi_download_rec *));
    if (!grown) return;
    srv->downloads = grown;
    srv->downloads[srv->download_count++] = j;
}

static int authorized(mi_server *srv, const http_request *req) {
    if (!srv->cfg.auth_token) return 1;
    const char *auth = request_header(req, "Authorization");
    if (!auth) return 0;
    if (strncmp(auth, "Bearer ", 7) != 0) return 0;
    return strcmp(auth + 7, srv->cfg.auth_token) == 0;
}

/* Prove a directory is writable by actually writing to it, rather than trusting
 * a mode bit or an access(2) check that a read-only mount would still pass.
 * Creates the directory if needed. Returns 0 when a real byte round-tripped. */
static int probe_writable(const char *dir, char *err, size_t err_size) {
    if (!dir || !*dir) {
        snprintf(err, err_size, "no directory given");
        return -1;
    }
    if (mi_mkdir_p(dir) != 0) {
        snprintf(err, err_size, "cannot create the directory");
        return -1;
    }
    char *probe = mi_asprintf("%s/.mi-write-probe", dir);
    if (!probe) {
        snprintf(err, err_size, "out of memory");
        return -1;
    }
    FILE *f = fopen(probe, "wb");
    if (!f) {
        snprintf(err, err_size, "directory is not writable");
        free(probe);
        return -1;
    }
    int wrote = fputc('x', f);
    int closed = fclose(f);
    unlink(probe);
    free(probe);
    if (wrote == EOF || closed != 0) {
        snprintf(err, err_size, "the write did not complete");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* container conversion                                                */
/* ------------------------------------------------------------------ */

/* Run a converter to completion, discarding its output. Returns the exit
 * status, or -1 when it could not be started. */
static int run_converter(char *const argv[]) {
    int devnull = open("/dev/null", O_RDWR);
    pid_t pid = fork();
    if (pid < 0) {
        if (devnull >= 0) close(devnull);
        return -1;
    }
    if (pid == 0) {
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    if (devnull >= 0) close(devnull);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/*
 * Convert a downloaded file into an MP4 the browser can play.
 *
 * A remux (`-c:v copy`) is tried first: an MKV almost always carries h264
 * already, so copying the video stream and re-encoding only the audio is both
 * near-instant and lossless for the picture. If that fails — a codec MP4 cannot
 * hold, an odd pixel format — it falls back to a real encode, which is slow but
 * works. The failure of the fast path is not reported: only the final outcome is.
 *
 * `+faststart` moves the index to the front of the file, without which the
 * browser has to fetch the whole thing before it will start playing.
 *
 * A file that arrives without a useful extension — a `.bin`, a `.php`, a URL
 * with nothing after it — reaches here the same way: the input is whatever is on
 * disk, and ffmpeg reads it by content rather than by name.
 */
static int convert_to_mp4(const char *input, const char *output, char *err, size_t err_size) {
    char *argv_copy[] = {
        "ffmpeg", "-y", "-i", (char *)input,
        "-c:v", "copy",
        "-c:a", "aac", "-b:a", "128k",
        "-movflags", "+faststart",
        (char *)output, NULL,
    };
    if (run_converter(argv_copy) == 0 && mi_file_size(output) > 0) return MI_OK;

    /* The fast path leaves a partial file behind when it fails; start clean. */
    unlink(output);

    char *argv_encode[] = {
        "ffmpeg", "-y", "-i", (char *)input,
        "-c:v", "libx264", "-preset", "veryfast", "-crf", "23",
        "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-b:a", "128k",
        "-movflags", "+faststart",
        (char *)output, NULL,
    };
    if (run_converter(argv_encode) == 0 && mi_file_size(output) > 0) return MI_OK;

    snprintf(err, err_size, "ffmpeg could not convert this file");
    return MI_ERR;
}

/* One conversion job. Runs off the connection thread so the request returns. */
typedef struct {
    mi_server *srv;
    mi_download_rec *download;
} convert_task;

static void *convert_thread(void *arg) {
    convert_task *task = arg;
    mi_server *srv = task->srv;
    mi_download_rec *j = task->download;
    free(task);

    pthread_mutex_lock(&srv->lock);
    char *dir = download_dir_for(srv, j);
    /* Older records predate the extension rename, so their bytes are still at
     * source.bin. Falling back keeps an old library convertible. */
    char *input = j->source_path ? mi_strdup(j->source_path)
                                 : (dir ? mi_path_join(dir, "source.bin") : NULL);
    pthread_mutex_unlock(&srv->lock);

    char *output = NULL;
    char err[256] = {0};
    int rc = MI_ERR;

    if (input && dir) {
        output = mi_path_join(dir, "converted.mp4");
        if (output) rc = convert_to_mp4(input, output, err, sizeof err);
    }

    pthread_mutex_lock(&srv->lock);
    j->converting = 0;
    if (rc == MI_OK) {
        free(j->converted_path);
        j->converted_path = output; /* takes ownership */
        j->converted = 1;
        free(j->convert_error);
        j->convert_error = NULL;
    } else {
        free(output);
        j->converted = 0;
        free(j->convert_error);
        j->convert_error = mi_strdup(err[0] ? err : "conversion failed");
    }
    /* Release the record last: a delete waiting on this count must not see it
     * reach zero before the write above has landed. */
    j->convert_active = 0;
    pthread_mutex_unlock(&srv->lock);

    free(input);
    free(dir);
    return NULL;
}

/* Start a conversion off the connection thread. Caller holds the server lock,
 * and has already set `converting` so a second request cannot start a rival
 * job on the same output file. */
static int start_conversion(mi_server *srv, mi_download_rec *j) {
    convert_task *task = mi_calloc(1, sizeof *task);
    if (!task) return MI_ERR_NOMEM;
    task->srv = srv;
    task->download = j;
    j->convert_active = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, convert_thread, task) != 0) {
        j->convert_active = 0;
        free(task);
        return MI_ERR;
    }
    pthread_detach(th);
    return MI_OK;
}

/* ------------------------------------------------------------------ */
/* Live page sessions (WebSocket)                                      */
/* ------------------------------------------------------------------ */

/* Pull a numeric query parameter, or `fallback` when it is absent. */
static int query_int(const char *query, const char *name, int fallback) {
    if (!query) return fallback;
    size_t nlen = strlen(name);
    for (const char *p = query; *p;) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > nlen && !strncmp(p, name, nlen) && p[nlen] == '=') {
            return atoi(p + nlen + 1);
        }
        p = amp ? amp + 1 : p + seg;
    }
    return fallback;
}

/* Pull a URL-decoded string query parameter. Allocated, or NULL. */
static char *query_str(const char *query, const char *name) {
    if (!query) return NULL;
    size_t nlen = strlen(name);
    for (const char *p = query; *p;) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > nlen && !strncmp(p, name, nlen) && p[nlen] == '=') {
            return percent_decode(p + nlen + 1, seg - nlen - 1);
        }
        p = amp ? amp + 1 : p + seg;
    }
    return NULL;
}

/* Forward one JSON command from the console to the browser. Unknown types are
 * ignored rather than fatal, so a newer console is not broken by an older
 * Vyzor. */
static void ws_dispatch(mi_interactive *it, const mi_json *m) {
    const char *type = mi_json_get_string(m, "type", NULL);
    if (!type) return;

    if (!strcmp(type, "navigate")) {
        const char *url = mi_json_get_string(m, "url", NULL);
        if (url) mi_interactive_navigate(it, url);
    } else if (!strcmp(type, "reload")) {
        mi_interactive_reload(it);
    } else if (!strcmp(type, "resize")) {
        mi_interactive_resize(it, (int)mi_json_get_number(m, "width", 0),
                              (int)mi_json_get_number(m, "height", 0));
    } else if (!strcmp(type, "screenshot")) {
        mi_interactive_screenshot(it);
    } else if (!strcmp(type, "mouse")) {
        mi_interactive_mouse(it, mi_json_get_string(m, "event", "mouseMoved"),
                             mi_json_get_number(m, "x", 0), mi_json_get_number(m, "y", 0),
                             mi_json_get_string(m, "button", "none"),
                             (int)mi_json_get_number(m, "click_count", 0),
                             (int)mi_json_get_number(m, "buttons", 0),
                             (int)mi_json_get_number(m, "modifiers", 0));
    } else if (!strcmp(type, "wheel")) {
        mi_interactive_wheel(it, mi_json_get_number(m, "x", 0),
                             mi_json_get_number(m, "y", 0),
                             mi_json_get_number(m, "dx", 0),
                             mi_json_get_number(m, "dy", 0),
                             (int)mi_json_get_number(m, "modifiers", 0));
    } else if (!strcmp(type, "key")) {
        mi_interactive_key(it, mi_json_get_string(m, "event", "keyDown"),
                           mi_json_get_string(m, "key", NULL),
                           mi_json_get_string(m, "code", NULL),
                           (int)mi_json_get_number(m, "vk", 0),
                           mi_json_get_string(m, "text", NULL),
                           (int)mi_json_get_number(m, "modifiers", 0));
    } else if (!strcmp(type, "text")) {
        mi_interactive_text(it, mi_json_get_string(m, "text", NULL));
    } else if (!strcmp(type, "back")) {
        mi_interactive_go_back(it);
    } else if (!strcmp(type, "forward")) {
        mi_interactive_go_forward(it);
    } else if (!strcmp(type, "stop")) {
        mi_interactive_stop(it);
    } else if (!strcmp(type, "watch")) {
        /*
         * Whether to collect the media the page loads.
         *
         * Off until asked: the collection is per-request bookkeeping, and a viewer
         * that never opens the list should not pay for it.
         */
        mi_interactive_watch_media(it, mi_json_get_bool(m, "on", 1));
    } else if (!strcmp(type, "rescan")) {
        mi_interactive_rescan_media(it);
    } else if (!strcmp(type, "ping")) {
        /*
         * The console's own heartbeat.
         *
         * A browser answers a protocol ping automatically, but a backgrounded
         * tab throttles its timers and a suspended phone stops answering at all,
         * so the browser's pong alone is not a reliable sign of life. The console
         * sends this on a timer that survives being backgrounded, and receiving
         * any bytes already counts as being heard (see the read loop) — this
         * handler exists so a heartbeat is a no-op rather than an error.
         */
    } else if (!strcmp(type, "touch")) {
        /* Both point shapes are accepted; see mi_interactive_parse_points. */
        double coords[32];
        int count = mi_interactive_parse_points(mi_json_get(m, "points"), coords, 16);
        mi_interactive_touch(it, mi_json_get_string(m, "event", "touchStart"), coords, count,
                             (int)mi_json_get_number(m, "modifiers", 0));
    }
}

/* Serialize a WebSocket frame into `out`, appending. Used to build a message the
 * outbox can treat as one indivisible unit. */
static void append_frame(mi_json_buf *out, int opcode, const void *payload, size_t len) {
    unsigned char header[10];
    size_t hlen = mi_ws_build_header(header, opcode, len);
    mi_json_bytes(out, header, hlen);
    mi_json_bytes(out, payload, len);
}

/*
 * Queue a picture as one message: the JSON header frame and the binary frame
 * together.
 *
 * They must travel as a unit. A client that receives the header and then loses
 * the picture is left waiting for bytes that never come; putting both in one
 * outbox message means the queue can drop the pair whole, which is what makes
 * dropping safe.
 */
static int ws_queue_image(mi_outbox *box, const char *type, const char *format, int w, int h,
                          int seq, const unsigned char *png, size_t png_len) {
    mi_json_buf hdr;
    mi_json_init(&hdr);
    mi_json_raw(&hdr, "{\"type\":");
    mi_json_string(&hdr, type);
    mi_json_raw(&hdr, ",\"format\":");
    mi_json_string(&hdr, format);
    mi_json_raw(&hdr, ",\"width\":");
    mi_json_int(&hdr, w);
    mi_json_raw(&hdr, ",\"height\":");
    mi_json_int(&hdr, h);
    mi_json_raw(&hdr, ",\"seq\":");
    mi_json_int(&hdr, seq);
    mi_json_raw(&hdr, "}");

    /* A scratch buffer holding the header frame followed by the picture. */
    mi_json_buf msg;
    mi_json_init(&msg);
    append_frame(&msg, MI_WS_TEXT, mi_json_data(&hdr), hdr.len);
    append_frame(&msg, MI_WS_BINARY, png, png_len);
    mi_json_free(&hdr);

    int rc = mi_outbox_push(box, mi_json_data(&msg), msg.len, 1 /* droppable */);
    mi_json_free(&msg);
    return rc;
}

/* Queue one text message (a state change, an error, a farewell). */
static int ws_queue_text(mi_outbox *box, const char *json, size_t len, int droppable) {
    mi_json_buf msg;
    mi_json_init(&msg);
    append_frame(&msg, MI_WS_TEXT, json, len);
    int rc = mi_outbox_push(box, mi_json_data(&msg), msg.len, droppable);
    mi_json_free(&msg);
    return rc;
}

/*
 * Drive a live page for the life of one connection.
 *
 * This function owns the socket until the session ends. It is a poll loop over
 * two directions: commands in from the console, and frames/state out from the
 * browser. The two are interleaved on one thread because a session is one
 * user's page; there is nothing to parallelise, and one thread per session is
 * what makes the concurrency gate meaningful.
 */
/* ------------------------------------------------------------------ */
/* Live video sessions (raw video over WebRTC, signalled over HTTP)     */
/* ------------------------------------------------------------------ */

/*
 * A session registry.
 *
 * The signalling is HTTP, so a session has to survive between requests — the
 * offer is one request, the answer is another, and the poll that watches it is a
 * third. This maps a token to the session, and it is the token that keeps a
 * request from steering somebody else's stream.
 *
 * One mutex around the table. The sessions themselves are only ever touched by
 * the pump thread (below) and by a request while it holds the lock, which is what
 * makes a lock this coarse safe: nothing else runs GStreamer.
 */
typedef struct {
    char token[40];
    mi_rtc *rtc;
} rtc_slot;

typedef struct rtc_registry {
    pthread_mutex_t lock;
    rtc_slot *slots;
    size_t count, cap;
    pthread_t pump;
    int pump_running;
    int stop;
} rtc_registry;

static rtc_registry *g_rtc_reg = NULL;

static rtc_registry *rtc_registry_get(void) {
    if (g_rtc_reg) return g_rtc_reg;
    rtc_registry *r = mi_calloc(1, sizeof *r);
    if (!r) return NULL;
    pthread_mutex_init(&r->lock, NULL);
    g_rtc_reg = r;
    return r;
}

/*
 * The pump thread.
 *
 * `mi_rtc_pump` runs the session's GLib main context, which is what delivers
 * webrtcbin's signals and the data channel's messages. Nothing else does, and a
 * session that is not pumped does not negotiate, does not process input, and
 * never notices its browser has gone. Since the requests are short and
 * intermittent, this has to be a thread rather than something a request does.
 */
static void *rtc_pump_main(void *arg) {
    rtc_registry *r = arg;
    while (!r->stop) {
        pthread_mutex_lock(&r->lock);
        for (size_t i = 0; i < r->count; i++) {
            mi_rtc *rtc = r->slots[i].rtc;
            if (!rtc) continue;
            /*
             * A finished session is closed and removed here, not left for a
             * client to ask about.
             *
             * `mi_rtc_pump` returns -1 once the session has ended — an idle
             * timeout, a failed connection, a pipeline error — and nothing else
             * will ever call `mi_rtc_close` on it: the HTTP routes only close a
             * session the caller names, and a browser that simply navigates away
             * never does. Ignoring the return value leaked the gate slot *and* a
             * browser and an X server with it, so the next session was refused
             * as busy forever.
             */
            if (mi_rtc_pump(rtc, 5) != 0) {
                mi_rtc_close(rtc);
                r->slots[i] = r->slots[r->count - 1];
                r->count--;
                i--; /* the slot now holds the one that was last */
            }
        }
        pthread_mutex_unlock(&r->lock);
        struct timespec t = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000};
        nanosleep(&t, NULL);
    }
    return NULL;
}

static void rtc_registry_start(rtc_registry *r) {
    if (!r || r->pump_running) return;
    r->stop = 0;
    if (pthread_create(&r->pump, NULL, rtc_pump_main, r) == 0) r->pump_running = 1;
}

static void rtc_registry_stop(rtc_registry *r) {
    if (!r || !r->pump_running) return;
    r->stop = 1;
    pthread_join(r->pump, NULL);
    r->pump_running = 0;
}

/* Stop the pump thread and drop every session. Called on server shutdown. */
void rtc_registry_shutdown(void);
void rtc_registry_shutdown(void) {
    rtc_registry *r = g_rtc_reg;
    if (!r) return;
    rtc_registry_stop(r);
    pthread_mutex_lock(&r->lock);
    for (size_t i = 0; i < r->count; i++) {
        if (r->slots[i].rtc) mi_rtc_close(r->slots[i].rtc);
    }
    r->count = 0;
    pthread_mutex_unlock(&r->lock);
}

/* Add a session under a fresh token. Returns the token, or NULL. */
static char *rtc_registry_add(rtc_registry *r, mi_rtc *rtc) {
    if (!r) return NULL;
    pthread_mutex_lock(&r->lock);
    if (r->count == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 4;
        rtc_slot *grown = mi_realloc(r->slots, cap * sizeof *grown);
        if (!grown) { pthread_mutex_unlock(&r->lock); return NULL; }
        r->slots = grown;
        r->cap = cap;
    }
    char token[40];
    gen_id(token);
    snprintf(r->slots[r->count].token, sizeof r->slots[r->count].token, "%s", token);
    r->slots[r->count].rtc = rtc;
    r->count++;
    pthread_mutex_unlock(&r->lock);
    return mi_strdup(token);
}

/*
 * Look a session up, and remove it in the same breath.
 *
 * `take` is 1 for a close (the caller owns the session and must free it) and 0
 * for a peek (the caller only reads state, under the lock). Removing on close is
 * what keeps the table from growing without bound.
 */
static mi_rtc *rtc_registry_take(rtc_registry *r, const char *token, int take) {
    if (!r || !token) return NULL;
    mi_rtc *found = NULL;
    pthread_mutex_lock(&r->lock);
    for (size_t i = 0; i < r->count; i++) {
        if (strcmp(r->slots[i].token, token) != 0) continue;
        found = r->slots[i].rtc;
        if (take) {
            r->slots[i] = r->slots[r->count - 1];
            r->count--;
        }
        break;
    }
    pthread_mutex_unlock(&r->lock);
    return found;
}

/* Read a session's state under the lock, for a poll. */
static void rtc_registry_state(rtc_registry *r, const char *token, const char **state,
                               int *connected, const char **reason) {
    *state = "closed";
    *connected = 0;
    *reason = "";
    if (!r || !token) return;
    pthread_mutex_lock(&r->lock);
    for (size_t i = 0; i < r->count; i++) {
        if (strcmp(r->slots[i].token, token) != 0) continue;
        mi_rtc *rtc = r->slots[i].rtc;
        if (rtc) {
            *state = mi_rtc_state(rtc);
            *connected = mi_rtc_connected(rtc);
            *reason = mi_rtc_close_reason(rtc);
        }
        break;
    }
    pthread_mutex_unlock(&r->lock);
}

/* Read a request body as JSON, capped. Returns NULL on anything unusable. */
static mi_json *read_json_body(http_request *req) {
    if (!req->body || req->body_len == 0) return NULL;
    return mi_json_parse(req->body);
}

/*
 * POST /api/v1/rtc  {url}
 *
 * Opens a session and returns its offer once ICE gathering has finished. The
 * wait is the price of having no socket to trickle candidates on; see mi_rtc.h.
 */
static void handle_rtc_open(mi_server *srv, int fd, http_request *req, const char *cors) {
    mi_json *body = read_json_body(req);
    if (!body) {
        send_error(fd, 400, "Bad Request", "expected a JSON body with a url", cors);
        return;
    }
    const char *url = mi_json_get_string(body, "url", NULL);
    if (!url || !*url) {
        mi_json_value_free(body);
        send_error(fd, 400, "Bad Request", "missing 'url'", cors);
        return;
    }
    if (!srv->rtc) {
        mi_json_value_free(body);
        send_error(fd, 503, "Service Unavailable",
                   "live video is not enabled on this Vyzor", cors);
        return;
    }
    if (!mi_rtc_pool_available(srv->rtc)) {
        mi_json_value_free(body);
        send_error(fd, 503, "Service Unavailable",
                   "no browser, Xvfb or GStreamer webrtc plugins on Vyzor host", cors);
        return;
    }

    char err[200];
    mi_rtc *rtc = NULL;
    mi_rtc_status st = mi_rtc_open(srv->rtc, url, &rtc, err, sizeof err);
    if (st != MI_RTC_OK) {
        mi_json_value_free(body);
        int code = 502;
        const char *text = "Bad Gateway";
        if (st == MI_RTC_BUSY) { code = 503; text = "Service Unavailable"; }
        else if (st == MI_RTC_BAD_URL) { code = 400; text = "Bad Request"; }
        else if (st == MI_RTC_NO_PLUGIN) { code = 503; text = "Service Unavailable"; }
        send_error(fd, code, text, err[0] ? err : "could not start a live video session", cors);
        return;
    }

    /* The offer is only final once gathering has settled, so wait for it here. */
    if (mi_rtc_wait_offer(rtc, 0) != 0) {
        const char *why = mi_rtc_close_reason(rtc);
        mi_rtc_close(rtc);
        mi_json_value_free(body);
        send_error(fd, 504, "Gateway Timeout",
                   (why && *why) ? why : "the browser produced no offer in time", cors);
        return;
    }

    char *offer = mi_rtc_offer(rtc);
    if (!offer) {
        mi_rtc_close(rtc);
        mi_json_value_free(body);
        send_error(fd, 502, "Bad Gateway", "the offer was empty", cors);
        return;
    }

    rtc_registry *reg = rtc_registry_get();
    char *token = rtc_registry_add(reg, rtc);
    if (!token) {
        free(offer);
        mi_rtc_close(rtc);
        mi_json_value_free(body);
        send_error(fd, 500, "Internal Server Error", "out of memory", cors);
        return;
    }
    /* The pump has to be running before the answer arrives, or the connection
     * would not be driven. */
    rtc_registry_start(reg);

    int vw = 0, vh = 0;
    mi_rtc_viewport(rtc, &vw, &vh);

    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"token\":");
    mi_json_string(&b, token);
    mi_json_raw(&b, ",\"sdp\":");
    mi_json_string(&b, offer);
    mi_json_raw(&b, ",\"width\":");
    mi_json_int(&b, vw);
    mi_json_raw(&b, ",\"height\":");
    mi_json_int(&b, vh);
    mi_json_raw(&b, ",\"gathered\":");
    mi_json_bool(&b, mi_rtc_gathering_done(rtc));
    mi_json_raw(&b, "}");
    send_json(fd, 200, "OK", &b, cors);
    mi_json_free(&b);

    free(token);
    free(offer);
    mi_json_value_free(body);
}

/* POST /api/v1/rtc/{token}/answer  {sdp} */
static void handle_rtc_answer(mi_server *srv, int fd, http_request *req,
                              const char *token, const char *cors) {
    (void)srv;
    mi_json *body = read_json_body(req);
    if (!body) {
        send_error(fd, 400, "Bad Request", "expected a JSON body with an sdp", cors);
        return;
    }
    const char *sdp = mi_json_get_string(body, "sdp", NULL);
    if (!sdp || !*sdp) {
        mi_json_value_free(body);
        send_error(fd, 400, "Bad Request", "missing 'sdp'", cors);
        return;
    }

    rtc_registry *reg = rtc_registry_get();
    mi_rtc *rtc = rtc_registry_take(reg, token, 0);
    if (!rtc) {
        mi_json_value_free(body);
        send_error(fd, 404, "Not Found", "no such session", cors);
        return;
    }

    pthread_mutex_lock(&reg->lock);
    int ok = mi_rtc_set_answer(rtc, sdp) == 0;
    pthread_mutex_unlock(&reg->lock);
    mi_json_value_free(body);

    if (!ok) {
        send_error(fd, 400, "Bad Request", "the answer was not valid SDP", cors);
        return;
    }
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"ok\":true}");
    send_json(fd, 200, "OK", &b, cors);
    mi_json_free(&b);
}

/* GET /api/v1/rtc/{token} */
static void handle_rtc_status(mi_server *srv, int fd, const char *token, const char *cors) {
    (void)srv;
    rtc_registry *reg = rtc_registry_get();
    const char *state = "closed", *reason = "";
    int connected = 0;
    rtc_registry_state(reg, token, &state, &connected, &reason);
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"state\":");
    mi_json_string(&b, state);
    mi_json_raw(&b, ",\"connected\":");
    mi_json_bool(&b, connected);
    mi_json_raw(&b, ",\"reason\":");
    mi_json_string(&b, reason);
    mi_json_raw(&b, "}");
    send_json(fd, 200, "OK", &b, cors);
    mi_json_free(&b);
}

/* DELETE /api/v1/rtc/{token} */
static void handle_rtc_close(mi_server *srv, int fd, const char *token, const char *cors) {
    (void)srv;
    rtc_registry *reg = rtc_registry_get();
    mi_rtc *rtc = rtc_registry_take(reg, token, 1);
    if (rtc) mi_rtc_close(rtc); /* closes the browser, the display and the pipeline */
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"ok\":true}");
    send_json(fd, 200, "OK", &b, cors);
    mi_json_free(&b);
}

static void serve_interactive_ws(mi_server *srv, int fd, http_request *req) {
    /* The handshake. A browser's WebSocket client does not send a frame until
     * it has seen the 101, so nothing is lost from having read only the head. */
    const char *key = request_header(req, "Sec-WebSocket-Key");
    const char *upgrade = request_header(req, "Upgrade");
    const char *connection = request_header(req, "Connection");
    if (!mi_ws_is_upgrade(upgrade, connection, key)) {
        send_error(fd, 400, "Bad Request", "expected a WebSocket upgrade", NULL);
        return;
    }
    /* The Origin is checked here too, because a WebSocket handshake is not
     * subject to CORS: without this, any page a user visits could open a
     * session to their Vyzor. */
    const char *origin = request_header(req, "Origin");
    if (srv->cfg.allow_origins && origin && !origin_allowed(srv->cfg.allow_origins, origin)) {
        send_error(fd, 403, "Forbidden", "origin is not allowed", NULL);
        return;
    }

    char *url = query_str(req->query, "url");
    if (!url || !*url) {
        free(url);
        send_error(fd, 400, "Bad Request", "missing 'url'", NULL);
        return;
    }

    if (!srv->driver) {
        send_error(fd, 503, "Service Unavailable",
                   "live sessions are not enabled on this Vyzor", NULL);
        free(url);
        return;
    }
    if (!mi_interactive_pool_available(srv->driver)) {
        send_error(fd, 503, "Service Unavailable",
                   "no browser is installed on Vyzor host", NULL);
        free(url);
        return;
    }

    int width = query_int(req->query, "w", srv->cfg.render_viewport_width);
    int height = query_int(req->query, "h", srv->cfg.render_viewport_height);
    /* A touch device says so, and the browser emulates touch to match. */
    int touch = query_int(req->query, "touch", 0) != 0;

    char err[200];
    mi_interactive *it = NULL;
    mi_it_status st = mi_interactive_open(srv->driver, url, width, height, touch, &it,
                                          err, sizeof err);
    if (st != MI_IT_OK) {
        int code = 502;
        const char *text = "Bad Gateway";
        if (st == MI_IT_BUSY) { code = 503; text = "Service Unavailable"; }
        else if (st == MI_IT_BAD_URL) { code = 400; text = "Bad Request"; }
        else if (st == MI_IT_NO_BROWSER) { code = 503; text = "Service Unavailable"; }
        send_error(fd, code, text, err[0] ? err : "could not open a live session", NULL);
        free(url);
        return;
    }

    /* Complete the upgrade only once the browser is actually up, so a failure is
     * an HTTP status the console can read rather than a socket that opens and
     * immediately dies. */
    char *accept = mi_ws_accept_key(key);
    if (!accept) {
        mi_interactive_close(it);
        free(url);
        send_error(fd, 500, "Internal Server Error", "out of memory", NULL);
        return;
    }
    char handshake[512];
    int hn = snprintf(handshake, sizeof handshake,
                      "HTTP/1.1 101 Switching Protocols\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: %s\r\n\r\n",
                      accept);
    free(accept);
    send_all(fd, handshake, (size_t)hn);

    /*
     * Everything from here on goes through the outbox rather than straight to
     * the socket.
     *
     * A live session writes far more than the socket can always take — a picture
     * is tens of kilobytes and the page may paint faster than the link drains —
     * so the queue is what makes the difference between a session that degrades
     * and one that dies. A full socket becomes a dropped stale picture, not a
     * closed connection.
     */
    mi_outbox *out = mi_outbox_new(0); /* the built-in bound */
    if (!out) {
        mi_interactive_close(it);
        free(url);
        return;
    }

    /* Tell the console the page is live, then stream. */
    {
        mi_json_buf b;
        mi_json_init(&b);
        mi_json_raw(&b, "{\"type\":\"ready\",\"url\":");
        mi_json_string(&b, url);
        mi_json_raw(&b, ",\"width\":");
        mi_json_int(&b, width);
        mi_json_raw(&b, ",\"height\":");
        mi_json_int(&b, height);
        mi_json_raw(&b, "}");
        ws_queue_text(out, mi_json_data(&b), b.len, 0);
        mi_json_free(&b);
    }

    /* Non-blocking, so the poll loop can watch both directions. */
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    unsigned char rbuf[65536];
    size_t rlen = 0;
    int seq = 0;
    int alive = 1;

    while (alive) {
        /*
         * Watch both directions. POLLOUT is only asked for when something is
         * waiting, so an idle session does not wake on every writable socket.
         */
        short want = POLLIN;
        if (!mi_outbox_empty(out)) want |= POLLOUT;
        struct pollfd p = {.fd = fd, .events = want};
        int pr = poll(&p, 1, 10);
        if (pr > 0 && (p.revents & (POLLIN | POLLHUP))) {
            ssize_t n = recv(fd, rbuf + rlen, sizeof rbuf - rlen, 0);
            if (n > 0) {
                rlen += (size_t)n;
                /* Any bytes at all — a pong included — mean the console is still
                 * there. This is what the keepalive below is judged against. */
                mi_interactive_note_heard(it);
            } else if (n == 0) {
                alive = 0;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                alive = 0;
            }
        }
        /* Drain the queue whenever the socket will take more. A failure here
         * means the peer is gone, which is the only case that ends the session. */
        if (alive && mi_outbox_flush(out, fd) != 0) alive = 0;

        /*
         * The keepalive. A tab closed behind a proxy can leave a connection that
         * never reports EOF, and a session held by a console that is gone is a
         * browser nobody can use and a gate nobody can pass. Ping when the socket
         * has been quiet, and give up on a client that has not answered for a
         * while — a peer that is really there answers a ping promptly.
         */
        if (mi_interactive_should_ping(it)) {
            mi_interactive_note_ping(it);
            if (mi_ws_send(fd, MI_WS_PING, NULL, 0) != 0) alive = 0;
        }
        if (mi_interactive_silence(it) > (double)mi_interactive_silence_limit(it)) {
            alive = 0;
        }

        /* Consume whole frames from the buffer. */
        size_t consumed = 0;
        while (rlen - consumed >= 2) {
            mi_ws_frame f;
            ssize_t used = mi_ws_parse(rbuf + consumed, rlen - consumed, &f);
            if (used == 0) break;
            if (used < 0) { alive = 0; break; }
            consumed += (size_t)used;

            if (f.opcode == MI_WS_CLOSE) {
                /* Tear the session down *before* acknowledging the close, so the
                 * concurrency slot is free by the time the console's close event
                 * fires. Otherwise a console that replaces a session promptly —
                 * opening a new page while one is live — is refused as busy even
                 * though this one is already going away. */
                mi_interactive_close(it);
                it = NULL;
                mi_ws_send(fd, MI_WS_CLOSE, NULL, 0);
                mi_ws_frame_free(&f);
                alive = 0;
                break;
            } else if (f.opcode == MI_WS_PING) {
                /* A pong goes out at once rather than through the queue: it is
                 * tiny, and a delayed keepalive answer is worse than useless. */
                mi_ws_send(fd, MI_WS_PONG, f.payload, f.len);
            } else if (f.opcode == MI_WS_PONG) {
                /* Liveness only; nothing to do. */
            } else if (f.opcode == MI_WS_TEXT) {
                mi_json *m = mi_json_parse((const char *)f.payload);
                if (m) {
                    ws_dispatch(it, m);
                    mi_json_value_free(m);
                } else {
                    mi_json_buf b;
                    mi_json_init(&b);
                    mi_json_raw(&b, "{\"type\":\"error\",\"message\":");
                    mi_json_string(&b, "a command was not valid JSON");
                    mi_json_raw(&b, "}");
                    ws_queue_text(out, mi_json_data(&b), b.len, 0);
                    mi_json_free(&b);
                }
            }
            mi_ws_frame_free(&f);
        }
        if (consumed > 0) {
            memmove(rbuf, rbuf + consumed, rlen - consumed);
            rlen -= consumed;
        }
        if (!alive) break;

        /* Then whatever the browser has for us. */
        mi_it_event ev = mi_interactive_pump(it, 10);
        if (ev == MI_IT_EV_FRAME) {
            size_t len = 0;
            const unsigned char *frame = mi_interactive_frame(it, &len);
            if (frame && len > 0) {
                /* Droppable: a newer picture supersedes this one, so a slow link
                 * loses a frame rather than falling behind. */
                ws_queue_image(out, "frame", mi_interactive_frame_format(it),
                               mi_interactive_frame_width(it), mi_interactive_frame_height(it),
                               ++seq, frame, len);
            }
        } else if (ev == MI_IT_EV_SHOT) {
            size_t len = 0;
            const unsigned char *frame = mi_interactive_frame(it, &len);
            if (frame && len > 0) {
                /* A still was asked for explicitly, so it is not droppable: the
                 * user pressed a button and expects that picture. */
                ws_queue_image(out, "shot", mi_interactive_frame_format(it),
                               mi_interactive_frame_width(it), mi_interactive_frame_height(it),
                               ++seq, frame, len);
            }
        } else if (ev == MI_IT_EV_MEDIA) {
            /* The set of things to download changed. Sent whole rather than as a
             * delta: the list is short, and a client that joined late or missed a
             * message is then never out of step. */
            const char *list = mi_interactive_media_json(it);
            mi_json_buf b;
            mi_json_init(&b);
            mi_json_raw(&b, "{\"type\":\"media\",\"items\":");
            mi_json_raw(&b, list ? list : "[]");
            mi_json_raw(&b, "}");
            ws_queue_text(out, mi_json_data(&b), b.len, 0);
            mi_json_free(&b);
        } else if (ev == MI_IT_EV_STATE) {
            const char *state = mi_interactive_state(it);
            if (state) {
                /* The state is already a JSON object; wrap it with its type.
                 * Never droppable: it carries information, not a moment. */
                mi_json_buf b;
                mi_json_init(&b);
                mi_json_raw(&b, "{\"type\":\"state\",\"page\":");
                mi_json_raw(&b, state);
                mi_json_raw(&b, "}");
                ws_queue_text(out, mi_json_data(&b), b.len, 0);
                mi_json_free(&b);
            }
        } else if (ev == MI_IT_EV_CLOSED) {
            const char *why = mi_interactive_close_reason(it);
            mi_json_buf b;
            mi_json_init(&b);
            mi_json_raw(&b, "{\"type\":\"closed\",\"reason\":");
            mi_json_string(&b, why);
            mi_json_raw(&b, "}");
            ws_queue_text(out, mi_json_data(&b), b.len, 0);
            mi_json_free(&b);
            /* Give the farewell a moment to leave before the socket closes. */
            for (int i = 0; i < 20 && !mi_outbox_empty(out); i++) {
                if (mi_outbox_flush(out, fd) != 0) break;
                struct timespec t = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
                nanosleep(&t, NULL);
            }
            alive = 0;
        }
    }

    /*
     * How the stream actually behaved, for the log.
     *
     * These are the numbers that say whether the transport is the problem: a
     * rising `would_block` means the link could not take the pictures as fast as
     * the page produced them, and `dropped` says how many were skipped to keep
     * the view current. Silence here means the link kept up.
     */
    mi_logf(MI_LOG_INFO, "live: %s ended (%s); frames dropped %llu, socket full %llu",
            url, mi_interactive_close_reason(it),
            (unsigned long long)mi_outbox_dropped(out),
            (unsigned long long)mi_outbox_would_block(out));
    mi_outbox_free(out);
    mi_interactive_close(it);
    free(url);
}

static void handle_request(mi_server *srv, int fd, http_request *req) {
    const char *cors = cors_origin_for(srv, req);

    if (!strcmp(req->method, "OPTIONS")) {
        send_response(fd, 204, "No Content", "text/plain", "", 0, cors);
        return;
    }

    if (!strcmp(req->path, "/healthz")) {
        mi_json_buf b;
        mi_json_init(&b);
        mi_json_raw(&b, "{\"status\":\"ok\"}");
        send_json(fd, 200, "OK", &b, cors);
        mi_json_free(&b);
        return;
    }

    if (strncmp(req->path, "/api/", 5) != 0) {
        send_error(fd, 404, "Not Found", "unknown path", cors);
        return;
    }

    /*
     * Live video over WebRTC, signalled over plain HTTP.
     *
     * There is no WebSocket here on purpose: the offer, the answer and the
     * state are one request each, and input rides a WebRTC data channel. A
     * session is identified by a token Vyzor mints, which is why these
     * routes are handled before the bearer check — a browser cannot put an
     * Authorization header on a WebRTC connection's setup, and the token is
     * the credential.
     */
    if (!strcmp(req->path, "/api/v1/rtc")) {
        if (srv->cfg.auth_token && !authorized(srv, req)) {
            send_error(fd, 401, "Unauthorized", "missing or invalid token", cors);
            return;
        }
        if (!strcmp(req->method, "POST")) {
            handle_rtc_open(srv, fd, req, cors);
            return;
        }
        send_error(fd, 405, "Method Not Allowed", "use POST to open a session", cors);
        return;
    }
    if (!strncmp(req->path, "/api/v1/rtc/", 12)) {
        if (srv->cfg.auth_token && !authorized(srv, req)) {
            send_error(fd, 401, "Unauthorized", "missing or invalid token", cors);
            return;
        }
        /* /api/v1/rtc/{token} and /api/v1/rtc/{token}/answer */
        const char *rest = req->path + 12;
        const char *slash = strchr(rest, '/');
        char token[64];
        size_t tlen = slash ? (size_t)(slash - rest) : strlen(rest);
        if (tlen == 0 || tlen >= sizeof token) {
            send_error(fd, 404, "Not Found", "unknown path", cors);
            return;
        }
        memcpy(token, rest, tlen);
        token[tlen] = '\0';
        const char *sub = slash ? slash + 1 : "";
        if (!strcmp(sub, "answer") && !strcmp(req->method, "POST")) {
            handle_rtc_answer(srv, fd, req, token, cors);
            return;
        }
        if (!*sub && !strcmp(req->method, "GET")) {
            handle_rtc_status(srv, fd, token, cors);
            return;
        }
        if (!*sub && !strcmp(req->method, "DELETE")) {
            handle_rtc_close(srv, fd, token, cors);
            return;
        }
        send_error(fd, 405, "Method Not Allowed", "unsupported method", cors);
        return;
    }

    /*
     * GET /api/v1/interactive?url=…  (WebSocket upgrade)
     *
     * Handled before the generic bearer check, because a browser cannot put an
     * Authorization header on a WebSocket. The token may instead arrive as a
     * `token` query parameter; the Origin check inside the handler is the other
     * half of the defence, since a WebSocket handshake is not subject to CORS.
     */
    if (!strcmp(req->path, "/api/v1/interactive") && !strcmp(req->method, "GET")) {
        if (srv->cfg.auth_token) {
            const char *auth = request_header(req, "Authorization");
            char *qtok = query_str(req->query, "token");
            int ok = (auth && !strncmp(auth, "Bearer ", 7) &&
                      !strcmp(auth + 7, srv->cfg.auth_token)) ||
                     (qtok && !strcmp(qtok, srv->cfg.auth_token));
            free(qtok);
            if (!ok) {
                send_error(fd, 401, "Unauthorized", "missing or invalid token", cors);
                return;
            }
        }
        serve_interactive_ws(srv, fd, req);
        return;
    }

    if (!authorized(srv, req)) {
        send_error(fd, 401, "Unauthorized", "missing or invalid bearer token", cors);
        return;
    }

    /* GET /api/v1/capabilities */
    if (!strcmp(req->path, "/api/v1/capabilities") && !strcmp(req->method, "GET")) {
        mi_json_buf b;
        mi_json_init(&b);
        mi_json_raw(&b, "{\"version\":\"1\",\"detectors\":[");
        const char *kinds[] = {"mp4", "matroska", "avi", "flv", "ogg", "asf",
                               "mpeg-ts", "mpeg-ps", "hls", "dash"};
        for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
            if (i) mi_json_raw(&b, ",");
            mi_json_string(&b, kinds[i]);
        }
        mi_json_raw(&b, "],\"hls\":true,\"upload\":true,\"purge\":true");
        mi_json_raw(&b, ",\"max_body_bytes\":");
        mi_json_int(&b, srv->cfg.max_body_bytes);
        mi_json_raw(&b, ",\"workers\":");
        mi_json_int(&b, srv->cfg.workers);
        mi_json_raw(&b, ",\"vyzor_version\":");
        mi_json_string(&b, MI_VYZOR_VERSION);
        /* Whether transcoding is actually possible is a property of this host,
         * so it is probed rather than asserted. */
        mi_json_raw(&b, ",\"ffmpeg\":");
        mi_json_bool(&b, mi_which("ffmpeg"));
        /* Whether a page can be rendered here, and within what bounds. Reported
         * rather than assumed, so a console can hide the control on a host that
         * cannot do it instead of offering a button that fails. */
        mi_json_raw(&b, ",\"render\":");
        mi_json_raw(&b, "{");
        mi_json_key(&b, "enabled");
        mi_json_bool(&b, srv->renderer != NULL);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "available");
        mi_json_bool(&b, srv->renderer && mi_render_pool_available(srv->renderer));
        mi_json_raw(&b, ",");
        mi_json_key(&b, "max_concurrent");
        mi_json_int(&b, srv->renderer ? mi_render_pool_limit(srv->renderer) : 0);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "timeout_seconds");
        mi_json_int(&b, srv->cfg.render_timeout_seconds);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "viewport_width");
        mi_json_int(&b, srv->cfg.render_viewport_width);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "viewport_height");
        mi_json_int(&b, srv->cfg.render_viewport_height);
        mi_json_raw(&b, ",");
        /* Whether a repeat view is cheap, and how much is being kept. Reported
         * so an operator can see the cache working rather than trusting it. */
        mi_json_key(&b, "cache");
        mi_json_bool(&b, mi_render_pool_caching(srv->renderer));
        mi_json_raw(&b, ",");
        mi_json_key(&b, "cache_entries");
        {
            size_t entries = 0, bytes = 0;
            uint64_t hits = 0, misses = 0;
            mi_render_pool_cache_stats(srv->renderer, &entries, &bytes, &hits, &misses);
            mi_json_int(&b, (int64_t)entries);
            mi_json_raw(&b, ",");
            mi_json_key(&b, "cache_bytes");
            mi_json_int(&b, (int64_t)bytes);
            mi_json_raw(&b, ",");
            mi_json_key(&b, "cache_hits");
            mi_json_int(&b, (int64_t)hits);
            mi_json_raw(&b, ",");
            mi_json_key(&b, "cache_misses");
            mi_json_int(&b, (int64_t)misses);
        }
        mi_json_raw(&b, "}");
        /* Whether a page can be driven live, and within what bounds. Separate
         * from rendering because it is a separate opt-in and a separate cost. */
        mi_json_raw(&b, ",\"interactive\":");
        mi_json_raw(&b, "{");
        mi_json_key(&b, "enabled");
        mi_json_bool(&b, srv->driver != NULL);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "available");
        mi_json_bool(&b, mi_server_can_drive(srv));
        mi_json_raw(&b, ",");
        mi_json_key(&b, "max_concurrent");
        mi_json_int(&b, srv->driver ? mi_interactive_pool_limit(srv->driver) : 0);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "active");
        mi_json_int(&b, srv->driver ? mi_interactive_pool_active(srv->driver) : 0);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "idle_timeout_seconds");
        mi_json_int(&b, srv->cfg.interactive_idle_seconds);
        mi_json_raw(&b, "}");
        /* Whether the page can be streamed as live video, and within what
         * bounds. Its own opt-in because it costs a display and a browser. */
        mi_json_raw(&b, ",\"rtc\":");
        mi_json_raw(&b, "{");
        mi_json_key(&b, "enabled");
        mi_json_bool(&b, srv->rtc != NULL);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "available");
        mi_json_bool(&b, srv->rtc != NULL && mi_rtc_pool_available(srv->rtc));
        mi_json_raw(&b, ",");
        mi_json_key(&b, "max_concurrent");
        mi_json_int(&b, srv->rtc ? mi_rtc_pool_limit(srv->rtc) : 0);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "active");
        mi_json_int(&b, srv->rtc ? mi_rtc_pool_active(srv->rtc) : 0);
        mi_json_raw(&b, "}");
        /*
         * The media extractor.
         *
         * Reported so the console never offers a control that cannot work: a
         * host without yt-dlp installs no "find a video" tab, rather than showing
         * one that fails. `available` is the answer that matters; `enabled` says
         * whether it was asked for at all.
         */
        mi_extract_config ecfg;
        mi_extract_config_init(&ecfg);
        ecfg.binary = srv->cfg.extractor_path;
        ecfg.gallery_binary = srv->cfg.gallery_extractor_path;
        mi_json_raw(&b, ",\"extract\":");
        mi_json_raw(&b, "{");
        mi_json_key(&b, "enabled");
        mi_json_bool(&b, 1);
        mi_json_raw(&b, ",");
        /*
         * The two extractors are reported separately.
         *
         * They are independent — a host may have yt-dlp and not gallery-dl — and
         * the console offers a different mode for each, so one flag would make it
         * offer a control that cannot work.
         */
        mi_json_key(&b, "available");
        mi_json_bool(&b, mi_extract_available_kind(&ecfg, MI_EXTRACT_VIDEO));
        mi_json_raw(&b, ",");
        mi_json_key(&b, "gallery_available");
        mi_json_bool(&b, mi_extract_available_kind(&ecfg, MI_EXTRACT_GALLERY));
        mi_json_raw(&b, "}");
        mi_json_raw(&b, "}");
        send_json(fd, 200, "OK", &b, cors);
        mi_json_free(&b);
        return;
    }

    /* POST /api/v1/probe  {url} */
    if (!strcmp(req->path, "/api/v1/probe") && !strcmp(req->method, "POST")) {
        mi_json *in = req->body ? mi_json_parse(req->body) : NULL;
        const char *url = mi_json_get_string(in, "url", NULL);
        if (!url || !*url) {
            send_error(fd, 400, "Bad Request", "missing or empty 'url'", cors);
            mi_json_value_free(in);
            return;
        }
        mi_download_config cfg;
        mi_download_config_init(&cfg);
        cfg.require_media = 0;
        cfg.user_agent = srv->cfg.user_agent;
        mi_download_plan plan;
        if (mi_probe(&cfg, url, &plan) != MI_OK) {
            send_error(fd, 502, "Bad Gateway", "probe failed", cors);
            mi_json_value_free(in);
            return;
        }

        /* For HLS/DASH, fetch the manifest body and parse the selectable
         * renditions so the UI can offer real quality options with real sizes.
         * `duration_seconds` may be supplied by the caller (it usually knows
         * the title length); without it sizes stay unknown rather than guessed. */
        double duration = mi_json_get_number(in, "duration_seconds", 0);
        if (plan.have_media &&
            (plan.media.kind == MI_KIND_HLS || plan.media.kind == MI_KIND_DASH)) {
            char *body = mi_download_fetch_text(&cfg, plan.final_url ? plan.final_url : url,
                                                512 * 1024);
            if (body) {
                size_t blen = strlen(body);
                if (plan.media.kind == MI_KIND_HLS) {
                    mi_parse_hls_renditions(body, blen, duration, &plan.media);
                } else {
                    mi_parse_dash_renditions(body, blen, duration, &plan.media);
                }
                free(body);
            }
        }
        mi_json_buf b;
        mi_json_init(&b);
        mi_json_raw(&b, "{\"url\":");
        mi_json_string(&b, url);
        mi_json_raw(&b, ",\"final_url\":");
        mi_json_string(&b, plan.final_url);
        mi_json_raw(&b, ",\"content_type\":");
        mi_json_string(&b, plan.content_type);
        mi_json_raw(&b, ",\"total_bytes\":");
        mi_json_int(&b, plan.total_bytes);
        mi_json_raw(&b, ",\"accept_ranges\":");
        mi_json_bool(&b, plan.accept_ranges);
        mi_json_raw(&b, ",\"media\":");
        if (plan.have_media) {
            media_to_json(&plan.media, &b);
        } else {
            mi_json_null(&b);
        }
        mi_json_raw(&b, "}");
        send_json(fd, 200, "OK", &b, cors);
        mi_json_free(&b);
        mi_download_plan_free(&plan);
        mi_json_value_free(in);
        return;
    }

    /* POST /api/v1/extract  {url} — the media URLs a page will not link.
     *
     * The crawl's browser fallback covers a page that builds its list in
     * JavaScript. It does not cover a page whose media URL is *signed* — the
     * player computes it at play time, so it is in neither the HTML nor the
     * browser's traffic until something asks for it properly. A streaming site
     * is the case, and the extractor is what resolves it.
     *
     * Resolve only: the URLs come back and a download is queued separately, so
     * the extractor never fetches the media itself. */
    if (!strcmp(req->path, "/api/v1/extract") && !strcmp(req->method, "POST")) {
        mi_json *in = req->body ? mi_json_parse(req->body) : NULL;
        const char *url = mi_json_get_string(in, "url", NULL);
        const char *term = mi_json_get_string(in, "search", NULL);
        /*
         * Either a URL or a search. They are different questions — "resolve this
         * address" and "find something matching this" — and a caller supplies
         * one or the other, never both.
         */
        int searching = term && *term;
        if ((!url || !*url) && !searching) {
            send_error(fd, 400, "Bad Request", "give either 'url' or 'search'", cors);
            mi_json_value_free(in);
            return;
        }
        if (url && *url && !mi_render_url_allowed(url)) {
            send_error(fd, 400, "Bad Request", "that address cannot be read", cors);
            mi_json_value_free(in);
            return;
        }

        mi_extract_config ecfg;
        mi_extract_config_init(&ecfg);
        ecfg.binary = srv->cfg.extractor_path;
        ecfg.gallery_binary = srv->cfg.gallery_extractor_path;
        double timeout = mi_json_get_number(in, "timeout_seconds", 0);
        if (timeout > 0) ecfg.timeout_seconds = timeout;
        if (searching) {
            ecfg.search_term = term;
            int limit = (int)mi_json_get_number(in, "limit", 0);
            if (limit > 0) ecfg.search_limit = limit;
        }
        ecfg.enumerate = mi_json_get_bool(in, "enumerate", 0);
        /*
         * The discovery filters, passed through in the extractor's own syntax.
         *
         * Vyzor does not parse them: `duration > 600` and `1:5` are the
         * extractor's language, and re-spelling them here would mean tracking
         * that language. The caller that knows what it wants says it, and the
         * extractor applies it while listing.
         */
        ecfg.date_after = mi_json_get_string(in, "date_after", NULL);
        ecfg.date_before = mi_json_get_string(in, "date_before", NULL);
        ecfg.playlist_items = mi_json_get_string(in, "playlist_items", NULL);
        ecfg.match_filter = mi_json_get_string(in, "match_filter", NULL);
        const mi_json *xargs = mi_json_get(in, "extractor_args");
        const char *xargv[8];
        size_t xargc = 0;
        if (xargs && xargs->type == MI_JSON_ARRAY) {
            for (size_t i = 0; i < xargs->count && xargc < 8; i++) {
                const mi_json *el = xargs->items[i];
                if (el && el->type == MI_JSON_STRING && el->string && *el->string) {
                    xargv[xargc++] = el->string;
                }
            }
        }
        if (xargc > 0) {
            ecfg.extractor_args = xargv;
            ecfg.extractor_arg_count = xargc;
        }

        mi_extract_result er;
        int rc = mi_extract(&ecfg, url, &er);

        mi_json_buf b;
        mi_json_init(&b);
        mi_json_raw(&b, "{");
        mi_json_key(&b, "ok");
        mi_json_bool(&b, rc == MI_OK);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "url");
        mi_json_string(&b, url);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "available");
        mi_json_bool(&b, mi_extract_available(&ecfg));
        mi_json_raw(&b, ",");
        mi_json_key(&b, "title");
        mi_json_string(&b, er.title);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "extractor");
        mi_json_string(&b, er.extractor);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "duration_seconds");
        mi_json_double(&b, er.duration_seconds);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "thumbnail");
        mi_json_string(&b, er.thumbnail);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "format_count");
        mi_json_int(&b, (int64_t)er.format_count);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "entry_count");
        mi_json_int(&b, (int64_t)er.entry_count);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "entries");
        mi_json_raw(&b, "[");
        for (size_t i = 0; i < er.entry_count; i++) {
            const mi_extract_entry *e = &er.entries[i];
            if (i) mi_json_raw(&b, ",");
            mi_json_raw(&b, "{\"id\":");
            mi_json_string(&b, e->id);
            mi_json_raw(&b, ",\"title\":");
            mi_json_string(&b, e->title);
            mi_json_raw(&b, ",\"url\":");
            mi_json_string(&b, e->url);
            mi_json_raw(&b, ",\"uploader\":");
            mi_json_string(&b, e->uploader);
            mi_json_raw(&b, ",\"thumbnail\":");
            mi_json_string(&b, e->thumbnail);
            mi_json_raw(&b, ",\"duration_seconds\":");
            mi_json_double(&b, e->duration_seconds);
            mi_json_raw(&b, ",\"is_playlist\":");
            mi_json_bool(&b, e->is_playlist);
            mi_json_raw(&b, "}");
        }
        mi_json_raw(&b, "]");
        mi_json_raw(&b, ",");
        mi_json_key(&b, "formats");
        mi_json_raw(&b, "[");
        for (size_t i = 0; i < er.format_count; i++) {
            const mi_extract_format *f = &er.formats[i];
            if (i) mi_json_raw(&b, ",");
            mi_json_raw(&b, "{\"url\":");
            mi_json_string(&b, f->url);
            mi_json_raw(&b, ",\"format_id\":");
            mi_json_string(&b, f->format_id);
            mi_json_raw(&b, ",\"ext\":");
            mi_json_string(&b, f->ext);
            mi_json_raw(&b, ",\"resolution\":");
            mi_json_string(&b, f->resolution);
            mi_json_raw(&b, ",\"vcodec\":");
            mi_json_string(&b, f->vcodec);
            mi_json_raw(&b, ",\"acodec\":");
            mi_json_string(&b, f->acodec);
            mi_json_raw(&b, ",\"mime\":");
            mi_json_string(&b, f->mime);
            mi_json_raw(&b, ",\"filesize\":");
            mi_json_int(&b, f->filesize);
            mi_json_raw(&b, ",\"bitrate\":");
            mi_json_double(&b, f->bitrate);
            mi_json_raw(&b, ",\"height\":");
            mi_json_int(&b, f->height);
            mi_json_raw(&b, ",\"has_video\":");
            mi_json_bool(&b, f->has_video);
            mi_json_raw(&b, ",\"has_audio\":");
            mi_json_bool(&b, f->has_audio);
            mi_json_raw(&b, "}");
        }
        mi_json_raw(&b, "]}");
        send_json(fd, 200, "OK", &b, cors);
        mi_json_free(&b);
        mi_extract_result_free(&er);
        mi_json_value_free(in);
        return;
    }

    /* POST /api/v1/crawl  {url, term, ...} — walk a site for files matching a name.
     *
     * The console cannot do this itself: the browser enforces CORS, and a crawl
     * needs to read many pages on another host. Vyzor has no such limit, so the
     * walk happens here.
     *
     * This is a *blocking* call, unlike a download. A crawl is bounded by its own
     * budget (a few tens of seconds) and the caller is waiting for the list, so
     * a job-and-poll dance would add machinery for no gain. The budget is what
     * keeps it from becoming an unbounded request. */
    if (!strcmp(req->path, "/api/v1/crawl") && !strcmp(req->method, "POST")) {
        mi_json *in = req->body ? mi_json_parse(req->body) : NULL;
        const char *url = mi_json_get_string(in, "url", NULL);
        if (!url || !*url) {
            send_error(fd, 400, "Bad Request", "missing or empty 'url'", cors);
            mi_json_value_free(in);
            return;
        }
        if (!mi_render_url_allowed(url)) {
            send_error(fd, 400, "Bad Request", "that address cannot be crawled", cors);
            mi_json_value_free(in);
            return;
        }
        const char *term = mi_json_get_string(in, "term", NULL);

        mi_crawl_config ccfg;
        mi_crawl_config_init(&ccfg);
        ccfg.user_agent = srv->cfg.user_agent;
        ccfg.render_pool = srv->renderer;
        /*
         * The interactive pool is preferred for the fallback: a session can be
         * driven — wait for the network to settle, click what needs clicking —
         * which is what reaches a listing assembled by script or hidden behind a
         * tab. The renderer is only used when there is no session pool.
         */
        ccfg.max_browser_pages = 3;
        ccfg.interactive_pool = srv->driver;
        /*
         * The extractor, for a page whose media is not among its links. On by
         * default; a caller can turn it off, and it is skipped anyway when the
         * binary is not installed.
         */
        ccfg.extract_enabled = mi_json_get_bool(in, "use_extractor", 1);
        ccfg.extractor_path = srv->cfg.extractor_path;
        ccfg.gallery_extractor_path = srv->cfg.gallery_extractor_path;
        int depth = (int)mi_json_get_number(in, "depth", 0);
        int pages = (int)mi_json_get_number(in, "max_pages", 0);
        int budget = (int)mi_json_get_number(in, "budget_seconds", 0);
        if (depth > 0) ccfg.max_depth = depth;
        if (pages > 0) ccfg.max_pages = pages;
        if (budget > 0) ccfg.budget_seconds = budget;
        ccfg.same_host_only = mi_json_get_bool(in, "same_host_only", 0);
        ccfg.use_browser_fallback = mi_json_get_bool(in, "use_browser", 1);
        int browser_pages = (int)mi_json_get_number(in, "max_browser_pages", 0);
        if (browser_pages > 0) ccfg.max_browser_pages = browser_pages;
        /*
         * Selectors to click before reading, so a listing behind a consent
         * button, a tab or a "show all" is reachable. A caller that knows the
         * site can name them; the defaults cover the common consent shapes,
         * which are otherwise what a crawl hits first and never gets past.
         */
        static const char *DEFAULT_CLICKS[] = {
            "button[aria-label*='accept' i]", "button[aria-label*='Accept' i]",
            "#onetrust-accept-btn-handler", ".cookie-accept", ".accept-cookies",
            "button[class*='consent' i]", "button[class*='accept' i]",
        };
        const char *user_clicks[8];
        size_t user_click_count = 0;
        const mi_json *clicks = mi_json_get(in, "click_selectors");
        if (clicks && clicks->type == MI_JSON_ARRAY) {
            for (size_t i = 0; i < clicks->count && user_click_count < 8; i++) {
                const mi_json *el = clicks->items[i];
                if (el && el->type == MI_JSON_STRING && el->string && *el->string) {
                    user_clicks[user_click_count++] = el->string;
                }
            }
        }
        if (user_click_count > 0) {
            ccfg.click_selectors = user_clicks;
            ccfg.click_selector_count = user_click_count;
        } else if (mi_json_get_bool(in, "click_consent", 1)) {
            ccfg.click_selectors = DEFAULT_CLICKS;
            ccfg.click_selector_count = sizeof DEFAULT_CLICKS / sizeof DEFAULT_CLICKS[0];
        }
        int settle = (int)mi_json_get_number(in, "settle_seconds", 0);
        if (settle > 0) ccfg.settle_seconds = settle;

        mi_crawl_result cr;
        int rc = mi_crawl(&ccfg, url, term, &cr);

        mi_json_buf b;
        mi_json_init(&b);
        mi_json_raw(&b, "{");
        mi_json_key(&b, "ok");
        mi_json_bool(&b, rc == MI_OK);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "url");
        mi_json_string(&b, url);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "term");
        mi_json_string(&b, term);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "found");
        mi_json_int(&b, (int64_t)cr.hit_count);
        mi_json_raw(&b, ",");
        /* What the walk did, so an empty result can be explained rather than
         * just reported: "40 pages read, 12 needed the browser, none matched"
         * is a different answer from "the host refused us". */
        mi_json_key(&b, "pages_read");
        mi_json_int(&b, (int64_t)cr.page_count);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "pages_failed");
        mi_json_int(&b, (int64_t)cr.failed);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "pages_rendered");
        mi_json_int(&b, (int64_t)cr.rendered);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "skipped_offsite");
        mi_json_int(&b, (int64_t)cr.skipped_offsite);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "items");
        mi_json_raw(&b, "[");
        for (size_t i = 0; i < cr.hit_count; i++) {
            const mi_crawl_hit *h = &cr.hits[i];
            if (i) mi_json_raw(&b, ",");
            mi_json_raw(&b, "{\"url\":");
            mi_json_string(&b, h->url);
            mi_json_raw(&b, ",\"filename\":");
            mi_json_string(&b, h->filename);
            mi_json_raw(&b, ",\"title\":");
            mi_json_string(&b, h->title);
            mi_json_raw(&b, ",\"mime\":");
            mi_json_string(&b, h->mime);
            mi_json_raw(&b, ",\"score\":");
            mi_json_int(&b, h->score);
            mi_json_raw(&b, ",\"found_on\":");
            mi_json_string(&b, h->found_on);
            mi_json_raw(&b, ",\"via_browser\":");
            mi_json_bool(&b, h->via_browser);
            mi_json_raw(&b, ",\"resolved\":");
            mi_json_bool(&b, h->resolved);
            mi_json_raw(&b, ",\"thumbnail\":");
            mi_json_string(&b, h->thumbnail);
            mi_json_raw(&b, ",\"qualities\":[");
            for (size_t q = 0; q < h->quality_count; q++) {
                const mi_crawl_quality *qy = &h->qualities[q];
                if (q) mi_json_raw(&b, ",");
                mi_json_raw(&b, "{\"height\":");
                mi_json_int(&b, qy->height);
                mi_json_raw(&b, ",\"label\":");
                mi_json_string(&b, qy->label);
                mi_json_raw(&b, ",\"bytes\":");
                mi_json_int(&b, qy->bytes);
                mi_json_raw(&b, ",\"best\":");
                mi_json_bool(&b, qy->best);
                mi_json_raw(&b, "}");
            }
            mi_json_raw(&b, "]");
            mi_json_raw(&b, "}");
        }
        mi_json_raw(&b, "]}");
        send_json(fd, 200, "OK", &b, cors);
        mi_json_free(&b);
        mi_crawl_result_free(&cr);
        mi_json_value_free(in);
        return;
    }

    /* POST /api/v1/browse  {url} — list the media a page or directory offers.
     *
     * The console cannot do this itself: the browser enforces CORS and most
     * media hosts send no CORS headers, so a direct fetch from the page is
     * blocked. Vyzor is not bound by that, so the listing happens here. */
    if (!strcmp(req->path, "/api/v1/browse") && !strcmp(req->method, "POST")) {
        mi_json *in = req->body ? mi_json_parse(req->body) : NULL;
        const char *url = mi_json_get_string(in, "url", NULL);
        if (!url || !*url) {
            send_error(fd, 400, "Bad Request", "missing or empty 'url'", cors);
            mi_json_value_free(in);
            return;
        }
        if (strncasecmp(url, "http://", 7) != 0 && strncasecmp(url, "https://", 8) != 0) {
            send_error(fd, 400, "Bad Request", "url must be http or https", cors);
            mi_json_value_free(in);
            return;
        }

        /* "media" (the default) keeps only what this app can play or show;
         * "all" keeps every reference, so a site can be read as a browser shows
         * it rather than only as a source of media. */
        const char *want = mi_json_get_string(in, "filter", "media");
        mi_browse_filter filter =
            (want && !strcasecmp(want, "all")) ? MI_BROWSE_ALL : MI_BROWSE_MEDIA;

        mi_browse_result res;
        int rc = mi_browse_url(url, srv->cfg.user_agent, filter, &res);
        if (rc != MI_OK) {
            mi_browse_result_free(&res);
            send_error(fd, 502, "Bad Gateway", "could not fetch that page", cors);
            mi_json_value_free(in);
            return;
        }

        mi_json_buf b;
        mi_json_init(&b);
        mi_json_raw(&b, "{\"url\":");
        mi_json_string(&b, url);
        mi_json_raw(&b, ",\"title\":");
        mi_json_string(&b, res.title);
        mi_json_raw(&b, ",\"items\":[");
        for (size_t i = 0; i < res.count; i++) {
            const mi_browse_item *it = &res.items[i];
            if (i) mi_json_raw(&b, ",");
            mi_json_raw(&b, "{\"url\":");
            mi_json_string(&b, it->url);
            mi_json_raw(&b, ",\"filename\":");
            mi_json_string(&b, it->filename);
            mi_json_raw(&b, ",\"title\":");
            mi_json_string(&b, it->title);
            mi_json_raw(&b, ",\"mime\":");
            mi_json_string(&b, it->mime);
            mi_json_raw(&b, ",\"bytes\":");
            mi_json_int(&b, it->bytes);
            mi_json_raw(&b, "}");
        }
        mi_json_raw(&b, "],\"count\":");
        mi_json_int(&b, (int64_t)res.count);
        mi_json_raw(&b, ",\"filter\":");
        mi_json_string(&b, filter == MI_BROWSE_ALL ? "all" : "media");
        mi_json_raw(&b, "}");
        send_json(fd, 200, "OK", &b, cors);
        mi_json_free(&b);
        mi_browse_result_free(&res);
        mi_json_value_free(in);
        return;
    }

    /* GET /api/v1/page?url=… — the fetched bytes of a page, to be shown.
     *
     * The console cannot fetch an arbitrary site itself (CORS), so Vyzor
     * does, and the console renders the result. The response says whether the
     * site permits being framed: a browser enforces X-Frame-Options and CSP
     * frame-ancestors on the framed document, so a site that refuses is
     * reported rather than left as a blank frame the user cannot explain. */
    if (!strcmp(req->path, "/api/v1/page") && !strcmp(req->method, "GET")) {
        const char *url = NULL;
        const char *q = strstr(req->query, "url=");
        char *decoded = NULL;
        if (q) {
            decoded = percent_decode(q + 4, strlen(q + 4));
            url = decoded;
        }
        if (!url || !*url) {
            free(decoded);
            send_error(fd, 400, "Bad Request", "missing 'url'", cors);
            return;
        }
        if (strncasecmp(url, "http://", 7) != 0 && strncasecmp(url, "https://", 8) != 0) {
            free(decoded);
            send_error(fd, 400, "Bad Request", "url must be http or https", cors);
            return;
        }

        mi_download_config cfg;
        mi_download_config_init(&cfg);
        cfg.require_media = 0;
        cfg.user_agent = srv->cfg.user_agent;
        mi_fetch_info info;
        char *body = mi_download_fetch_document(&cfg, url, 4 * 1024 * 1024, &info);
        free(decoded);
        if (!body) {
            mi_fetch_info_free(&info);
            send_error(fd, 502, "Bad Gateway", "could not fetch that page", cors);
            return;
        }

        /* The frame refusal travels as a header the console can read without
         * parsing the body, and as a value a proxy would not strip. */
        char extra[512];
        extra[0] = '\0';
        if (!info.framable) {
            snprintf(extra, sizeof extra, "X-Vyzorix-Framable: no\r\nX-Vyzorix-Frame-Blocker: %s\r\n",
                     info.frame_blocker ? info.frame_blocker : "the site's policy");
        } else {
            snprintf(extra, sizeof extra, "X-Vyzorix-Framable: yes\r\n");
        }

        const char *ctype = info.content_type ? info.content_type : "text/html; charset=utf-8";
        size_t blen = strlen(body);
        char header[2048];
        /* The CORS headers matter here as much as anywhere: the console reads
         * the framing header cross-origin, and an iframe needs the origin
         * allowed too. Omitting them made the request fail in the browser while
         * succeeding from curl, which is exactly the trap. */
        int hn = snprintf(header, sizeof header,
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %zu\r\n"
                          "Cache-Control: no-store\r\n"
                          "%s"
                          "%s%s%s"
                          "Connection: close\r\n\r\n",
                          ctype, blen, extra,
                          cors ? "Access-Control-Allow-Origin: " : "",
                          cors ? cors : "",
                          cors ? "\r\nAccess-Control-Expose-Headers: X-Vyzorix-Framable,X-Vyzorix-Frame-Blocker\r\n"
                                 "Vary: Origin\r\n"
                               : "");
        send_all(fd, header, (size_t)hn);
        send_all(fd, body, blen);
        free(body);
        mi_fetch_info_free(&info);
        return;
    }

    /*
     * GET /api/v1/render?url=…&mode=image|html — render a page with a real
     * browser and return the picture or the rendered DOM.
     *
     * This is the only way to show a page that builds itself with JavaScript,
     * and the only way to show one that forbids being framed: it is rendered
     * here rather than embedded, so the site's framing policy does not apply.
     *
     * A render is expensive — roughly half a gigabyte and a second or two — so
     * a request that arrives when the gate is full is refused with 503 and a
     * Retry-After rather than queued. A queue behind a slow page would grow
     * without bound and take the whole Vyzor down with it.
     */
    if (!strcmp(req->path, "/api/v1/render") && !strcmp(req->method, "GET")) {
        if (!srv->renderer) {
            send_error(fd, 503, "Service Unavailable",
                       "rendering is not enabled on this Vyzor", cors);
            return;
        }
        if (!mi_render_pool_available(srv->renderer)) {
            send_error(fd, 503, "Service Unavailable",
                       "no browser is installed on Vyzor host", cors);
            return;
        }

        /*
         * The `url` parameter, which must stop at the next `&`.
         *
         * Decoding to the end of the query string instead would fold the other
         * parameters into the URL — `…google.com/&mode=html` — which the site
         * then answers with a 404. That looked like the renderer failing when it
         * was the argument parsing.
         */
        char *decoded = NULL;
        const char *url = NULL;
        const char *q = strstr(req->query, "url=");
        if (q) {
            const char *val = q + 4;
            const char *end = strchr(val, '&');
            decoded = percent_decode(val, end ? (size_t)(end - val) : strlen(val));
            url = decoded;
        }
        if (!url || !*url) {
            free(decoded);
            send_error(fd, 400, "Bad Request", "missing 'url'", cors);
            return;
        }

        /* "image" is the default because it is the one that cannot be got
         * subtly wrong: a sanitised DOM can be sanitised badly, a PNG cannot. */
        const char *want = NULL;
        const char *m = strstr(req->query, "mode=");
        char mode_buf[16] = {0};
        if (m) {
            size_t n = 0;
            while (m[5 + n] && m[5 + n] != '&' && n + 1 < sizeof mode_buf) {
                mode_buf[n] = m[5 + n];
                n++;
            }
            mode_buf[n] = '\0';
            want = mode_buf;
        }
        mi_render_mode mode =
            (want && !strcasecmp(want, "html")) ? MI_RENDER_DOM : MI_RENDER_IMAGE;

        mi_render_result res;
        mi_render_status st = mi_render_run(srv->renderer, url, mode, &res);
        /* The URL is needed for the sanitiser's link rewriting, so it is kept
         * until after that runs. */
        int sanitized = 0;

        if (st != MI_RENDER_OK) {
            /* Each failure says what happened and what to do about it. A 503
             * carries Retry-After so a client knows to come back rather than
             * hammer the gate. */
            const char *why = res.error ? res.error : mi_render_status_name(st);
            /* A truncated result is still a result: the bytes are useful and the
             * caller is told they were cut by a header, so this is not an error. */
            if (st != MI_RENDER_TOO_LARGE) {
                int code = 502;
                const char *text = "Bad Gateway";
                int retry_after = 0;
                if (st == MI_RENDER_BUSY) {
                    code = 503;
                    text = "Service Unavailable";
                    /* Tell the caller when to come back, rather than leaving it
                     * to hammer a gate that is deliberately narrow. */
                    retry_after = 3;
                } else if (st == MI_RENDER_BAD_URL) {
                    code = 400;
                    text = "Bad Request";
                } else if (st == MI_RENDER_TIMEOUT) {
                    code = 504;
                    text = "Gateway Timeout";
                } else if (st == MI_RENDER_NO_BROWSER) {
                    code = 503;
                    text = "Service Unavailable";
                }
                send_error_retry(fd, code, text, why, cors, retry_after);
                mi_render_result_free(&res);
                return;
            }
        }

        /*
         * A rendered DOM is the page's own markup, so it is sanitised before it
         * is served: scripts, event handlers, frames and the rest are removed,
         * links are routed back through the renderer so a click stays in the
         * app, and a strict policy is injected.
         *
         * This is hygiene, not the security boundary — that is the console's
         * sandboxed frame, which the browser enforces. But serving the raw DOM
         * would be handing the console a document full of script, and relying on
         * one attribute for safety is a single point of failure.
         */
        if (mode == MI_RENDER_DOM && res.data) {
            mi_sanitize_config scfg;
            memset(&scfg, 0, sizeof scfg);
            scfg.link_prefix = "/api/v1/render?url=";
            scfg.base_url = url;
            char *clean = mi_sanitize_html((const char *)res.data, res.len, &scfg);
            if (clean) {
                free(res.data);
                res.data = (unsigned char *)clean;
                res.len = strlen(clean);
                sanitized = 1;
            }
        }

        /* The response is hand-built, so the CORS headers have to be sent by
         * hand too — omitting them is how a route that works under curl fails
         * in the browser. */
        char meta[512];
        meta[0] = '\0';
        snprintf(meta, sizeof meta,
                 "X-Vyzorix-Render: %s\r\n"
                 "X-Vyzorix-Render-Ms: %.0f\r\n"
                 "X-Vyzorix-Render-Cache: %s\r\n"
                 "%s"
                 "%s",
                 mi_render_status_name(st), res.elapsed_seconds * 1000.0,
                 res.from_cache ? "hit" : "miss",
                 sanitized ? "X-Vyzorix-Sanitized: yes\r\n" : "",
                 res.truncated ? "X-Vyzorix-Render-Truncated: yes\r\n" : "");
        if (res.title) {
            /* The page's own title travels as a header so a client can label a
             * picture without parsing an image. */
            char title[512];
            snprintf(title, sizeof title, "X-Vyzorix-Title: %.400s\r\n", res.title);
            strncat(meta, title, sizeof meta - strlen(meta) - 1);
        }

        char header[2048];
        int hn = snprintf(header, sizeof header,
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %zu\r\n"
                          "Cache-Control: private, max-age=60\r\n"
                          "%s"
                          "%s%s%s"
                          "Connection: close\r\n\r\n",
                          res.content_type, res.len, meta,
                          cors ? "Access-Control-Allow-Origin: " : "",
                          cors ? cors : "",
                          cors ? "\r\nAccess-Control-Expose-Headers: X-Vyzorix-Render,X-Vyzorix-Render-Ms,X-Vyzorix-Render-Cache,X-Vyzorix-Sanitized,X-Vyzorix-Title,X-Vyzorix-Render-Truncated\r\n"
                                 "Vary: Origin\r\n"
                               : "");
        send_all(fd, header, (size_t)hn);
        if (res.data && res.len > 0) send_all(fd, (const char *)res.data, res.len);
        mi_render_result_free(&res);
        free(decoded);
        return;
    }

    /* POST /api/v1/detect  (raw octet-stream body) */
    if (!strcmp(req->path, "/api/v1/detect") && !strcmp(req->method, "POST")) {
        mi_media_info info;
        mi_detect((const unsigned char *)req->body, req->body_len, &info);
        mi_json_buf b;
        mi_json_init(&b);
        mi_json_raw(&b, "{\"kind\":");
        mi_json_string(&b, mi_kind_name(info.kind));
        mi_json_raw(&b, ",\"container\":");
        mi_json_string(&b, info.container);
        mi_json_raw(&b, ",\"confidence\":");
        mi_json_double(&b, info.confidence);
        mi_json_raw(&b, ",\"is_video\":");
        mi_json_bool(&b, info.is_video);
        mi_json_raw(&b, "}");
        send_json(fd, 200, "OK", &b, cors);
        mi_json_free(&b);
        mi_media_info_free(&info);
        return;
    }

    /* POST /api/v1/downloads */
    if (!strcmp(req->path, "/api/v1/downloads") && !strcmp(req->method, "POST")) {
        mi_json *in = req->body ? mi_json_parse(req->body) : NULL;
        const char *url = mi_json_get_string(in, "url", NULL);
        if (!url || !*url) {
            send_error(fd, 400, "Bad Request", "missing or empty 'url'", cors);
            mi_json_value_free(in);
            return;
        }
        mi_download_rec *j = download_new(url);
        if (!j) {
            send_error(fd, 500, "Internal Server Error", "out of memory", cors);
            mi_json_value_free(in);
            return;
        }
        const char *hls = mi_json_get_string(in, "hls_dir", NULL);
        const char *prefix = mi_json_get_string(in, "prefix", NULL);
        if (hls) j->hls_dir = mi_strdup(hls);
        if (prefix) j->prefix = mi_strdup(prefix);
        /* Echoed back so the UI can say which format and which stages this
         * particular download runs. */
        const char *quality = mi_json_get_string(in, "quality", NULL);
        if (quality) j->requested_quality = mi_strdup(quality);
        j->do_hls = mi_json_get_bool(in, "hls", 1);
        j->do_upload = mi_json_get_bool(in, "upload", 0);
        j->do_purge = mi_json_get_bool(in, "purge", 0);
        int conns = (int)mi_json_get_int(in, "connections", 8);
        j->connections = conns > 0 ? conns : 8;
        /* A caller that wants a document or text file has to say so, because
         * refusing an unrecognised payload is the safer default. */
        j->allow_non_media = mi_json_get_bool(in, "allow_non_media", 0);

        storage_snapshot *snap = snapshot_from_headers(req);

        pthread_mutex_lock(&srv->lock);
        add_download(srv, j);
        pthread_mutex_unlock(&srv->lock);

        if (start_download(srv, j, snap) != MI_OK) {
            pthread_mutex_lock(&srv->lock);
            j->state = DOWNLOAD_FAILED;
            j->error = mi_strdup("could not start worker");
            pthread_mutex_unlock(&srv->lock);
            snapshot_free(snap);
        }

        mi_json_buf b;
        mi_json_init(&b);
        download_to_json(j, &b);
        send_json(fd, 202, "Accepted", &b, cors);
        mi_json_free(&b);
        mi_json_value_free(in);
        return;
    }

    /* GET /api/v1/downloads */
    if (!strcmp(req->path, "/api/v1/downloads") && !strcmp(req->method, "GET")) {
        pthread_mutex_lock(&srv->lock);
        mi_json_buf b;
        mi_json_init(&b);
        mi_json_raw(&b, "{\"items\":[");
        for (size_t i = 0; i < srv->download_count; i++) {
            if (i) mi_json_raw(&b, ",");
            download_to_json(srv->downloads[i], &b);
        }
        mi_json_raw(&b, "],\"count\":");
        mi_json_int(&b, (int64_t)srv->download_count);
        mi_json_raw(&b, "}");
        pthread_mutex_unlock(&srv->lock);
        send_json(fd, 200, "OK", &b, cors);
        mi_json_free(&b);
        return;
    }

    /* GET|HEAD /api/v1/downloads/{id}/content — the downloaded bytes. */
    /* GET|HEAD /api/v1/downloads/{id}/content, /hls/{path}, /converted */
    {
        static const char kDownloadsPrefix2[] = "/api/v1/downloads/";
        if (!strncmp(req->path, kDownloadsPrefix2, sizeof kDownloadsPrefix2 - 1) &&
            (!strcmp(req->method, "GET") || !strcmp(req->method, "HEAD"))) {
            const char *rest = req->path + (sizeof kDownloadsPrefix2 - 1);
            size_t rest_len = strlen(rest);
            /* The id runs to the first '/'; everything after is a sub-path. */
            const char *slash = strchr(rest, '/');
            size_t idlen = slash ? (size_t)(slash - rest) : rest_len;
            const char *sub = slash ? slash + 1 : "";

            if (idlen > 0 && idlen < 64) {
                char idbuf[64];
                memcpy(idbuf, rest, idlen);
                idbuf[idlen] = '\0';

                if (!*sub) {
                    /* Bare id: not a content route; fall through to the
                     * record routes below. */
                } else if (!strcmp(sub, "content")) {
                    serve_download_content(fd, req, srv, idbuf, cors);
                    return;
                } else if (!strcmp(sub, "converted")) {
                    /* The MP4 produced by a conversion, when there is one. */
                    pthread_mutex_lock(&srv->lock);
                    mi_download_rec *cj = find_download(srv, idbuf);
                    char *cpath = (cj && cj->converted_path)
                                      ? mi_strdup(cj->converted_path)
                                      : NULL;
                    pthread_mutex_unlock(&srv->lock);
                    if (!cpath) {
                        send_error(fd, 404, "Not Found",
                                   "this item has not been converted", cors);
                        return;
                    }
                    serve_file(fd, req, cpath, "video/mp4", NULL, cors);
                    free(cpath);
                    return;
                } else if (!strncmp(sub, "hls/", 4)) {
                    /* Transcoded HLS output: playlists and segments. */
                    serve_artifact(fd, req, srv, idbuf, sub + 4, cors);
                    return;
                }
            }
        }
    }

    /* GET|PATCH|DELETE /api/v1/downloads/{id} */
    /* Derive the offset from the literal so renaming the route cannot leave a
     * stale length behind (this exact bug shipped once with a hardcoded 13). */
    static const char kDownloadsPrefix[] = "/api/v1/downloads/";
    if (!strncmp(req->path, kDownloadsPrefix, sizeof kDownloadsPrefix - 1)) {
        const char *id = req->path + (sizeof kDownloadsPrefix - 1);
        pthread_mutex_lock(&srv->lock);
        mi_download_rec *j = find_download(srv, id);
        if (!j) {
            pthread_mutex_unlock(&srv->lock);
            send_error(fd, 404, "Not Found", "no such download", cors);
            return;
        }
        if (!strcmp(req->method, "GET")) {
            mi_json_buf b;
            mi_json_init(&b);
            download_to_json(j, &b);
            pthread_mutex_unlock(&srv->lock);
            send_json(fd, 200, "OK", &b, cors);
            mi_json_free(&b);
            return;
        }
        if (!strcmp(req->method, "PATCH")) {
            mi_json *in = req->body ? mi_json_parse(req->body) : NULL;
            const char *name = mi_json_get_string(in, "name", NULL);
            const char *move_to = mi_json_get_string(in, "move_to", NULL);
            const char *action = mi_json_get_string(in, "action", NULL);

            /* Convert to MP4: for a container the browser cannot play (MKV,
             * AVI), so "play" has something playable to point at. Only a
             * finished download with bytes on disk can be converted. */
            if (action && !strcmp(action, "convert")) {
                if (j->state != DOWNLOAD_DONE) {
                    pthread_mutex_unlock(&srv->lock);
                    send_error(fd, 409, "Conflict",
                               "only a completed download can be converted", cors);
                    mi_json_value_free(in);
                    return;
                }
                if (j->converting) {
                    /* Already running: report the state rather than starting a
                     * second ffmpeg on the same output file. */
                    mi_json_buf b;
                    mi_json_init(&b);
                    download_to_json(j, &b);
                    pthread_mutex_unlock(&srv->lock);
                    send_json(fd, 202, "Accepted", &b, cors);
                    mi_json_free(&b);
                    mi_json_value_free(in);
                    return;
                }
                j->converting = 1;
                free(j->convert_error);
                j->convert_error = NULL;
                int started = start_conversion(srv, j);
                if (started != MI_OK) {
                    j->converting = 0;
                    free(j->convert_error);
                    j->convert_error = mi_strdup("could not start the converter");
                }
                mi_json_buf b;
                mi_json_init(&b);
                download_to_json(j, &b);
                pthread_mutex_unlock(&srv->lock);
                send_json(fd, 202, "Accepted", &b, cors);
                mi_json_free(&b);
                mi_json_value_free(in);
                return;
            }

            /* Transfer control: pause stops the worker but keeps everything, so
             * resume continues from what is already on disk. */
            if (action && (!strcmp(action, "pause") || !strcmp(action, "resume"))) {
                int want_pause = !strcmp(action, "pause");
                int restart = 0;
                if (want_pause) {
                    if (j->state != DOWNLOAD_RUNNING && j->state != DOWNLOAD_QUEUED) {
                        pthread_mutex_unlock(&srv->lock);
                        send_error(fd, 409, "Conflict",
                                   "only a running download can be paused", cors);
                        mi_json_value_free(in);
                        return;
                    }
                    /* Ask the worker to stop; it flips the state to paused when
                     * it actually stops, so the UI never claims a pause that has
                     * not happened. */
                    j->pause_requested = 1;
                } else {
                    if (j->state != DOWNLOAD_PAUSED) {
                        pthread_mutex_unlock(&srv->lock);
                        send_error(fd, 409, "Conflict",
                                   "only a paused download can be resumed", cors);
                        mi_json_value_free(in);
                        return;
                    }
                    j->pause_requested = 0;
                    j->state = DOWNLOAD_QUEUED;
                    j->error = NULL;
                    /* Restart the transfer. The parts and state file from the
                     * pause are still on disk, so this resumes rather than
                     * starting over. The storage snapshot was released when the
                     * original worker stopped, so a resumed download fetches but
                     * does not re-upload — the bytes are already local. */
                    restart = !j->worker_active;
                }
                mi_json_buf b;
                mi_json_init(&b);
                download_to_json(j, &b);
                pthread_mutex_unlock(&srv->lock);
                if (restart) start_download(srv, j, NULL);
                send_json(fd, 200, "OK", &b, cors);
                mi_json_free(&b);
                mi_json_value_free(in);
                return;
            }

            /* Rename and/or move. Both are metadata edits that keep the bytes,
             * so they are reversible and need no confirmation. */
            if (!name && !move_to) {
                pthread_mutex_unlock(&srv->lock);
                send_error(fd, 400, "Bad Request",
                           "provide 'name', 'move_to' or 'action'", cors);
                mi_json_value_free(in);
                return;
            }
            int rc = MI_OK;
            if (name && apply_rename(j, name) != MI_OK) rc = MI_ERR_ARG;
            if (rc == MI_OK && move_to && apply_move(srv, j, move_to) != MI_OK) rc = MI_ERR_IO;
            mi_json_buf b;
            mi_json_init(&b);
            if (rc == MI_OK) {
                download_to_json(j, &b);
            } else {
                mi_json_raw(&b, "{\"error\":");
                mi_json_string(&b, "could not apply the change");
                mi_json_raw(&b, ",\"status\":");
                mi_json_int(&b, rc == MI_ERR_ARG ? 400 : 500);
                mi_json_raw(&b, "}");
            }
            pthread_mutex_unlock(&srv->lock);
            if (rc == MI_OK) {
                send_json(fd, 200, "OK", &b, cors);
            } else {
                send_json(fd, rc == MI_ERR_ARG ? 400 : 500,
                          rc == MI_ERR_ARG ? "Bad Request" : "Internal Server Error", &b, cors);
            }
            mi_json_free(&b);
            mi_json_value_free(in);
            return;
        }
        if (!strcmp(req->method, "DELETE")) {
            /* Two distinct intents share this verb:
             *   ?files=0 (default) -> cancel: stop the transfer, keep the record
             *   ?files=1           -> delete: stop it, remove it from the list
             *                         AND delete its artifacts from disk.
             * Removing files is irreversible, so it is opt-in and reported. */
            int remove_files = 0;
            const char *fq = strstr(req->query, "files=");
            if (fq && fq[6] == '1') remove_files = 1;

            if (remove_files) {
                if (j->state == DOWNLOAD_RUNNING || j->state == DOWNLOAD_QUEUED) {
                    j->state = DOWNLOAD_CANCELLED;
                }
                char *out_dir = j->out_dir
                                    ? mi_strdup(j->out_dir)
                                    : mi_asprintf("%s/%s", srv->download_dir, j->id);

                /* Drop the record from the list before releasing the lock so a
                 * concurrent GET cannot resurrect it. */
                size_t idx = 0;
                for (size_t i = 0; i < srv->download_count; i++) {
                    if (srv->downloads[i] == j) {
                        idx = i;
                        break;
                    }
                }
                memmove(&srv->downloads[idx], &srv->downloads[idx + 1],
                        (srv->download_count - idx - 1) * sizeof(mi_download_rec *));
                srv->download_count--;
                /* Stop the transfer, then wait for the worker — and any
                 * converter — to actually let go before freeing the record,
                 * otherwise either would keep writing through a dangling
                 * pointer. */
                j->pause_requested = 1;
                pthread_mutex_unlock(&srv->lock);
                for (int i = 0; i < 100 && (j->worker_active || j->convert_active); i++) {
                    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
                    nanosleep(&ts, NULL);
                }

                int removed = mi_rm_rf(out_dir) == 0;
                free(out_dir);
                download_free(j);

                mi_json_buf b;
                mi_json_init(&b);
                mi_json_raw(&b, "{\"deleted\":true,\"files_removed\":");
                mi_json_bool(&b, removed);
                mi_json_raw(&b, "}");
                send_json(fd, 200, "OK", &b, cors);
                mi_json_free(&b);
                return;
            }

            /* Ask the worker to stop. A cancel is final: the record stays but
             * it will not be resumed. */
            j->pause_requested = 1;
            j->state = DOWNLOAD_CANCELLED;
            pthread_mutex_unlock(&srv->lock);
            mi_json_buf b;
            mi_json_init(&b);
            mi_json_raw(&b, "{\"cancelled\":true}");
            send_json(fd, 200, "OK", &b, cors);
            mi_json_free(&b);
            return;
        }
    }

    /* GET /api/v1/system/storage — real disk + library figures.
     * Everything here is measured, never estimated: statvfs for the volume and
     * a walk of the work dir for what this Vyzor has written. */
    if (!strcmp(req->path, "/api/v1/system/storage") && !strcmp(req->method, "GET")) {
        const char *work_dir = srv->download_dir;

        mi_fs_info fs;
        int have_fs = mi_fs_stats(work_dir, &fs) == 0;

        int64_t library_bytes = 0;
        int64_t library_files = 0;
        mi_dir_usage(work_dir, &library_bytes, &library_files);

        /* Finished downloads still resident on disk, for the "on disk" figure. */
        pthread_mutex_lock(&srv->lock);
        int64_t completed = 0;
        int64_t active = 0;
        int64_t failed = 0;
        int64_t moved = 0;
        for (size_t i = 0; i < srv->download_count; i++) {
            mi_download_rec *d = srv->downloads[i];
            moved += d->downloaded;
            if (d->state == DOWNLOAD_DONE) completed++;
            else if (d->state == DOWNLOAD_FAILED) failed++;
            else if (d->state == DOWNLOAD_QUEUED || d->state == DOWNLOAD_RUNNING) active++;
        }
        pthread_mutex_unlock(&srv->lock);

        /* Which volume the library is actually on, so the UI can name it
         * rather than guess. */
        char mount_point[PATH_MAX] = {0};
        char fs_type[128] = {0};
        char fs_device[512] = {0};
        int have_mount = mi_mount_info(work_dir, mount_point, sizeof mount_point,
                                       fs_type, sizeof fs_type,
                                       fs_device, sizeof fs_device) == 0;

        /* Writability is proven, not assumed: the UI needs to know whether the
         * Vyzor can really write here before it offers the path as a choice. */
        char werr[160] = {0};
        int writable = probe_writable(work_dir, werr, sizeof werr) == 0;

        mi_json_buf b;
        mi_json_init(&b);
        mi_json_raw(&b, "{\"work_dir\":");
        mi_json_string(&b, work_dir);
        mi_json_raw(&b, ",\"writable\":");
        mi_json_bool(&b, writable);
        mi_json_raw(&b, ",\"write_error\":");
        if (!writable) mi_json_string(&b, werr);
        else mi_json_null(&b);
        mi_json_raw(&b, ",\"mount_point\":");
        if (have_mount && mount_point[0]) mi_json_string(&b, mount_point);
        else mi_json_null(&b);
        mi_json_raw(&b, ",\"fs_type\":");
        if (have_mount && fs_type[0]) mi_json_string(&b, fs_type);
        else mi_json_null(&b);
        mi_json_raw(&b, ",\"fs_device\":");
        if (have_mount && fs_device[0]) mi_json_string(&b, fs_device);
        else mi_json_null(&b);
        mi_json_raw(&b, ",\"filesystem\":");
        if (have_fs) {
            mi_json_raw(&b, "{");
            mi_json_key(&b, "total_bytes");
            mi_json_int(&b, fs.total_bytes);
            mi_json_raw(&b, ",");
            mi_json_key(&b, "free_bytes");
            mi_json_int(&b, fs.free_bytes);
            mi_json_raw(&b, ",");
            mi_json_key(&b, "avail_bytes");
            mi_json_int(&b, fs.avail_bytes);
            mi_json_raw(&b, ",");
            mi_json_key(&b, "used_bytes");
            mi_json_int(&b, fs.total_bytes - fs.free_bytes);
            mi_json_raw(&b, "}");
        } else {
            mi_json_null(&b);
        }
        mi_json_raw(&b, ",\"library\":{");
        mi_json_key(&b, "bytes");
        mi_json_int(&b, library_bytes);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "files");
        mi_json_int(&b, library_files);
        mi_json_raw(&b, "},\"downloads\":{");
        mi_json_key(&b, "completed");
        mi_json_int(&b, completed);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "active");
        mi_json_int(&b, active);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "failed");
        mi_json_int(&b, failed);
        mi_json_raw(&b, ",");
        mi_json_key(&b, "bytes_transferred");
        mi_json_int(&b, moved);
        mi_json_raw(&b, "}}");
        send_json(fd, 200, "OK", &b, cors);
        mi_json_free(&b);
        return;
    }

    /* PUT /api/v1/storage/location  {dir} — choose where downloads are written.
     * Validated with a real write before it is accepted, so the UI can trust
     * the answer it gets back. */
    if (!strcmp(req->path, "/api/v1/storage/location") && !strcmp(req->method, "PUT")) {
        mi_json *in = req->body ? mi_json_parse(req->body) : NULL;
        const char *dir = mi_json_get_string(in, "dir", NULL);
        if (!dir || !*dir) {
            send_error(fd, 400, "Bad Request", "missing or empty 'dir'", cors);
            mi_json_value_free(in);
            return;
        }
        char err[160] = {0};
        if (probe_writable(dir, err, sizeof err) != 0) {
            mi_json_buf eb;
            mi_json_init(&eb);
            mi_json_raw(&eb, "{\"error\":");
            mi_json_string(&eb, err);
            mi_json_raw(&eb, ",\"dir\":");
            mi_json_string(&eb, dir);
            mi_json_raw(&eb, "}");
            send_json(fd, 422, "Unprocessable Entity", &eb, cors);
            mi_json_free(&eb);
            mi_json_value_free(in);
            return;
        }
        /* Nothing references the work dir directly, so a swap is safe: existing
         * items keep the absolute path recorded in out_dir. */
        pthread_mutex_lock(&srv->lock);
        free(srv->download_dir);
        srv->download_dir = mi_strdup(dir);
        pthread_mutex_unlock(&srv->lock);

        mi_json_buf b;
        mi_json_init(&b);
        mi_json_raw(&b, "{\"dir\":");
        mi_json_string(&b, dir);
        mi_json_raw(&b, ",\"writable\":");
        mi_json_bool(&b, 1);
        mi_json_raw(&b, "}");
        send_json(fd, 200, "OK", &b, cors);
        mi_json_free(&b);
        mi_json_value_free(in);
        return;
    }

    /* GET /api/v1/storage/config  (non-secret view) */
    if (!strcmp(req->path, "/api/v1/storage/config") && !strcmp(req->method, "GET")) {
        const char *endpoint = request_header(req, "X-S3-Endpoint");
        const char *bucket = request_header(req, "X-S3-Bucket");
        mi_json_buf b;
        mi_json_init(&b);
        mi_json_raw(&b, "{\"endpoint\":");
        mi_json_string(&b, endpoint);
        mi_json_raw(&b, ",\"bucket\":");
        mi_json_string(&b, bucket);
        mi_json_raw(&b, ",\"configured\":");
        mi_json_bool(&b, bucket && request_header(req, "X-S3-Access-Key") != NULL);
        mi_json_raw(&b, "}");
        send_json(fd, 200, "OK", &b, cors);
        mi_json_free(&b);
        return;
    }

    send_error(fd, 404, "Not Found", "unknown endpoint", cors);
}

/* ------------------------------------------------------------------ */
/* connection handling                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    mi_server *srv;
    int fd;
} conn_ctx;

static void *conn_thread(void *arg) {
    conn_ctx *ctx = arg;
    http_request req;
    int rc = read_request(ctx->fd, &ctx->srv->cfg, &req);
    if (rc == 0) {
        handle_request(ctx->srv, ctx->fd, &req);
    } else if (rc == -2) {
        send_error(ctx->fd, 413, "Payload Too Large", "body exceeds limit",
                   cors_origin_for(ctx->srv, &req));
    }
    if (rc == 0) request_free(&req);
    close(ctx->fd);
    free(ctx);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

mi_server *mi_server_new(const mi_server_config *cfg) {
    mi_server *srv = mi_calloc(1, sizeof *srv);
    if (!srv) return NULL;
    srv->cfg = *cfg;
    srv->download_dir = mi_strdup(cfg->work_dir ? cfg->work_dir : "./downloads");
    srv->listen_fd = -1;
    srv->stop = 0;
    pthread_mutex_init(&srv->lock, NULL);

    /* Build the renderer once, here, so its concurrency gate is a property of
     * the process rather than of a request. A pool with no browser still
     * exists; it reports MI_RENDER_NO_BROWSER rather than crashing. */
    if (cfg->render_enabled) {
        mi_render_config rcfg;
        mi_render_config_init(&rcfg);
        rcfg.chromium_path = cfg->render_browser;
        rcfg.timeout_seconds = cfg->render_timeout_seconds;
        rcfg.max_concurrent = cfg->render_max_concurrent;
        rcfg.v8_heap_mb = cfg->render_v8_heap_mb;
        rcfg.viewport_width = cfg->render_viewport_width;
        rcfg.viewport_height = cfg->render_viewport_height;
        rcfg.user_agent = cfg->user_agent;
        rcfg.profile_root = cfg->render_profile_root;

        /* The cache lives beside the downloads unless told otherwise: it is the
         * same volume the operator already sized for this Vyzor's data. */
        char *cache_dir = NULL;
        if (cfg->render_cache_dir && *cfg->render_cache_dir) {
            cache_dir = mi_strdup(cfg->render_cache_dir);
        } else if (srv->download_dir) {
            cache_dir = mi_asprintf("%s/.render-cache", srv->download_dir);
        }
        rcfg.cache_dir = cache_dir;
        rcfg.cache_max_bytes = cfg->render_cache_max_bytes;
        rcfg.cache_max_entries = cfg->render_cache_max_entries;
        rcfg.cache_ttl_seconds = cfg->render_cache_ttl_seconds;

        srv->renderer = mi_render_pool_new(&rcfg);
        free(cache_dir);
    }

    /* The live-session pool, built the same way and for the same reason: the
     * gate has to be a property of the process, not of a request. */
    if (cfg->interactive_enabled) {
        mi_interactive_config icfg;
        mi_interactive_config_init(&icfg);
        icfg.chromium_path = cfg->render_browser;
        icfg.max_concurrent = cfg->interactive_max_concurrent;
        icfg.idle_timeout_seconds = cfg->interactive_idle_seconds;
        icfg.v8_heap_mb = cfg->interactive_v8_heap_mb;
        icfg.viewport_width = cfg->render_viewport_width;
        icfg.viewport_height = cfg->render_viewport_height;
        icfg.user_agent = cfg->user_agent;
        icfg.profile_root = cfg->render_profile_root;
        if (cfg->interactive_keepalive_ping_seconds > 0)
            icfg.keepalive_ping_seconds = cfg->interactive_keepalive_ping_seconds;
        if (cfg->interactive_keepalive_timeout_seconds > 0)
            icfg.keepalive_timeout_seconds = cfg->interactive_keepalive_timeout_seconds;
        srv->driver = mi_interactive_pool_new(&icfg);
    }

    /* The live-video pool, built the same way and for the same reason. It is
     * the only one that spawns an X server as well as a browser, so its gate
     * bounds displays as much as browsers. */
    if (cfg->rtc_enabled) {
        mi_rtc_config rcfg;
        mi_rtc_config_init(&rcfg);
        rcfg.chromium_path = cfg->render_browser;
        rcfg.max_concurrent = cfg->rtc_max_concurrent > 0 ? cfg->rtc_max_concurrent : 1;
        rcfg.idle_timeout_seconds = cfg->rtc_idle_seconds > 0 ? cfg->rtc_idle_seconds : 180;
        rcfg.v8_heap_mb = cfg->rtc_v8_heap_mb > 0 ? cfg->rtc_v8_heap_mb : 256;
        rcfg.width = cfg->rtc_width > 0 ? cfg->rtc_width : cfg->render_viewport_width;
        rcfg.height = cfg->rtc_height > 0 ? cfg->rtc_height : cfg->render_viewport_height;
        if (cfg->rtc_fps > 0) rcfg.fps = cfg->rtc_fps;
        if (cfg->rtc_bitrate_kbps > 0) rcfg.bitrate_kbps = cfg->rtc_bitrate_kbps;
        rcfg.user_agent = cfg->user_agent;
        rcfg.profile_root = cfg->render_profile_root;
        rcfg.stun_server = cfg->rtc_stun_server;
        srv->rtc = mi_rtc_pool_new(&rcfg);
    }
    return srv;
}

int mi_server_can_render(const mi_server *srv) {
    return srv && srv->renderer && mi_render_pool_available(srv->renderer);
}

int mi_server_can_drive(const mi_server *srv) {
    return srv && srv->driver && mi_interactive_pool_available(srv->driver);
}

void mi_server_free(mi_server *srv) {
    if (!srv) return;
    /* The pump thread touches sessions; it must be stopped before the pool
     * that owns them is freed. */
    rtc_registry_shutdown();
    for (size_t i = 0; i < srv->download_count; i++) download_free(srv->downloads[i]);
    free(srv->downloads);
    free(srv->threads);
    free(srv->download_dir);
    mi_render_pool_free(srv->renderer);
    mi_interactive_pool_free(srv->driver);
    mi_rtc_pool_free(srv->rtc);
    if (srv->listen_fd >= 0) close(srv->listen_fd);
    pthread_mutex_destroy(&srv->lock);
    free(srv);
}

int mi_server_listen(mi_server *srv) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return MI_ERR;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)srv->cfg.port);
    if (!strcmp(srv->cfg.host, "0.0.0.0")) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, srv->cfg.host, &addr.sin_addr) != 1) {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return MI_ERR;
    }
    if (listen(fd, 64) != 0) {
        close(fd);
        return MI_ERR;
    }
    srv->listen_fd = fd;
    return MI_OK;
}

int mi_server_port(const mi_server *srv) {
    struct sockaddr_in addr;
    socklen_t len = sizeof addr;
    if (srv->listen_fd < 0) return -1;
    if (getsockname(srv->listen_fd, (struct sockaddr *)&addr, &len) != 0) return -1;
    return ntohs(addr.sin_port);
}

void mi_server_stop(mi_server *srv) {
    if (srv) srv->stop = 1;
    if (srv && srv->listen_fd >= 0) shutdown(srv->listen_fd, SHUT_RDWR);
}

int mi_server_run(mi_server *srv) {
    while (!srv->stop) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int fd = accept(srv->listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            if (srv->stop) break;
            if (errno == EINTR) continue;
            break;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        conn_ctx *ctx = mi_calloc(1, sizeof *ctx);
        if (!ctx) {
            close(fd);
            continue;
        }
        ctx->srv = srv;
        ctx->fd = fd;
        pthread_t th;
        if (pthread_create(&th, NULL, conn_thread, ctx) != 0) {
            close(fd);
            free(ctx);
            continue;
        }
        pthread_detach(th);
    }
    return MI_OK;
}

int mi_server_start(mi_server *srv) {
    if (mi_server_listen(srv) != MI_OK) return MI_ERR;
    return mi_server_run(srv);
}
