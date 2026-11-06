#include "mi_frontier.h"
#include "mi_turso.h"
#include "mi_util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* In-memory backend                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    char *url;
    int depth;
} mem_entry;

struct mi_frontier {
    /* NULL for the in-memory backend. */
    char *db_url;
    char *db_token;
    char *run_id;
    /* The in-memory queue, and the set already seen. */
    mem_entry *items;
    size_t head, count, cap;
    char **seen;
    size_t seen_count, seen_cap;
    /* One process must not race itself; the database arbitrates across
     * processes. */
    pthread_mutex_t lock;
};

static mi_frontier *base_new(void) {
    mi_frontier *f = mi_calloc(1, sizeof *f);
    if (!f) return NULL;
    pthread_mutex_init(&f->lock, NULL);
    return f;
}

mi_frontier *mi_frontier_new_memory(void) { return base_new(); }

mi_frontier *mi_frontier_new_turso(const char *url, const char *token,
                                   const char *run_id, char *err, size_t errcap) {
    if (err && errcap) err[0] = '\0';
    if (!url || !*url || !run_id || !*run_id) {
        if (err && errcap) snprintf(err, errcap, "missing database url or run id");
        return NULL;
    }
    mi_frontier *f = base_new();
    if (!f) return NULL;
    f->db_url = mi_strdup(url);
    f->db_token = mi_strdup(token ? token : "");
    f->run_id = mi_strdup(run_id);

    /*
     * The schema.
     *
     * `frontier` holds the queue (claimed_at NULL means "still to visit");
     * `visited` holds everything ever enqueued, which is what makes the walk
     * skip a page two others both link. The primary keys make the inserts
     * idempotent, which is what makes it safe for several workers.
     */
    static const char *SCHEMA[] = {
        "CREATE TABLE IF NOT EXISTS frontier ("
        " run_id TEXT NOT NULL, url TEXT NOT NULL, depth INTEGER NOT NULL,"
        " claimed_at TEXT, PRIMARY KEY (run_id, url))",
        "CREATE TABLE IF NOT EXISTS visited ("
        " run_id TEXT NOT NULL, url TEXT NOT NULL, PRIMARY KEY (run_id, url))",
    };
    for (size_t i = 0; i < sizeof SCHEMA / sizeof SCHEMA[0]; i++) {
        int status = 0;
        mi_json *r = mi_turso_execute(f->db_url, f->db_token, SCHEMA[i], NULL, 0,
                                      &status, err, errcap);
        if (!r) {
            mi_frontier_free(f);
            return NULL;
        }
        mi_json_value_free(r);
    }
    return f;
}

/* ------------------------------------------------------------------ */
/* push                                                                */
/* ------------------------------------------------------------------ */

static int mem_push(mi_frontier *f, const char *url, int depth) {
    if (f->count == f->cap) {
        size_t want = f->cap ? f->cap * 2 : 64;
        mem_entry *grown = mi_realloc(f->items, want * sizeof *grown);
        if (!grown) return -1;
        f->items = grown;
        f->cap = want;
    }
    f->items[f->count].url = mi_strdup(url);
    if (!f->items[f->count].url) return -1;
    f->items[f->count].depth = depth;
    f->count++;
    return 0;
}

int mi_frontier_push(mi_frontier *f, const char *url, int depth) {
    if (!f || !url) return -1;
    if (!f->db_url) return mem_push(f, url, depth);

    char depth_s[16];
    snprintf(depth_s, sizeof depth_s, "%d", depth);
    mi_turso_arg args[] = {MI_TURSO_TEXT(f->run_id), MI_TURSO_TEXT(url),
                           MI_TURSO_INT(depth_s)};
    char err[160];
    int status = 0;
    mi_json *r = mi_turso_execute(
        f->db_url, f->db_token,
        "INSERT OR IGNORE INTO frontier (run_id, url, depth) VALUES (?, ?, ?)",
        args, 3, &status, err, sizeof err);
    if (!r) {
        mi_logf(MI_LOG_ERROR, "frontier push failed: %s", err);
        return -1;
    }
    mi_json_value_free(r);
    return 0;
}

/* ------------------------------------------------------------------ */
/* mark_seen                                                           */
/* ------------------------------------------------------------------ */

static int mem_mark_seen(mi_frontier *f, const char *url) {
    for (size_t i = 0; i < f->seen_count; i++) {
        if (!strcmp(f->seen[i], url)) return 0;
    }
    if (f->seen_count == f->seen_cap) {
        size_t want = f->seen_cap ? f->seen_cap * 2 : 128;
        char **grown = mi_realloc(f->seen, want * sizeof *grown);
        if (!grown) return 1; /* fail open: a duplicate visit beats a skipped page */
        f->seen = grown;
        f->seen_cap = want;
    }
    f->seen[f->seen_count++] = mi_strdup(url);
    return 1;
}

int mi_frontier_mark_seen(mi_frontier *f, const char *url) {
    if (!f || !url) return 0;
    if (!f->db_url) return mem_mark_seen(f, url);

    /*
     * "Insert, and tell me whether it was new."
     *
     * `changes()` after an INSERT OR IGNORE is 1 when a row was added and 0 when
     * it already existed — "new vs seen" in one atomic statement, with no
     * read-then-write race between two workers. Both statements go in one call so
     * they share a connection; a second HTTP call would read `changes()` as 0.
     */
    mi_turso_arg ins[] = {MI_TURSO_TEXT(f->run_id), MI_TURSO_TEXT(url)};
    mi_turso_stmt stmts[] = {
        {.sql = "INSERT OR IGNORE INTO visited (run_id, url) VALUES (?, ?)",
         .args = ins, .nargs = 2},
        {.sql = "SELECT changes() AS added", .args = NULL, .nargs = 0},
    };
    char err[160];
    int status = 0;
    mi_json *r = mi_turso_pipeline(f->db_url, f->db_token, stmts, 2, &status, err,
                                   sizeof err);
    if (!r) {
        mi_logf(MI_LOG_ERROR, "frontier mark_seen failed: %s", err);
        return 1; /* fail open */
    }
    const char *added = mi_turso_cell_text(r, 0, 0);
    int is_new = added && atoi(added) > 0;
    mi_json_value_free(r);
    return is_new;
}

/* ------------------------------------------------------------------ */
/* take                                                                */
/* ------------------------------------------------------------------ */

/* A claim token that is unique per call: pid + a monotonic nanosecond clock.
 * It is how the row just claimed is identified, since libSQL has no RETURNING. */
static void make_token(char *out, size_t cap) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(out, cap, "%d-%lld.%09ld", (int)getpid(), (long long)ts.tv_sec,
             (long)ts.tv_nsec);
}

static int mem_take(mi_frontier *f, char **url, int *depth) {
    if (f->head >= f->count) return 0;
    mem_entry e = f->items[f->head++];
    *url = mi_strdup(e.url);
    *depth = e.depth;
    return *url ? 1 : -1;
}

int mi_frontier_take(mi_frontier *f, char **url, int *depth) {
    if (!f || !url || !depth) return -1;
    if (!f->db_url) return mem_take(f, url, depth);

    pthread_mutex_lock(&f->lock);
    int result = 0;
    for (int tries = 0; tries < 20 && result == 0; tries++) {
        char token[64];
        make_token(token, sizeof token);
        char err[200];
        int status = 0;

        /*
         * Claim one row atomically, in one statement.
         *
         * `UPDATE ... RETURNING` picks the oldest unclaimed row and hands it
         * back in the same atomic step: the worker that wins the row gets its
         * url and depth, and a worker that loses gets no rows. There is no
         * read-then-write window for two workers to both take the same page, and
         * no need for a second `SELECT changes()` round trip.
         */
        mi_turso_arg up[] = {MI_TURSO_TEXT(token), MI_TURSO_TEXT(f->run_id),
                             MI_TURSO_TEXT(f->run_id)};
        mi_json *r = mi_turso_execute(
            f->db_url, f->db_token,
            "UPDATE frontier SET claimed_at = ?"
            " WHERE run_id = ? AND claimed_at IS NULL"
            " AND rowid = (SELECT rowid FROM frontier"
            "              WHERE run_id = ? AND claimed_at IS NULL"
            "              ORDER BY rowid LIMIT 1)"
            " RETURNING url, depth",
            up, 3, &status, err, sizeof err);
        if (!r) {
            mi_logf(MI_LOG_ERROR, "frontier take failed: %s", err);
            pthread_mutex_unlock(&f->lock);
            return -1;
        }
        if (mi_turso_row_count(r) >= 1) {
            const char *u = mi_turso_cell_text(r, 0, 0);
            const char *d = mi_turso_cell_text(r, 0, 1);
            char *copy = mi_strdup(u ? u : "");
            if (copy) {
                *url = copy;
                *depth = d ? atoi(d) : 0;
                result = 1;
            } else {
                result = -1;
            }
        }
        mi_json_value_free(r);
        /* No rows means nothing left to claim (a lost race would have returned
         * the row to someone else, and this worker simply tries again). */
        if (result == 0) {
            mi_json *any = mi_turso_execute(
                f->db_url, f->db_token,
                "SELECT 1 FROM frontier WHERE run_id = ? AND claimed_at IS NULL LIMIT 1",
                (mi_turso_arg[]){MI_TURSO_TEXT(f->run_id)}, 1, &status, err, sizeof err);
            int more = any && mi_turso_row_count(any) > 0;
            mi_json_value_free(any);
            if (!more) break;
        }
    }
    pthread_mutex_unlock(&f->lock);
    return result;
}

int mi_frontier_empty(mi_frontier *f) {
    if (!f) return 1;
    if (!f->db_url) return f->head >= f->count;
    mi_turso_arg args[] = {MI_TURSO_TEXT(f->run_id)};
    char err[160];
    int status = 0;
    mi_json *r = mi_turso_execute(
        f->db_url, f->db_token,
        "SELECT count(*) FROM frontier WHERE run_id = ? AND claimed_at IS NULL",
        args, 1, &status, err, sizeof err);
    if (!r) return 1;
    const char *n = mi_turso_cell_text(r, 0, 0);
    int empty = !n || atoi(n) == 0;
    mi_json_value_free(r);
    return empty;
}

void mi_frontier_free(mi_frontier *f) {
    if (!f) return;
    free(f->db_url);
    free(f->db_token);
    free(f->run_id);
    for (size_t i = 0; i < f->count; i++) free(f->items[i].url);
    free(f->items);
    for (size_t i = 0; i < f->seen_count; i++) free(f->seen[i]);
    free(f->seen);
    pthread_mutex_destroy(&f->lock);
    free(f);
}
