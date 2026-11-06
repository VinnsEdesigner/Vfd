/* test_frontier.c - the crawl queue: in memory, in a local SQLite file, and
 * shared through Turso.
 *
 * The memory and SQLite backends are always tested (SQLite in a temp file, so no
 * account or network is needed). The Turso backend is tested only when
 * TURSO_DB_URL and TURSO_TOKEN are set — but when they are, it is a real test:
 * two frontiers on the same run id share one queue, and a URL claimed by one is
 * not handed to the other.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mi_frontier.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);   \
        }                                                                       \
    } while (0)

static void test_memory_fifo(void) {
    mi_frontier *f = mi_frontier_new_memory();
    CHECK(f != NULL, "memory frontier builds");
    CHECK(mi_frontier_empty(f), "starts empty");

    CHECK(mi_frontier_push(f, "http://a/", 0) == 0, "push a");
    CHECK(mi_frontier_push(f, "http://b/", 1) == 0, "push b");
    CHECK(mi_frontier_push(f, "http://c/", 1) == 0, "push c");
    CHECK(!mi_frontier_empty(f), "not empty after pushing");
    CHECK(mi_frontier_pending(f) == 3, "three pending");

    char *u = NULL;
    int d = -1;
    CHECK(mi_frontier_take(f, &u, &d) == 1, "take one");
    CHECK(u && !strcmp(u, "http://a/"), "first in, first out");
    CHECK(d == 0, "depth is carried");
    free(u);

    CHECK(mi_frontier_take(f, &u, &d) == 1 && !strcmp(u, "http://b/"), "take b");
    free(u);
    CHECK(mi_frontier_take(f, &u, &d) == 1 && !strcmp(u, "http://c/"), "take c");
    free(u);

    CHECK(mi_frontier_take(f, &u, &d) == 0, "empty once drained");
    mi_frontier_free(f);
}

static void test_memory_seen(void) {
    mi_frontier *f = mi_frontier_new_memory();
    CHECK(mi_frontier_mark_seen(f, "http://x/") == 1, "first sighting is new");
    CHECK(mi_frontier_mark_seen(f, "http://x/") == 0, "second sighting is not");
    CHECK(mi_frontier_mark_seen(f, "http://y/") == 1, "a different url is new");
    mi_frontier_free(f);
}

/* The local, durable backend: a real SQLite file, no account needed. */
static void test_sqlite_file(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/mi-frontier-test-%d.db", (int)getpid());
    unlink(path);
    char err[256];
    char run_id[64];
    snprintf(run_id, sizeof run_id, "sqlite-%d", (int)getpid());

    mi_frontier *f = mi_frontier_new_sqlite(path, run_id, err, sizeof err);
    CHECK(f != NULL, "sqlite frontier builds");
    if (!f) { fprintf(stderr, "  (%s)\n", err); return; }

    CHECK(mi_frontier_push(f, "http://one/", 0) == 0, "sqlite push");
    CHECK(mi_frontier_push(f, "http://two/", 1) == 0, "sqlite push two");
    CHECK(mi_frontier_mark_seen(f, "http://one/") == 1, "sqlite mark new");
    CHECK(mi_frontier_mark_seen(f, "http://one/") == 0, "sqlite mark seen again");
    CHECK(mi_frontier_pending(f) == 2, "sqlite reports pending");

    char *u = NULL;
    int d = -1;
    CHECK(mi_frontier_take(f, &u, &d) == 1, "sqlite take");
    CHECK(u && !strcmp(u, "http://one/"), "sqlite first in, first out");
    free(u);
    CHECK(mi_frontier_pending(f) == 1, "pending drops after a claim");
    mi_frontier_free(f);

    /*
     * A second frontier on the same file and run id resumes the queue: this is
     * the "survives a restart" property, tested by literally reopening the file.
     */
    mi_frontier *g = mi_frontier_new_sqlite(path, run_id, err, sizeof err);
    CHECK(g != NULL, "sqlite frontier reopens");
    if (g) {
        CHECK(mi_frontier_pending(g) == 1, "the reopened frontier resumes the queue");
        CHECK(mi_frontier_take(g, &u, &d) == 1 && !strcmp(u, "http://two/"),
              "the surviving url is the unclaimed one");
        free(u);
        CHECK(mi_frontier_take(g, &u, &d) == 0, "drained after resume");
        CHECK(mi_frontier_mark_seen(g, "http://one/") == 0,
              "the seen-set survived the restart");
        mi_frontier_free(g);
    }
    /* The -wal/-shm sidecars go with it. */
    unlink(path);
    char side[160];
    snprintf(side, sizeof side, "%s-wal", path);
    unlink(side);
    snprintf(side, sizeof side, "%s-shm", path);
    unlink(side);
}

/* The distributed backend, against a real database when one is configured. */
static void test_turso_shared(void) {
    const char *url = getenv("TURSO_DB_URL");
    const char *token = getenv("TURSO_TOKEN");
    if (!url || !*url) {
        printf("skip: no TURSO_DB_URL, so the shared frontier is not exercised\n");
        return;
    }
    char err[256];
    char run_id[64];
    snprintf(run_id, sizeof run_id, "test-%d", (int)getpid());

    mi_frontier *a = mi_frontier_new_turso(url, token, run_id, err, sizeof err);
    CHECK(a != NULL, "turso frontier builds");
    if (!a) { fprintf(stderr, "  (%s)\n", err); return; }
    mi_frontier *b = mi_frontier_new_turso(url, token, run_id, err, sizeof err);
    CHECK(b != NULL, "a second worker joins the same run");

    CHECK(mi_frontier_push(a, "http://one/", 0) == 0, "push through worker a");
    CHECK(mi_frontier_push(a, "http://two/", 0) == 0, "push two");

    char *u1 = NULL, *u2 = NULL;
    int d1 = -1, d2 = -1;
    int got1 = mi_frontier_take(a, &u1, &d1);
    int got2 = mi_frontier_take(b, &u2, &d2);
    CHECK(got1 == 1 && got2 == 1, "both workers got a url");
    CHECK(u1 && u2 && strcmp(u1, u2) != 0, "the two workers did not get the same url");
    free(u1); free(u2);

    CHECK(mi_frontier_mark_seen(a, "http://z/") == 1, "a marks z new");
    CHECK(mi_frontier_mark_seen(b, "http://z/") == 0, "b sees z as already visited");

    mi_frontier_free(a);
    mi_frontier_free(b);
}

int main(void) {
    test_memory_fifo();
    test_memory_seen();
    test_sqlite_file();
    test_turso_shared();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
