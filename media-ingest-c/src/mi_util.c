#define _GNU_SOURCE
#include "mi_util.h"

#include <ctype.h>
#include <limits.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static mi_log_level g_level = MI_LOG_INFO;

/* ------------------------------------------------------------------ */
/* The log ring                                                        */
/*                                                                    */
/* The lines the server has logged lately, so an API can serve them.   */
/* A fixed array used as a ring: bounded memory, and an old line is    */
/* overwritten rather than the buffer growing. Guarded by one mutex,   */
/* because log lines come from every worker thread.                    */
/* ------------------------------------------------------------------ */
#define LOG_RING_CAP 500

static struct {
    pthread_mutex_t lock;
    int enabled;
    uint64_t next_seq; /* the seq the next line will get; 1-based */
    size_t count;      /* how many slots hold a line */
    size_t head;       /* where the next line is written */
    mi_log_record rec[LOG_RING_CAP];
} g_ring = {.lock = PTHREAD_MUTEX_INITIALIZER};

void mi_log_ring_enable(int on) {
    pthread_mutex_lock(&g_ring.lock);
    g_ring.enabled = on != 0;
    /* A fresh start: keeping lines from before the toggle would make the seq
     * the caller last saw ambiguous. */
    g_ring.count = 0;
    g_ring.head = 0;
    pthread_mutex_unlock(&g_ring.lock);
}

int mi_log_ring_enabled(void) {
    pthread_mutex_lock(&g_ring.lock);
    int on = g_ring.enabled;
    pthread_mutex_unlock(&g_ring.lock);
    return on;
}

static void ring_append(int level, const char *text) {
    if (!g_ring.enabled) return;
    pthread_mutex_lock(&g_ring.lock);
    mi_log_record *r = &g_ring.rec[g_ring.head];
    r->seq = ++g_ring.next_seq;
    r->ts = (double)time(NULL);
    r->level = level;
    /* Truncate, because some lines carry a signed URL thousands of bytes long. */
    snprintf(r->text, sizeof r->text, "%s", text);
    g_ring.head = (g_ring.head + 1) % LOG_RING_CAP;
    if (g_ring.count < LOG_RING_CAP) g_ring.count++;
    pthread_mutex_unlock(&g_ring.lock);
}

size_t mi_log_snapshot(uint64_t since, mi_log_record **out) {
    if (out) *out = NULL;
    pthread_mutex_lock(&g_ring.lock);
    /* Count first, so the array is sized exactly and no realloc is needed. */
    size_t n = 0;
    for (size_t i = 0; i < g_ring.count; i++) {
        size_t idx = (g_ring.head + LOG_RING_CAP - g_ring.count + i) % LOG_RING_CAP;
        if (g_ring.rec[idx].seq > since) n++;
    }
    mi_log_record *arr = n ? mi_malloc(n * sizeof *arr) : NULL;
    size_t w = 0;
    if (arr) {
        for (size_t i = 0; i < g_ring.count; i++) {
            size_t idx = (g_ring.head + LOG_RING_CAP - g_ring.count + i) % LOG_RING_CAP;
            if (g_ring.rec[idx].seq > since) arr[w++] = g_ring.rec[idx];
        }
    }
    pthread_mutex_unlock(&g_ring.lock);
    if (out) *out = arr;
    return arr ? w : 0;
}

void mi_log_snapshot_free(mi_log_record *p) { free(p); }

void mi_log_set_level(mi_log_level level) { g_level = level; }
mi_log_level mi_log_get_level(void) { return g_level; }

void mi_logf(mi_log_level level, const char *fmt, ...) {
    if (level > g_level) return;
    const char *tag = level == MI_LOG_ERROR ? "error"
                    : level == MI_LOG_INFO  ? "info"
                                            : "debug";
    fprintf(stderr, "[%s] ", tag);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);

    /*
     * The same line into the ring, so an API can serve it. Formatted separately
     * rather than captured from the stderr write, because stderr is a FILE the
     * caller may have redirected and this must work regardless.
     */
    if (g_ring.enabled) {
        char buf[MI_LOG_LINE_CAP];
        va_list ap2;
        va_start(ap2, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap2);
        va_end(ap2);
        ring_append(level, buf);
    }
}

void *mi_malloc(size_t n) { return malloc(n ? n : 1); }
void *mi_calloc(size_t n, size_t sz) { return calloc(n ? n : 1, sz ? sz : 1); }
void *mi_realloc(void *p, size_t n) { return realloc(p, n ? n : 1); }

char *mi_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = mi_malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *mi_strndup(const char *s, size_t n) {
    if (!s) return NULL;
    size_t len = 0;
    while (len < n && s[len]) len++;
    char *p = mi_malloc(len + 1);
    if (p) {
        memcpy(p, s, len);
        p[len] = '\0';
    }
    return p;
}

char *mi_asprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return NULL;
    }
    char *buf = mi_malloc((size_t)n + 1);
    if (buf) vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return buf;
}

char *mi_path_join(const char *a, const char *b) {
    if (!a || !*a) return mi_strdup(b);
    if (!b || !*b) return mi_strdup(a);
    size_t la = strlen(a);
    int needs_slash = a[la - 1] != '/';
    return mi_asprintf("%s%s%s", a, needs_slash ? "/" : "", b);
}

int mi_path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

int mi_path_is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int64_t mi_file_size(const char *path) {
    struct stat st;
    if (!path || stat(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

int mi_mkdir_p(const char *path) {
    if (!path || !*path) return -1;
    char *tmp = mi_strdup(path);
    if (!tmp) return -1;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
                free(tmp);
                return -1;
            }
            *p = '/';
        }
    }
    int rc = (mkdir(tmp, 0775) != 0 && errno != EEXIST) ? -1 : 0;
    free(tmp);
    return rc;
}

int mi_rm_rf(const char *path) {
    if (!path) return -1;
    struct stat st;
    if (lstat(path, &st) != 0) return 0; /* already gone */
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0 ? 0 : -1;

    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *ent;
    int rc = 0;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char *child = mi_path_join(path, ent->d_name);
        if (!child) {
            rc = -1;
            break;
        }
        if (mi_rm_rf(child) != 0) rc = -1;
        free(child);
    }
    closedir(d);
    if (rmdir(path) != 0) rc = -1;
    return rc;
}

char *mi_url_encode(const char *s) {
    if (!s) return NULL;
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(s);
    char *out = mi_malloc(n * 3 + 1);
    if (!out) return NULL;
    char *w = out;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            *w++ = (char)*p;
        } else {
            *w++ = '%';
            *w++ = hex[*p >> 4];
            *w++ = hex[*p & 0xF];
        }
    }
    *w = '\0';
    return out;
}

ssize_t mi_read_head(const char *path, size_t max, unsigned char **out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char *buf = mi_malloc(max + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t got = fread(buf, 1, max, f);
    fclose(f);
    buf[got] = '\0';
    *out = buf;
    return (ssize_t)got;
}

int mi_fs_stats(const char *path, mi_fs_info *out) {
    if (!path || !out) return -1;
    struct statvfs vfs;
    if (statvfs(path, &vfs) != 0) return -1;
    /* f_frsize is the fragment size; blocks are expressed in it. */
    uint64_t unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    out->total_bytes = (int64_t)(vfs.f_blocks * unit);
    out->free_bytes = (int64_t)(vfs.f_bfree * unit);
    out->avail_bytes = (int64_t)(vfs.f_bavail * unit);
    return 0;
}

/* Parse one /proc/mounts line into device, mount point and fstype.
 * The kernel escapes spaces and tabs in the path as \040 and \011. */
static int parse_mount_line(const char *line, char *dev, size_t dev_size,
                            char *mp, size_t mp_size, char *type, size_t type_size) {
    const char *p = line;
    size_t fields = 0;
    char *outs[3] = {dev, mp, type};
    size_t sizes[3] = {dev_size, mp_size, type_size};
    while (*p && fields < 3) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') break;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        size_t len = (size_t)(p - start);
        if (len >= sizes[fields]) len = sizes[fields] - 1;
        memcpy(outs[fields], start, len);
        outs[fields][len] = '\0';
        fields++;
    }
    return fields == 3 ? 0 : -1;
}

int mi_mount_info(const char *path, char *mount_point, size_t mp_size,
                  char *fs_type, size_t ft_size, char *device, size_t dev_size) {
    if (mount_point && mp_size) mount_point[0] = '\0';
    if (fs_type && ft_size) fs_type[0] = '\0';
    if (device && dev_size) device[0] = '\0';
    if (!path || !mount_point || mp_size == 0) return -1;

    /* The work dir may not exist yet on first run, so fall back to the nearest
     * ancestor that does: that is still the volume it will be created on. */
    char candidate[PATH_MAX];
    snprintf(candidate, sizeof candidate, "%s", path);
    char real[PATH_MAX];
    for (;;) {
        if (realpath(candidate, real)) break;
        if (!strcmp(candidate, "/")) return -1;
        char *slash = strrchr(candidate, '/');
        if (!slash) return -1;
        if (slash == candidate) snprintf(candidate, sizeof candidate, "/");
        else *slash = '\0';
    }
    struct stat st;
    if (stat(real, &st) != 0) return -1;

    /* The mount point is the highest ancestor that is still on the same device:
     * walk up while the parent reports the same st_dev. */
    char cur[PATH_MAX];
    snprintf(cur, sizeof cur, "%s", real);
    for (;;) {
        if (!strcmp(cur, "/")) break;
        char parent[PATH_MAX];
        snprintf(parent, sizeof parent, "%s", cur);
        char *slash = strrchr(parent, '/');
        if (!slash) break;
        if (slash == parent) snprintf(parent, sizeof parent, "/");
        else *slash = '\0';
        struct stat pst;
        if (stat(parent, &pst) != 0) break;
        if (pst.st_dev != st.st_dev) break; /* `cur` is the mount point */
        snprintf(cur, sizeof cur, "%s", parent);
    }
    snprintf(mount_point, mp_size, "%s", cur);

    /* Then read the type and backing device for that mount point. */
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;
    char line[4096];
    char d[512], m[512], t[128];
    while (fgets(line, sizeof line, f)) {
        if (parse_mount_line(line, d, sizeof d, m, sizeof m, t, sizeof t) != 0) continue;
        if (strcmp(m, cur) != 0) continue;
        if (fs_type && ft_size) snprintf(fs_type, ft_size, "%s", t);
        if (device && dev_size) snprintf(device, dev_size, "%s", d);
        break;
    }
    fclose(f);
    return 0;
}

int mi_dir_usage(const char *path, int64_t *total_bytes, int64_t *file_count) {
    if (!path) return -1;
    if (total_bytes) *total_bytes = 0;
    if (file_count) *file_count = 0;

    DIR *d = opendir(path);
    if (!d) return -1;

    int64_t bytes = 0;
    int64_t count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char *child = mi_path_join(path, ent->d_name);
        if (!child) continue;
        struct stat st;
        if (lstat(child, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                int64_t sub_bytes = 0, sub_count = 0;
                if (mi_dir_usage(child, &sub_bytes, &sub_count) == 0) {
                    bytes += sub_bytes;
                    count += sub_count;
                }
            } else if (S_ISREG(st.st_mode)) {
                bytes += (int64_t)st.st_size;
                count += 1;
            }
        }
        free(child);
    }
    closedir(d);

    if (total_bytes) *total_bytes = bytes;
    if (file_count) *file_count = count;
    return 0;
}

double mi_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------ */
/* SHA-256 (FIPS 180-4), self-contained so we need no crypto library.  */
/* ------------------------------------------------------------------ */

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static void sha256_transform(mi_sha256_ctx *ctx, const unsigned char *data) {
    uint32_t m[64];
    for (int i = 0, j = 0; i < 16; i++, j += 4)
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(m[i - 15], 7) ^ ROTR(m[i - 15], 18) ^ (m[i - 15] >> 3);
        uint32_t s1 = ROTR(m[i - 2], 17) ^ ROTR(m[i - 2], 19) ^ (m[i - 2] >> 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + m[i];
        uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void mi_sha256_init(mi_sha256_ctx *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

void mi_sha256_update(mi_sha256_ctx *ctx, const void *vdata, size_t len) {
    const unsigned char *data = vdata;
    for (size_t i = 0; i < len; i++) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

void mi_sha256_final(mi_sha256_ctx *ctx, unsigned char out[32]) {
    size_t i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0x00;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += (uint64_t)ctx->datalen * 8;
    ctx->data[63] = (unsigned char)(ctx->bitlen);
    ctx->data[62] = (unsigned char)(ctx->bitlen >> 8);
    ctx->data[61] = (unsigned char)(ctx->bitlen >> 16);
    ctx->data[60] = (unsigned char)(ctx->bitlen >> 24);
    ctx->data[59] = (unsigned char)(ctx->bitlen >> 32);
    ctx->data[58] = (unsigned char)(ctx->bitlen >> 40);
    ctx->data[57] = (unsigned char)(ctx->bitlen >> 48);
    ctx->data[56] = (unsigned char)(ctx->bitlen >> 56);
    sha256_transform(ctx, ctx->data);
    unsigned char *hash = out;
    for (i = 0; i < 4; i++) {
        hash[i] = (unsigned char)(ctx->state[0] >> (24 - i * 8));
        hash[i + 4] = (unsigned char)(ctx->state[1] >> (24 - i * 8));
        hash[i + 8] = (unsigned char)(ctx->state[2] >> (24 - i * 8));
        hash[i + 12] = (unsigned char)(ctx->state[3] >> (24 - i * 8));
        hash[i + 16] = (unsigned char)(ctx->state[4] >> (24 - i * 8));
        hash[i + 20] = (unsigned char)(ctx->state[5] >> (24 - i * 8));
        hash[i + 24] = (unsigned char)(ctx->state[6] >> (24 - i * 8));
        hash[i + 28] = (unsigned char)(ctx->state[7] >> (24 - i * 8));
    }
}

int mi_sha256_file(const char *path, char *out_hex) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    mi_sha256_ctx ctx;
    mi_sha256_init(&ctx);
    unsigned char buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) mi_sha256_update(&ctx, buf, got);
    int bad = ferror(f);
    fclose(f);
    if (bad) return -1;
    unsigned char hash[32];
    mi_sha256_final(&ctx, hash);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2] = hex[hash[i] >> 4];
        out_hex[i * 2 + 1] = hex[hash[i] & 0xF];
    }
    out_hex[64] = '\0';
    return 0;
}

void mi_sha256(const void *data, size_t len, unsigned char out[32]) {
    mi_sha256_ctx ctx;
    mi_sha256_init(&ctx);
    mi_sha256_update(&ctx, data, len);
    mi_sha256_final(&ctx, out);
}

void mi_hex_encode(const unsigned char *in, size_t len, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0xF];
    }
    out[len * 2] = '\0';
}

void mi_sha256_hex(const void *data, size_t len, char *out_hex) {
    unsigned char digest[32];
    mi_sha256(data, len, digest);
    mi_hex_encode(digest, 32, out_hex);
}

/* ------------------------------------------------------------------ */
/* SHA-1 (RFC 3174)                                                    */
/* ------------------------------------------------------------------ */

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_transform(uint32_t state[5], const unsigned char block[64]) {
    uint32_t w[80];
    for (int i = 0, j = 0; i < 16; i++, j += 4)
        w[i] = ((uint32_t)block[j] << 24) | ((uint32_t)block[j + 1] << 16) |
               ((uint32_t)block[j + 2] << 8) | ((uint32_t)block[j + 3]);
    for (int i = 16; i < 80; i++)
        w[i] = ROTL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        uint32_t tmp = ROTL32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROTL32(b, 30); b = a; a = tmp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

void mi_sha1(const void *vdata, size_t len, unsigned char out[20]) {
    const unsigned char *data = vdata;
    uint32_t state[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    uint64_t bits = (uint64_t)len * 8;

    size_t full = len / 64;
    for (size_t i = 0; i < full; i++) sha1_transform(state, data + i * 64);

    /* The tail: remaining bytes, a 0x80, zero padding, then the 64-bit length
     * big-endian, in one or two blocks. */
    unsigned char tail[128];
    size_t rem = len - full * 64;
    memset(tail, 0, sizeof tail);
    memcpy(tail, data + full * 64, rem);
    tail[rem] = 0x80;
    size_t tail_len = rem < 56 ? 64 : 128;
    for (int i = 0; i < 8; i++)
        tail[tail_len - 1 - i] = (unsigned char)(bits >> (8 * i));
    sha1_transform(state, tail);
    if (tail_len == 128) sha1_transform(state, tail + 64);

    for (int i = 0; i < 5; i++) {
        out[i * 4] = (unsigned char)(state[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(state[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(state[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(state[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Base64 (RFC 4648)                                                   */
/* ------------------------------------------------------------------ */

static const char B64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *mi_base64_encode(const unsigned char *data, size_t len) {
    size_t groups = (len + 2) / 3;
    char *out = mi_malloc(groups * 4 + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        int rem = (int)(len - i);
        if (rem > 1) v |= (uint32_t)data[i + 1] << 8;
        if (rem > 2) v |= (uint32_t)data[i + 2];
        out[o++] = B64_ALPHABET[(v >> 18) & 0x3F];
        out[o++] = B64_ALPHABET[(v >> 12) & 0x3F];
        out[o++] = rem > 1 ? B64_ALPHABET[(v >> 6) & 0x3F] : '=';
        out[o++] = rem > 2 ? B64_ALPHABET[v & 0x3F] : '=';
    }
    out[o] = '\0';
    return out;
}

/* -1 for a byte that is not in the alphabet. */
static int b64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

ssize_t mi_base64_decode(const char *in, size_t inlen, unsigned char *out, size_t cap) {
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < inlen; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        int v = b64_value(c);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    return (ssize_t)o;
}

void mi_hmac_sha256(const void *key, size_t keylen, const void *msg, size_t len,
                    unsigned char out[32]) {
    unsigned char k[64] = {0};
    if (keylen > 64) {
        mi_sha256(key, keylen, k); /* long keys are hashed first */
    } else {
        memcpy(k, key, keylen);
    }
    unsigned char ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    mi_sha256_ctx ctx;
    unsigned char inner[32];
    mi_sha256_init(&ctx);
    mi_sha256_update(&ctx, ipad, 64);
    mi_sha256_update(&ctx, msg, len);
    mi_sha256_final(&ctx, inner);
    mi_sha256_init(&ctx);
    mi_sha256_update(&ctx, opad, 64);
    mi_sha256_update(&ctx, inner, 32);
    mi_sha256_final(&ctx, out);
}

void mi_amz_timestamps(time_t when, char amz[17], char date[9]) {
    struct tm tmv;
    gmtime_r(&when, &tmv);
    strftime(amz, 17, "%Y%m%dT%H%M%SZ", &tmv);
    strftime(date, 9, "%Y%m%d", &tmv);
}

int mi_which(const char *name) {
    if (!name || !*name) return 0;
    /* An explicit path is checked directly. */
    if (strchr(name, '/')) return access(name, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path || !*path) return 0;
    char *copy = mi_strdup(path);
    if (!copy) return 0;
    int found = 0;
    for (char *p = copy; p && *p;) {
        char *colon = strchr(p, ':');
        if (colon) *colon = '\0';
        char *cand = mi_asprintf("%s/%s", *p ? p : ".", name);
        if (cand) {
            if (access(cand, X_OK) == 0) found = 1;
            free(cand);
        }
        if (found) break;
        p = colon ? colon + 1 : NULL;
    }
    free(copy);
    return found;
}
