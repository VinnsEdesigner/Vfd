/* mi_download.h - fast, resumable, concurrent download engine (libcurl). */
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
