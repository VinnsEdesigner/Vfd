/* mi_cache.h - a small, bounded, disk-backed cache.
 *
 * Written for rendered pages, because a render costs about half a gigabyte and
 * a second or two, and the same page is often looked at twice: once to see it,
 * once to check something. A cache turns the second look into a file read.
 *
 * Bounded is the point. An unbounded cache is a slow disk leak, so this one
 * holds a total byte budget and an entry count, and evicts the least recently
 * used entry when either is exceeded. Entries also expire, so a page that has
 * changed is not served from yesterday.
 *
 * The payload lives on disk and only the index is in memory. Memory is this
 * engine's binding constraint — a render holds half a gigabyte while it runs —
 * so the cache must not compete with it, and a screenshot is not something to
 * keep resident.
 */
#ifndef MI_CACHE_H
#define MI_CACHE_H

#include <stddef.h>
#include <stdint.h>

typedef struct mi_cache mi_cache;

typedef struct {
    /* The payload. Owned by the caller once returned; free it. */
    unsigned char *data;
    size_t len;
    char *content_type;
    char *title;
    /* How long the original took, so a cached answer can still say. */
    double render_ms;
} mi_cache_entry;

void mi_cache_entry_free(mi_cache_entry *e);

/*
 * Create a cache under `dir`.
 *
 * `max_bytes` and `max_entries` bound it; 0 means the built-in default. The
 * directory is created if needed. Returns NULL only when it cannot be made.
 */
mi_cache *mi_cache_new(const char *dir, size_t max_bytes, size_t max_entries,
                       int ttl_seconds);
void mi_cache_free(mi_cache *c);

/* A hit returns 1 and fills `out`; a miss, an expired entry or a missing file
 * returns 0. Thread-safe. */
int mi_cache_get(mi_cache *c, const char *key, mi_cache_entry *out);

/*
 * Store a payload. Fails silently when it cannot be written — a cache that
 * cannot store is slower, not broken, and must never fail the request it is
 * serving.
 */
void mi_cache_put(mi_cache *c, const char *key, const unsigned char *data, size_t len,
                  const char *content_type, const char *title, double render_ms);

/* Drop everything. */
void mi_cache_clear(mi_cache *c);

/* For reporting: how many entries, how many bytes, and the hit/miss counts. */
void mi_cache_stats(mi_cache *c, size_t *entries, size_t *bytes, uint64_t *hits,
                    uint64_t *misses);

#endif /* MI_CACHE_H */
