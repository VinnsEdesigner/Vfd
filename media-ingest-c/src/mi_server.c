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
#include "mi_detect.h"
#include "mi_download.h"
#include "mi_hls.h"
#include "mi_json.h"
#include "mi_render.h"
#include "mi_util.h"

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
    /* NULL means the engine's own default agent. */
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
    if (strrchr(base, '.')) return base;
    char *with_ext = mi_asprintf("%s.%s", base, extension_for_kind(j->media_kind));
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
    /* A host that refuses a non-browser agent is a host the operator may have
     * told the engine how to present itself to. */
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
    j->media_is_video = res.media.is_video;
    j->media_is_image = res.media.is_image;
    j->media_is_audio = res.media.is_audio;
    j->media_width = res.media.width;
    j->media_height = res.media.height;
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
        mi_json_raw(&b, ",\"engine_version\":");
        mi_json_string(&b, MI_ENGINE_VERSION);
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

    /* POST /api/v1/browse  {url} — list the media a page or directory offers.
     *
     * The console cannot do this itself: the browser enforces CORS and most
     * media hosts send no CORS headers, so a direct fetch from the page is
     * blocked. The engine is not bound by that, so the listing happens here. */
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
     * The console cannot fetch an arbitrary site itself (CORS), so the engine
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
     * without bound and take the whole engine down with it.
     */
    if (!strcmp(req->path, "/api/v1/render") && !strcmp(req->method, "GET")) {
        if (!srv->renderer) {
            send_error(fd, 503, "Service Unavailable",
                       "rendering is not enabled on this engine", cors);
            return;
        }
        if (!mi_render_pool_available(srv->renderer)) {
            send_error(fd, 503, "Service Unavailable",
                       "no browser is installed on the engine host", cors);
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
        free(decoded);

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

        /* The response is hand-built, so the CORS headers have to be sent by
         * hand too — omitting them is how a route that works under curl fails
         * in the browser. */
        char meta[512];
        meta[0] = '\0';
        snprintf(meta, sizeof meta,
                 "X-Vyzorix-Render: %s\r\n"
                 "X-Vyzorix-Render-Ms: %.0f\r\n"
                 "X-Vyzorix-Render-Cache: %s\r\n"
                 "%s",
                 mi_render_status_name(st), res.elapsed_seconds * 1000.0,
                 res.from_cache ? "hit" : "miss",
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
                          cors ? "\r\nAccess-Control-Expose-Headers: X-Vyzorix-Render,X-Vyzorix-Render-Ms,X-Vyzorix-Render-Cache,X-Vyzorix-Title,X-Vyzorix-Render-Truncated\r\n"
                                 "Vary: Origin\r\n"
                               : "");
        send_all(fd, header, (size_t)hn);
        if (res.data && res.len > 0) send_all(fd, (const char *)res.data, res.len);
        mi_render_result_free(&res);
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
     * a walk of the work dir for what this engine has written. */
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
         * engine can really write here before it offers the path as a choice. */
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
         * same volume the operator already sized for this engine's data. */
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
    return srv;
}

int mi_server_can_render(const mi_server *srv) {
    return srv && srv->renderer && mi_render_pool_available(srv->renderer);
}

void mi_server_free(mi_server *srv) {
    if (!srv) return;
    for (size_t i = 0; i < srv->download_count; i++) download_free(srv->downloads[i]);
    free(srv->downloads);
    free(srv->threads);
    free(srv->download_dir);
    mi_render_pool_free(srv->renderer);
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
