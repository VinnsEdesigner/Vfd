/* mi_storage.h - S3-compatible object storage upload + CDN cache purge.
 *
 * Speaks the S3 REST API with AWS Signature Version 4, so it works against
 * AWS S3, Cloudflare R2, MinIO, Backblaze B2 (S3 endpoint), DigitalOcean
 * Spaces, Wasabi, and so on. Optionally purges a Cloudflare zone cache.
 */
#ifndef MI_STORAGE_H
#define MI_STORAGE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *endpoint;       /* https://s3.amazonaws.com, https://<acct>.r2... */
    const char *region;         /* e.g. "us-east-1" or "auto" for R2 */
    const char *bucket;
    const char *access_key;
    const char *secret_key;
    const char *session_token;  /* optional (STS) */
    const char *content_type;   /* optional override; default "application/octet-stream" */
    int path_style;             /* 1 -> bucket in the path (MinIO/R2), 0 -> vhost */
    long timeout_seconds;
    int verify_tls;             /* default 1 */
} mi_s3_config;

typedef struct {
    char *url;        /* public URL of the uploaded object (or CDN base + key) */
    char *etag;       /* ETag returned by the server, if any */
    int64_t bytes;
    long http_status;
} mi_upload_result;

/* Cloudflare cache purge (optional post-upload step). */
typedef struct {
    const char *api_token;   /* bearer token with Zone.Cache Purge */
    const char *zone_id;
    long timeout_seconds;
} mi_cdn_config;

typedef struct {
    long http_status;
    int success;
    char *body;   /* raw response body (caller frees) */
} mi_cdn_result;

void mi_s3_config_init(mi_s3_config *cfg);
void mi_cdn_config_init(mi_cdn_config *cfg);

/* Upload a local file to `object_key`. Returns 0 or a negative MI_ERR_*. */
int mi_s3_upload_file(const mi_s3_config *cfg, const char *local_path,
                      const char *object_key, mi_upload_result *out);

/* Upload an in-memory buffer. */
int mi_s3_upload_bytes(const mi_s3_config *cfg, const void *data, size_t len,
                       const char *object_key, mi_upload_result *out);

void mi_upload_result_free(mi_upload_result *res);

/* Purge one or more absolute URLs from the Cloudflare cache. */
int mi_cdn_purge_urls(const mi_cdn_config *cfg, const char *const *urls,
                      size_t count, mi_cdn_result *out);

/* Build the public URL for a key (CDN base if set, else the S3 URL). */
char *mi_storage_public_url(const mi_s3_config *cfg, const char *cdn_base,
                            const char *object_key);

void mi_cdn_result_free(mi_cdn_result *res);

#endif /* MI_STORAGE_H */
