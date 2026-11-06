#include "mi_frontier.h"
#include "mi_turso.h"
#include "mi_util.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef enum { BACKEND_MEMORY, BACKEND_SQLITE, BACKEND_TURSO } backend_kind;

/* ------------------------------------------------------------------ */
/* In-memory backend                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    char *url;
    int depth;
} mem_entry;

struct mi_frontier {
    backend_kind kind;
    /* Shared-backend state. */
    sqlite3 *db;      /* SQLITE */
    char *db_url;     /* TURSO  */
    char *db_token;   /* TURSO  */
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

static mi_frontier *base_new(backend_kind kind) {
    mi_frontier *f = mi_calloc(1, sizeof *f);
    if (!f) return NULL;
    f->kind = kind;
    pthread_mutex_init(&f->lock, NULL);
    return f;
}

mi_frontier *mi_frontier_new_memory(void) { return base_new(BACKEND_MEMORY); }

/* ------------------------------------------------------------------ */
/* Schema, shared by both databases                                    */
/* ------------------------------------------------------------------ */

/*
 * `frontier` holds the queue (claimed_at NULL means "still to visit"); `visited`
 * holds everything ever enqueued, which is what makes the walk skip a page two
 * others both link. The primary keys make the inserts idempotent, which is what
 * makes it safe for several workers.
 */
static const char *SCHEMA[] = {
    "CREATE TABLE IF NOT EXISTS frontier ("
    " run_id TEXT NOT NULL, url TEXT NOT NULL, depth INTEGER NOT NULL,"
    " claimed_at TEXT, PRIMARY KEY (run_id, url))",
    "CREATE TABLE IF NOT EXISTS visited ("
    " run_id TEXT NOT NULL, url TEXT NOT NULL, PRIMARY KEY (run_id, url))",
};

/* ------------------------------------------------------------------ */
/* SQLite backend                                                      */
/* ------------------------------------------------------------------ */

/* Run one statement, ignoring its rows. Returns 0 on success. */
static int sqlite_exec(sqlite3 *db, const char *sql, char *err, size_t errcap) {
    char *msg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &msg);
    if (rc != SQLITE_OK) {
        if (err && errcap) snprintf(err, errcap, "%s", msg ? msg : "sqlite error");
        sqlite3_free(msg);
        return -1;
    }
    return 0;
}

mi_frontier *mi_frontier_new_sqlite(const char *path, const char *run_id,
                                    char *err, size_t errcap) {
    if (err && errcap) err[0] = '\0';
    if (!path || !*path || !run_id || !*run_id) {
        if (err && errcap) snprintf(err, errcap, "missing database path or run id");
        return NULL;
    }
    mi_frontier *f = base_new(BACKEND_SQLITE);
    if (!f) return NULL;
    f->run_id = mi_strdup(run_id);

    if (sqlite3_open(path, &f->db) != SQLITE_OK) {
        if (err && errcap) snprintf(err, errcap, "cannot open %s: %s", path,
                                    f->db ? sqlite3_errmsg(f->db) : "out of memory");
        mi_frontier_free(f);
        return NULL;
    }
    /* A crawl is a background job; a lock held for the seconds a page takes
     * would stall every other worker. WAL lets a reader proceed while one
     * writer holds the lock, and the busy timeout makes a brief overlap wait
     * rather than fail. */
    sqlite3_busy_timeout(f->db, 5000);
    if (sqlite_exec(f->db, "PRAGMA journal_mode=WAL", err, errcap) != 0 ||
        sqlite_exec(f->db, "PRAGMA synchronous=NORMAL", err, errcap) != 0) {
        mi_frontier_free(f);
        return NULL;
    }
    for (size_t i = 0; i < sizeof SCHEMA / sizeof SCHEMA[0]; i++) {
        if (sqlite_exec(f->db, SCHEMA[i], err, errcap) != 0) {
            mi_frontier_free(f);
            return NULL;
        }
    }
    return f;
}

/* A prepared-statement helper that binds the run id and one URL. */
static int sqlite_two_arg(sqlite3 *db, const char *sql, const char *a, const char *b,
                          char *err, size_t errcap) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        if (err && errcap) snprintf(err, errcap, "%s", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_text(st, 1, a, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, b, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        if (err && errcap) snprintf(err, errcap, "%s", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

static int sqlite_push(mi_frontier *f, const char *url, int depth) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(f->db,
            "INSERT OR IGNORE INTO frontier (run_id, url, depth) VALUES (?, ?, ?)",
            -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, f->run_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, depth);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int sqlite_mark_seen(mi_frontier *f, const char *url) {
    char err[160];
    if (sqlite_two_arg(f->db,
            "INSERT OR IGNORE INTO visited (run_id, url) VALUES (?, ?)",
            f->run_id, url, err, sizeof err) != 0) {
        return 1; /* fail open: a duplicate visit beats a skipped page */
    }
    return sqlite3_changes(f->db) > 0;
}

/*
 * Claim one row atomically.
 *
 * `UPDATE ... RETURNING` picks the oldest unclaimed row and hands it back in one
 * step, so two workers on one file cannot both take the same URL. The mutex
 * serialises this process against itself; SQLite's own lock serialises processes.
 */
static int sqlite_take(mi_frontier *f, char **url, int *depth) {
    pthread_mutex_lock(&f->lock);
    char token[64];
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(token, sizeof token, "%d-%lld.%09ld", (int)getpid(),
             (long long)ts.tv_sec, (long)ts.tv_nsec);

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(f->db,
        "UPDATE frontier SET claimed_at = ?"
        " WHERE run_id = ? AND claimed_at IS NULL"
        " AND rowid = (SELECT rowid FROM frontier"
        "              WHERE run_id = ? AND claimed_at IS NULL"
        "              ORDER BY rowid LIMIT 1)"
        " RETURNING url, depth",
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&f->lock);
        return -1;
    }
    sqlite3_bind_text(st, 1, token, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, f->run_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, f->run_id, -1, SQLITE_TRANSIENT);

    int result = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *u = sqlite3_column_text(st, 0);
        int d = sqlite3_column_int(st, 1);
        char *copy = mi_strdup(u ? (const char *)u : "");
        if (copy) {
            *url = copy;
            *depth = d;
            result = 1;
        } else {
            result = -1;
        }
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&f->lock);
    return result;
}

static int sqlite_empty(mi_frontier *f) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(f->db,
            "SELECT 1 FROM frontier WHERE run_id = ? AND claimed_at IS NULL LIMIT 1",
            -1, &st, NULL) != SQLITE_OK) {
        return 1;
    }
    sqlite3_bind_text(st, 1, f->run_id, -1, SQLITE_TRANSIENT);
    int has = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return !has;
}

static long sqlite_pending(mi_frontier *f) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(f->db,
            "SELECT count(*) FROM frontier WHERE run_id = ? AND claimed_at IS NULL",
            -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, f->run_id, -1, SQLITE_TRANSIENT);
    long n = -1;
    if (sqlite3_step(st) == SQLITE_ROW) n = (long)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

/* ------------------------------------------------------------------ */
/* Turso backend                                                       */
/* ------------------------------------------------------------------ */

mi_frontier *mi_frontier_new_turso(const char *url, const char *token,
                                   const char *run_id, char *err, size_t errcap) {
    if (err && errcap) err[0] = '\0';
    if (!url || !*url || !run_id || !*run_id) {
        if (err && errcap) snprintf(err, errcap, "missing database url or run id");
        return NULL;
    }
    mi_frontier *f = base_new(BACKEND_TURSO);
    if (!f) return NULL;
    f->db_url = mi_strdup(url);
    f->db_token = mi_strdup(token ? token : "");
    f->run_id = mi_strdup(run_id);

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

static int turso_push(mi_frontier *f, const char *url, int depth) {
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

static int turso_mark_seen(mi_frontier *f, const char *url) {
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
        return 1;
    }
    const char *added = mi_turso_cell_text(r, 0, 0);
    int is_new = added && atoi(added) > 0;
    mi_json_value_free(r);
    return is_new;
}

static int turso_take(mi_frontier *f, char **url, int *depth) {
    pthread_mutex_lock(&f->lock);
    int result = 0;
    for (int tries = 0; tries < 20 && result == 0; tries++) {
        char token[64];
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        snprintf(token, sizeof token, "%d-%lld.%09ld", (int)getpid(),
                 (long long)ts.tv_sec, (long)ts.tv_nsec);
        char err[200];
        int status = 0;
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

static int turso_empty(mi_frontier *f) {
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

static long turso_pending(mi_frontier *f) {
    mi_turso_arg args[] = {MI_TURSO_TEXT(f->run_id)};
    char err[160];
    int status = 0;
    mi_json *r = mi_turso_execute(
        f->db_url, f->db_token,
        "SELECT count(*) FROM frontier WHERE run_id = ? AND claimed_at IS NULL",
        args, 1, &status, err, sizeof err);
    if (!r) return -1;
    const char *n = mi_turso_cell_text(r, 0, 0);
    long v = n ? atol(n) : -1;
    mi_json_value_free(r);
    return v;
}

/* ------------------------------------------------------------------ */
/* In-memory backend                                                   */
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

static int mem_mark_seen(mi_frontier *f, const char *url) {
    for (size_t i = 0; i < f->seen_count; i++) {
        if (!strcmp(f->seen[i], url)) return 0;
    }
    if (f->seen_count == f->seen_cap) {
        size_t want = f->seen_cap ? f->seen_cap * 2 : 128;
        char **grown = mi_realloc(f->seen, want * sizeof *grown);
        if (!grown) return 1;
        f->seen = grown;
        f->seen_cap = want;
    }
    f->seen[f->seen_count++] = mi_strdup(url);
    return 1;
}

static int mem_take(mi_frontier *f, char **url, int *depth) {
    if (f->head >= f->count) return 0;
    mem_entry e = f->items[f->head++];
    *url = mi_strdup(e.url);
    *depth = e.depth;
    return *url ? 1 : -1;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

int mi_frontier_push(mi_frontier *f, const char *url, int depth) {
    if (!f || !url) return -1;
    switch (f->kind) {
        case BACKEND_SQLITE: return sqlite_push(f, url, depth);
        case BACKEND_TURSO: return turso_push(f, url, depth);
        default: return mem_push(f, url, depth);
    }
}

int mi_frontier_mark_seen(mi_frontier *f, const char *url) {
    if (!f || !url) return 0;
    switch (f->kind) {
        case BACKEND_SQLITE: return sqlite_mark_seen(f, url);
        case BACKEND_TURSO: return turso_mark_seen(f, url);
        default: return mem_mark_seen(f, url);
    }
}

int mi_frontier_take(mi_frontier *f, char **url, int *depth) {
    if (!f || !url || !depth) return -1;
    switch (f->kind) {
        case BACKEND_SQLITE: return sqlite_take(f, url, depth);
        case BACKEND_TURSO: return turso_take(f, url, depth);
        default: return mem_take(f, url, depth);
    }
}

int mi_frontier_empty(mi_frontier *f) {
    if (!f) return 1;
    switch (f->kind) {
        case BACKEND_SQLITE: return sqlite_empty(f);
        case BACKEND_TURSO: return turso_empty(f);
        default: return f->head >= f->count;
    }
}

long mi_frontier_pending(mi_frontier *f) {
    if (!f) return -1;
    switch (f->kind) {
        case BACKEND_SQLITE: return sqlite_pending(f);
        case BACKEND_TURSO: return turso_pending(f);
        default: return (long)(f->count - f->head);
    }
}

void mi_frontier_free(mi_frontier *f) {
    if (!f) return;
    if (f->db) sqlite3_close(f->db);
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
