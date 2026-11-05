/* mi_download.h - fast, resumable, concurrent download Vyzor (libcurl). */
#ifndef MI_DOWNLOAD_H
#define MI_DOWNLOAD_H

#include <stddef.h>
#include <stdint.h>

#include "mi_detect.h"

typedef struct {
    char *url;
    int64_t total_bytes; /* -1 when unknown */
    int accept_ranges;
    char *content_type;
    char *final_url;
    mi_media_info media;
    int have_media;
} mi_download_plan;

typedef struct {
    int64_t downloaded_bytes;
    int64_t total_bytes; /* -1 when unknown */
    double started_at;
    int finished;
} mi_progress;

typedef void (*mi_progress_fn)(const mi_progress *p, void *user);

/* Cooperative control for a running download. A worker polls `stop()` between
 * chunks; when it reports true the transfer is abandoned. The partial data is
 * left on disk and the state file records it, so the same call resumes later —
 * that is what makes pause/resume cheap rather than a restart.
 *
 * Both callbacks are invoked from worker threads and must be thread-safe. */
typedef struct {
    int (*stop)(void *user);
    void *user;
} mi_download_control;

typedef struct {
    int connections;
    int64_t chunk_size;
    int retries;
    double backoff;
    long timeout_seconds;
    /* Refuse payloads the detector does not recognise as video, image or
     * audio. Named for what it means now that all three are supported. */
    int require_media;
    int64_t max_bytes; /* 0 == unlimited */
    char **headers;    /* extra "Key: Value" headers */
    size_t header_count;
    /*
     * The User-Agent to send. NULL uses the built-in default.
     *
     * Worth being able to change: some hosts refuse a request that does not
     * look like a browser, and a caller fetching a page on the user's behalf
     * may need to say so. It is a request header like any other, so it travels
     * in the same list the extra headers do.
     */
    const char *user_agent;
    /* Optional: lets a caller stop a running download (pause/cancel). */
    const mi_download_control *control;
    /*
     * The ffmpeg binary, for a manifest.
     *
     * An HLS or DASH URL is a playlist, not a file, so it is read by ffmpeg —
     * which fetches every segment and muxes them. NULL uses "ffmpeg" from PATH.
     */
    const char *ffmpeg_path;
    /*
     * Whether to ask the extractor when the URL is a *page* rather than a file.
     *
     * A streaming site's media is not at the page URL, so an ordinary transfer
     * of it fetches HTML. With this set, a URL that probes as a non-video
     * document is offered to the extractor first, and its streams are muxed.
     * Off by default so a plain file download never pays for a subprocess.
     */
    int extract_pages;
    /* The extractor binary; NULL uses "yt-dlp" from PATH. */
    const char *extractor_path;
} mi_download_config;

typedef struct {
    char *url;
    char *path;
    int64_t bytes_written;
    mi_media_info media;
    int resumed;
    double elapsed_seconds;
} mi_download_result;

void mi_download_config_init(mi_download_config *cfg);

/* Fetch up to `max_bytes` of a URL as NUL-terminated text (for manifests).
 * Returns a malloc'd string, or NULL. Caller frees. */
char *mi_download_fetch_text(const mi_download_config *cfg, const char *url,
                             size_t max_bytes);

/*
 * What a fetch learned about the response, beyond its bytes.
 *
 * `framable` answers the one question that decides whether a page can be shown
 * inside the console: whether the site permits being embedded. `X-Frame-Options`
 * and a CSP `frame-ancestors` directive are the two ways a site says no, and
 * both are read here — a browser would enforce them, and reporting the refusal
 * up front is more useful than an embed that silently stays blank.
 */
typedef struct {
    char *final_url;     /* after any redirects; NULL when unchanged */
    char *content_type;  /* as sent; NULL when absent */
    int framable;        /* 1 unless the response forbids framing */
    char *frame_blocker; /* the header that forbade it, when one did */
} mi_fetch_info;

void mi_fetch_info_free(mi_fetch_info *info);

/* Like mi_download_fetch_text, but also reports what the response said. */
char *mi_download_fetch_document(const mi_download_config *cfg, const char *url,
                                 size_t max_bytes, mi_fetch_info *info);

/* Probe a URL without downloading the whole object. Returns 0/-1. */
int mi_probe(const mi_download_config *cfg, const char *url,
             mi_download_plan *out);
void mi_download_plan_free(mi_download_plan *plan);

/* Download `url` to `dest`, resuming from a prior partial if present.
 * `on_progress` (may be NULL) is invoked from worker threads and must be
 * thread-safe. Returns 0 or a negative MI_ERR_*. */
int mi_download(const mi_download_config *cfg, const char *url, const char *dest,
                mi_progress_fn on_progress, void *user,
                mi_download_result *out);
void mi_download_result_free(mi_download_result *res);

#endif /* MI_DOWNLOAD_H */
