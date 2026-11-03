/* mi_util.h - small shared helpers (errors, strings, files, logging). */
#ifndef MI_UTIL_H
#define MI_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>

/* Return codes. 0 == success, negative == failure. */
enum {
    MI_OK = 0,
    MI_ERR = -1,
    MI_ERR_ARG = -2,
    MI_ERR_IO = -3,
    MI_ERR_NOMEM = -4,
    MI_ERR_NET = -5,
    MI_ERR_ABORTED = -6
};

/* The engine's own version. Reported by `--help` and by the API, so it lives in
 * one place rather than being spelled out in each. */
#define MI_ENGINE_VERSION "0.1.0"

/* Log levels, controlled by mi_log_set_level(). */
typedef enum { MI_LOG_QUIET = 0, MI_LOG_ERROR, MI_LOG_INFO, MI_LOG_DEBUG } mi_log_level;

void mi_log_set_level(mi_log_level level);
mi_log_level mi_log_get_level(void);
void mi_logf(mi_log_level level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* xmalloc family: abort-on-OOM is avoided; callers check for NULL. */
void *mi_malloc(size_t n);
void *mi_calloc(size_t n, size_t sz);
void *mi_realloc(void *p, size_t n);
char *mi_strdup(const char *s);
char *mi_strndup(const char *s, size_t n);

/* printf into a freshly allocated string. */
char *mi_asprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Filesystem helpers. Return 0/-1. */
int mi_mkdir_p(const char *path);
int mi_rm_rf(const char *path);
int mi_path_exists(const char *path);
int mi_path_is_dir(const char *path);
int64_t mi_file_size(const char *path); /* -1 on error */

/* Join two path components into a newly allocated string. */
char *mi_path_join(const char *a, const char *b);

/* Percent-encode a string for use in a URL query (allocated). */
char *mi_url_encode(const char *s);

/* Read the first `max` bytes of a file into a buffer (allocated, NUL-terminated).
 * Returns the number of bytes read, or -1 on error. */
ssize_t mi_read_head(const char *path, size_t max, unsigned char **out);

/* SHA-256 of a file, lowercase hex into `out` (needs >= 65 bytes). */
int mi_sha256_file(const char *path, char *out_hex);

/* ------------------------------------------------------------------ */
/* SHA-256 / HMAC-SHA256 (FIPS 180-4 / RFC 2104)                       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    unsigned char data[64];
    size_t datalen;
} mi_sha256_ctx;

void mi_sha256_init(mi_sha256_ctx *ctx);
void mi_sha256_update(mi_sha256_ctx *ctx, const void *data, size_t len);
void mi_sha256_final(mi_sha256_ctx *ctx, unsigned char out[32]);
void mi_sha256(const void *data, size_t len, unsigned char out[32]);
void mi_hmac_sha256(const void *key, size_t keylen, const void *msg, size_t len,
                    unsigned char out[32]);

/* Lowercase hex encoding; `out` needs 2*len + 1 bytes. */
void mi_hex_encode(const unsigned char *in, size_t len, char *out);

/* SHA-256 of a byte range, lowercase hex into `out_hex` (>= 65 bytes). */
void mi_sha256_hex(const void *data, size_t len, char *out_hex);

/* Format a time_t as the two SigV4 stamps. `amz` needs 17, `date` needs 9. */
void mi_amz_timestamps(time_t when, char amz[17], char date[9]);

/* ------------------------------------------------------------------ */
/* Filesystem                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int64_t total_bytes;   /* filesystem size */
    int64_t free_bytes;    /* free to an unprivileged user */
    int64_t avail_bytes;   /* free including reserved */
} mi_fs_info;

/* Fill in filesystem figures for the volume holding `path`. 0/-1. */
int mi_fs_stats(const char *path, mi_fs_info *out);

/* Identify the volume holding `path`: its mount point, filesystem type and
 * backing device, read from the mount table. Any output may be NULL. Returns 0
 * when the mount point was resolved (the type/device are best-effort). */
int mi_mount_info(const char *path, char *mount_point, size_t mp_size,
                  char *fs_type, size_t ft_size, char *device, size_t dev_size);

/* Recursively total the regular files under `path`.
 * Returns 0 and writes the byte total and file count. */
int mi_dir_usage(const char *path, int64_t *total_bytes, int64_t *file_count);

/* Whether `name` resolves to an executable on PATH (or an explicit path).
 * Returns 1 when found. */
int mi_which(const char *name);

/* Monotonic clock in seconds. */
double mi_now(void);

#endif /* MI_UTIL_H */
