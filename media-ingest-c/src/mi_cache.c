/* mi_cache.c - a small, bounded, disk-backed cache. */

#define _GNU_SOURCE
#include "mi_cache.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "mi_util.h"

#define DEFAULT_MAX_BYTES (64u * 1024u * 1024u)
#define DEFAULT_MAX_ENTRIES 64
#define DEFAULT_TTL_SECONDS 600

typedef struct mi_cache_node {
    char *key;
    /* The payload's file name inside the cache directory. */
    char *file;
    size_t len;
    char *content_type;
    char *title;
    double render_ms;
    double stored_at;
    /* Monotonic use counter: the smallest is the least recently used. */
    uint64_t used;
    struct mi_cache_node *next;
} mi_cache_node;

struct mi_cache {
    char *dir;
    size_t max_bytes;
    size_t max_entries;
    int ttl_seconds;
    size_t bytes;
    size_t count;
    uint64_t clock;
    uint64_t hits;
    uint64_t misses;
    mi_cache_node *head;
    pthread_mutex_t lock;
};

void mi_cache_entry_free(mi_cache_entry *e) {
    if (!e) return;
    free(e->data);
    free(e->content_type);
    free(e->title);
    memset(e, 0, sizeof *e);
}

/* ------------------------------------------------------------------ */
/* Keys                                                                */
/* ------------------------------------------------------------------ */

/* FNV-1a. The key is a URL plus a mode, so it is hashed to a file name: a URL
 * is not a legal file name (slashes, colons, question marks) and two keys that
 * differ only in case must not collide on a case-insensitive filesystem. */
static void key_hash(const char *key, char out[33]) {
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        h ^= *p;
        h *= 1099511628211ull;
    }
    snprintf(out, 33, "%016llx", (unsigned long long)h);
}

static char *file_for(const mi_cache *c, const char *key) {
    char hash[33];
    key_hash(key, hash);
    return mi_asprintf("%s/%s.bin", c->dir, hash);
}

/* ------------------------------------------------------------------ */
/* List management                                                     */
/* ------------------------------------------------------------------ */

static void node_free(mi_cache_node *n) {
    if (!n) return;
    free(n->key);
    free(n->file);
    free(n->content_type);
    free(n->title);
    free(n);
}

static mi_cache_node *find(mi_cache *c, const char *key) {
    for (mi_cache_node *n = c->head; n; n = n->next) {
        if (!strcmp(n->key, key)) return n;
    }
    return NULL;
}

/* Unlink a node and delete its payload. Caller holds the lock. */
static void evict(mi_cache *c, mi_cache_node *target) {
    mi_cache_node **pp = &c->head;
    while (*pp && *pp != target) pp = &(*pp)->next;
    if (!*pp) return;
    *pp = target->next;
    unlink(target->file);
    c->bytes -= target->len;
    if (c->count > 0) c->count--;
    node_free(target);
}

/* Drop the least recently used entries until both budgets fit. */
static void trim(mi_cache *c) {
    while (c->head && (c->bytes > c->max_bytes || c->count > c->max_entries)) {
        mi_cache_node *lru = c->head;
        for (mi_cache_node *n = c->head; n; n = n->next) {
            if (n->used < lru->used) lru = n;
        }
        evict(c, lru);
    }
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */

mi_cache *mi_cache_new(const char *dir, size_t max_bytes, size_t max_entries,
                       int ttl_seconds) {
    if (!dir || !*dir) return NULL;
    mi_cache *c = mi_calloc(1, sizeof *c);
    if (!c) return NULL;
    c->dir = mi_strdup(dir);
    if (!c->dir) {
        free(c);
        return NULL;
    }
    if (mi_mkdir_p(c->dir) != 0) {
        mi_logf(MI_LOG_ERROR, "cache: cannot create %s", c->dir);
        free(c->dir);
        free(c);
        return NULL;
    }
    c->max_bytes = max_bytes ? max_bytes : DEFAULT_MAX_BYTES;
    c->max_entries = max_entries ? max_entries : DEFAULT_MAX_ENTRIES;
    c->ttl_seconds = ttl_seconds > 0 ? ttl_seconds : DEFAULT_TTL_SECONDS;
    pthread_mutex_init(&c->lock, NULL);
    return c;
}

void mi_cache_free(mi_cache *c) {
    if (!c) return;
    /* The payloads are left on disk: they are still valid, and a restart should
     * not have to re-render everything. Only the index goes. */
    mi_cache_node *n = c->head;
    while (n) {
        mi_cache_node *next = n->next;
        node_free(n);
        n = next;
    }
    pthread_mutex_destroy(&c->lock);
    free(c->dir);
    free(c);
}

int mi_cache_get(mi_cache *c, const char *key, mi_cache_entry *out) {
    if (!c || !key || !out) return 0;
    memset(out, 0, sizeof *out);

    pthread_mutex_lock(&c->lock);
    mi_cache_node *n = find(c, key);
    if (!n) {
        c->misses++;
        pthread_mutex_unlock(&c->lock);
        return 0;
    }

    /* Expired: drop it rather than serving a stale page. */
    double now = mi_now();
    if (c->ttl_seconds > 0 && now - n->stored_at > c->ttl_seconds) {
        evict(c, n);
        c->misses++;
        pthread_mutex_unlock(&c->lock);
        return 0;
    }

    /* Read the payload while holding the lock: the file must not be evicted
     * between the lookup and the read, or a concurrent put could unlink it. */
    FILE *f = fopen(n->file, "rb");
    if (!f) {
        /* The index points at a file that is gone; drop the entry. */
        evict(c, n);
        c->misses++;
        pthread_mutex_unlock(&c->lock);
        return 0;
    }
    unsigned char *data = mi_malloc(n->len ? n->len : 1);
    size_t got = data ? fread(data, 1, n->len, f) : 0;
    fclose(f);
    if (!data || got != n->len) {
        free(data);
        evict(c, n);
        c->misses++;
        pthread_mutex_unlock(&c->lock);
        return 0;
    }

    out->data = data;
    out->len = got;
    out->content_type = mi_strdup(n->content_type);
    out->title = n->title ? mi_strdup(n->title) : NULL;
    out->render_ms = n->render_ms;
    n->used = ++c->clock;
    c->hits++;
    pthread_mutex_unlock(&c->lock);
    return 1;
}

void mi_cache_put(mi_cache *c, const char *key, const unsigned char *data, size_t len,
                  const char *content_type, const char *title, double render_ms) {
    if (!c || !key || !data || len == 0) return;
    /* An entry larger than the whole budget would evict everything and still
     * not fit, so it is simply not cached. */
    if (len > c->max_bytes) return;

    pthread_mutex_lock(&c->lock);

    /* Replace an existing entry in place, so its payload file is reused. */
    mi_cache_node *n = find(c, key);
    if (n) {
        FILE *f = fopen(n->file, "wb");
        if (!f) {
            evict(c, n);
            pthread_mutex_unlock(&c->lock);
            return;
        }
        size_t wrote = fwrite(data, 1, len, f);
        fclose(f);
        if (wrote != len) {
            evict(c, n);
            pthread_mutex_unlock(&c->lock);
            return;
        }
        c->bytes -= n->len;
        n->len = len;
        c->bytes += len;
        free(n->content_type);
        n->content_type = mi_strdup(content_type);
        free(n->title);
        n->title = title ? mi_strdup(title) : NULL;
        n->render_ms = render_ms;
        n->stored_at = mi_now();
        n->used = ++c->clock;
        trim(c);
        pthread_mutex_unlock(&c->lock);
        return;
    }

    char *file = file_for(c, key);
    if (!file) {
        pthread_mutex_unlock(&c->lock);
        return;
    }
    FILE *f = fopen(file, "wb");
    if (!f) {
        free(file);
        pthread_mutex_unlock(&c->lock);
        return;
    }
    size_t wrote = fwrite(data, 1, len, f);
    fclose(f);
    if (wrote != len) {
        unlink(file);
        free(file);
        pthread_mutex_unlock(&c->lock);
        return;
    }

    mi_cache_node *fresh = mi_calloc(1, sizeof *fresh);
    if (!fresh) {
        unlink(file);
        free(file);
        pthread_mutex_unlock(&c->lock);
        return;
    }
    fresh->key = mi_strdup(key);
    fresh->file = file;
    fresh->len = len;
    fresh->content_type = mi_strdup(content_type);
    fresh->title = title ? mi_strdup(title) : NULL;
    fresh->render_ms = render_ms;
    fresh->stored_at = mi_now();
    fresh->used = ++c->clock;
    fresh->next = c->head;
    c->head = fresh;
    c->bytes += len;
    c->count++;

    trim(c);
    pthread_mutex_unlock(&c->lock);
}

void mi_cache_clear(mi_cache *c) {
    if (!c) return;
    pthread_mutex_lock(&c->lock);
    mi_cache_node *n = c->head;
    while (n) {
        mi_cache_node *next = n->next;
        unlink(n->file);
        node_free(n);
        n = next;
    }
    c->head = NULL;
    c->bytes = 0;
    c->count = 0;
    pthread_mutex_unlock(&c->lock);
}

void mi_cache_stats(mi_cache *c, size_t *entries, size_t *bytes, uint64_t *hits,
                    uint64_t *misses) {
    if (!c) return;
    pthread_mutex_lock(&c->lock);
    if (entries) *entries = c->count;
    if (bytes) *bytes = c->bytes;
    if (hits) *hits = c->hits;
    if (misses) *misses = c->misses;
    pthread_mutex_unlock(&c->lock);
}
