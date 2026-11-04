/* test_cache.c - the bounded disk cache.
 *
 * The interesting behaviour is the bounding: an unbounded cache is a slow disk
 * leak, so the tests that matter are that the byte budget and the entry count
 * are both honoured, that the least recently used entry is the one evicted, and
 * that an expired entry is not served. Everything here runs on the real
 * filesystem, because the whole point of the cache is that the payload is on
 * disk.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mi_cache.h"
#include "mi_util.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
        }                                                                   \
    } while (0)

#define CACHE_DIR "/tmp/mi-test-cache"

/* A payload of `n` bytes with a recognisable first character. */
static void fill(unsigned char *buf, size_t n, unsigned char tag) {
    memset(buf, tag, n);
}

/* How many payload files the cache directory holds. */
static int payload_count(void) {
    DIR *d = opendir(CACHE_DIR);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len > 4 && !strcmp(e->d_name + len - 4, ".bin")) n++;
    }
    closedir(d);
    return n;
}

static void test_basic_roundtrip(void) {
    mi_rm_rf(CACHE_DIR);
    mi_cache *c = mi_cache_new(CACHE_DIR, 1024 * 1024, 10, 600);
    CHECK(c != NULL, "cache created");
    if (!c) return;

    unsigned char data[100];
    fill(data, sizeof data, 'A');
    mi_cache_put(c, "key-one", data, sizeof data, "image/png", "A page", 1234.0);

    mi_cache_entry e;
    CHECK(mi_cache_get(c, "key-one", &e) == 1, "a stored key is found");
    CHECK(e.len == sizeof data, "the length is kept");
    CHECK(e.data && e.data[0] == 'A' && e.data[99] == 'A', "the payload is intact");
    CHECK(e.content_type && !strcmp(e.content_type, "image/png"), "the content type is kept");
    CHECK(e.title && !strcmp(e.title, "A page"), "the title is kept");
    CHECK(e.render_ms == 1234.0, "the original render time is kept");
    mi_cache_entry_free(&e);

    /* A key that was never stored. */
    CHECK(mi_cache_get(c, "nope", &e) == 0, "an unknown key misses");
    mi_cache_entry_free(&e);

    /* The payload really is on disk, not in memory. */
    CHECK(payload_count() == 1, "the payload is a file in the cache directory");

    /* The same key again replaces rather than duplicating. */
    fill(data, sizeof data, 'B');
    mi_cache_put(c, "key-one", data, sizeof data, "image/png", "A page", 1.0);
    CHECK(mi_cache_get(c, "key-one", &e) == 1, "the replaced key is found");
    CHECK(e.data && e.data[0] == 'B', "the replacement is what is served");
    mi_cache_entry_free(&e);
    CHECK(payload_count() == 1, "replacing does not leave a second file");

    size_t entries = 0, bytes = 0;
    uint64_t hits = 0, misses = 0;
    mi_cache_stats(c, &entries, &bytes, &hits, &misses);
    CHECK(entries == 1, "one entry is counted");
    CHECK(bytes == sizeof data, "the byte total is right");
    CHECK(hits == 2 && misses == 1, "hits and misses are counted");

    mi_cache_free(c);
    mi_rm_rf(CACHE_DIR);
}

static void test_entry_limit_evicts_lru(void) {
    mi_rm_rf(CACHE_DIR);
    /* Room for two entries only. */
    mi_cache *c = mi_cache_new(CACHE_DIR, 1024 * 1024, 2, 600);
    if (!c) {
        CHECK(0, "cache created");
        return;
    }

    unsigned char data[64];
    fill(data, sizeof data, '1');
    mi_cache_put(c, "a", data, sizeof data, "image/png", NULL, 1);
    fill(data, sizeof data, '2');
    mi_cache_put(c, "b", data, sizeof data, "image/png", NULL, 1);

    /* Touch `a`, so `b` becomes the least recently used. */
    mi_cache_entry e;
    CHECK(mi_cache_get(c, "a", &e) == 1, "a is present before the eviction");
    mi_cache_entry_free(&e);

    fill(data, sizeof data, '3');
    mi_cache_put(c, "c", data, sizeof data, "image/png", NULL, 1);

    size_t entries = 0;
    mi_cache_stats(c, &entries, NULL, NULL, NULL);
    CHECK(entries == 2, "the entry limit is honoured");

    /* `b` was used least recently, so it is the one that went. */
    CHECK(mi_cache_get(c, "b", &e) == 0, "the least recently used entry was evicted");
    mi_cache_entry_free(&e);
    CHECK(mi_cache_get(c, "a", &e) == 1, "the recently used entry survived");
    mi_cache_entry_free(&e);
    CHECK(mi_cache_get(c, "c", &e) == 1, "the newest entry is present");
    mi_cache_entry_free(&e);
    CHECK(payload_count() == 2, "the evicted payload's file was deleted");

    mi_cache_free(c);
    mi_rm_rf(CACHE_DIR);
}

static void test_byte_limit(void) {
    mi_rm_rf(CACHE_DIR);
    /* Room for three 100-byte payloads, and no more. */
    mi_cache *c = mi_cache_new(CACHE_DIR, 300, 100, 600);
    if (!c) {
        CHECK(0, "cache created");
        return;
    }

    unsigned char data[100];
    for (int i = 0; i < 5; i++) {
        fill(data, sizeof data, (unsigned char)('a' + i));
        char key[8];
        snprintf(key, sizeof key, "k%d", i);
        mi_cache_put(c, key, data, sizeof data, "image/png", NULL, 1);
    }

    size_t entries = 0, bytes = 0;
    mi_cache_stats(c, &entries, &bytes, NULL, NULL);
    CHECK(bytes <= 300, "the byte budget is honoured");
    CHECK(entries <= 3, "no more entries fit than the budget allows");
    CHECK(payload_count() == (int)entries, "the directory matches the index");

    /* The newest is certainly there; the oldest certainly is not. */
    mi_cache_entry e;
    CHECK(mi_cache_get(c, "k4", &e) == 1, "the newest entry is kept");
    mi_cache_entry_free(&e);
    CHECK(mi_cache_get(c, "k0", &e) == 0, "the oldest entry was evicted for room");
    mi_cache_entry_free(&e);

    mi_cache_free(c);
    mi_rm_rf(CACHE_DIR);
}

static void test_oversized_entry_is_not_stored(void) {
    mi_rm_rf(CACHE_DIR);
    mi_cache *c = mi_cache_new(CACHE_DIR, 100, 10, 600);
    if (!c) {
        CHECK(0, "cache created");
        return;
    }
    /* Larger than the whole budget: storing it would evict everything and still
     * not fit, so it must simply not be cached. */
    unsigned char big[500];
    fill(big, sizeof big, 'X');
    mi_cache_put(c, "huge", big, sizeof big, "image/png", NULL, 1);

    size_t entries = 0;
    mi_cache_stats(c, &entries, NULL, NULL, NULL);
    CHECK(entries == 0, "an entry larger than the budget is not stored");

    mi_cache_entry e;
    CHECK(mi_cache_get(c, "huge", &e) == 0, "and it cannot be fetched either");
    mi_cache_entry_free(&e);

    mi_cache_free(c);
    mi_rm_rf(CACHE_DIR);
}

static void test_ttl_expires(void) {
    mi_rm_rf(CACHE_DIR);
    /* A one-second TTL, so a short sleep is enough. */
    mi_cache *c = mi_cache_new(CACHE_DIR, 1024 * 1024, 10, 1);
    if (!c) {
        CHECK(0, "cache created");
        return;
    }
    unsigned char data[32];
    fill(data, sizeof data, 'T');
    mi_cache_put(c, "soon", data, sizeof data, "image/png", NULL, 1);

    mi_cache_entry e;
    CHECK(mi_cache_get(c, "soon", &e) == 1, "present before it expires");
    mi_cache_entry_free(&e);

    /* Long enough for the TTL to pass. */
    struct timespec ts = {.tv_sec = 2, .tv_nsec = 0};
    nanosleep(&ts, NULL);

    CHECK(mi_cache_get(c, "soon", &e) == 0, "an expired entry is not served");
    mi_cache_entry_free(&e);
    size_t entries = 0;
    mi_cache_stats(c, &entries, NULL, NULL, NULL);
    CHECK(entries == 0, "and it was dropped rather than left");

    mi_cache_free(c);
    mi_rm_rf(CACHE_DIR);
}

static void test_missing_payload_is_a_miss(void) {
    mi_rm_rf(CACHE_DIR);
    mi_cache *c = mi_cache_new(CACHE_DIR, 1024 * 1024, 10, 600);
    if (!c) {
        CHECK(0, "cache created");
        return;
    }
    unsigned char data[32];
    fill(data, sizeof data, 'M');
    mi_cache_put(c, "gone", data, sizeof data, "image/png", NULL, 1);

    /* Delete the payload behind the index, as a manual clean would. */
    DIR *d = opendir(CACHE_DIR);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d))) {
            if (strstr(ent->d_name, ".bin")) {
                char *p = mi_asprintf("%s/%s", CACHE_DIR, ent->d_name);
                unlink(p);
                free(p);
            }
        }
        closedir(d);
    }

    mi_cache_entry e;
    CHECK(mi_cache_get(c, "gone", &e) == 0, "a missing payload is a miss, not a crash");
    mi_cache_entry_free(&e);
    size_t entries = 0;
    mi_cache_stats(c, &entries, NULL, NULL, NULL);
    CHECK(entries == 0, "the stale index entry was dropped");

    mi_cache_free(c);
    mi_rm_rf(CACHE_DIR);
}

static void test_clear(void) {
    mi_rm_rf(CACHE_DIR);
    mi_cache *c = mi_cache_new(CACHE_DIR, 1024 * 1024, 10, 600);
    if (!c) {
        CHECK(0, "cache created");
        return;
    }
    unsigned char data[32];
    fill(data, sizeof data, 'C');
    mi_cache_put(c, "one", data, sizeof data, "image/png", NULL, 1);
    mi_cache_put(c, "two", data, sizeof data, "image/png", NULL, 1);
    mi_cache_clear(c);

    size_t entries = 0, bytes = 0;
    mi_cache_stats(c, &entries, &bytes, NULL, NULL);
    CHECK(entries == 0 && bytes == 0, "clear empties the index");
    CHECK(payload_count() == 0, "clear deletes the payloads too");

    mi_cache_free(c);
    mi_rm_rf(CACHE_DIR);
}

int main(void) {
    mi_log_set_level(MI_LOG_ERROR);
    test_basic_roundtrip();
    test_entry_limit_evicts_lru();
    test_byte_limit();
    test_oversized_entry_is_not_stored();
    test_ttl_expires();
    test_missing_payload_is_a_miss();
    test_clear();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
