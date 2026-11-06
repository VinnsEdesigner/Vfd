/* mi_frontier.h - the set of URLs to visit, and the set already visited.
 *
 * A crawl is a breadth-first walk over this. Keeping it behind an interface
 * lets the same walk run in one process (an in-memory queue), on this machine
 * (a SQLite file), or across several machines (a Turso database), without the
 * walk knowing which.
 *
 * The shared backends are the point: two crawlers pointed at the same `run_id`
 * share one frontier, so a large site is walked once rather than once per
 * process. Claiming the next URL is a single atomic statement, so two workers
 * never take the same page.
 *
 * The local one needs nothing but a file, so a single server is durable and
 * resumable without any cloud account; the remote one is for when the work is
 * split across hosts.
 */
#ifndef MI_FRONTIER_H
#define MI_FRONTIER_H

#include <stddef.h>

typedef struct mi_frontier mi_frontier;

/* An in-process queue. The backend a single-process, throwaway crawl uses. */
mi_frontier *mi_frontier_new_memory(void);

/*
 * A frontier in a local SQLite file.
 *
 * This is the durable, self-contained option: it needs no account and no
 * network, and a crawl interrupted by a restart resumes from the same file.
 * Several processes on the same machine may share it (WAL keeps readers from
 * blocking the writer). `path` is created if missing. `run_id` names the crawl,
 * so one file can hold several. Returns NULL with `err` filled on failure.
 */
mi_frontier *mi_frontier_new_sqlite(const char *path, const char *run_id,
                                    char *err, size_t errcap);

/*
 * A frontier shared through a Turso database.
 *
 * The remote option, for a crawl split across hosts. `url` is the database's
 * endpoint (`libsql://…` or `https://…`), `token` its auth token, and `run_id`
 * names this crawl — every worker with the same id shares the frontier. Tables
 * are created if they are missing. Returns NULL if the database cannot be
 * prepared, with `err` filled.
 */
mi_frontier *mi_frontier_new_turso(const char *url, const char *token,
                                   const char *run_id, char *err, size_t errcap);

/* Append a URL at a depth. Returns 0 on success. */
int mi_frontier_push(mi_frontier *f, const char *url, int depth);

/*
 * Take the next URL to visit.
 *
 * Returns 1 and sets `*url` (owned by the caller) and `*depth` when one is
 * available, 0 when the frontier is empty, -1 on error. On a shared backend the
 * claim is atomic, so two workers never get the same URL.
 */
int mi_frontier_take(mi_frontier *f, char **url, int *depth);

/* Whether the frontier is empty (a hint; `take` is authoritative). */
int mi_frontier_empty(mi_frontier *f);

/*
 * Record a URL as visited, atomically across workers.
 *
 * Returns 1 if it was newly added (so the caller should visit it), 0 if it was
 * already there (so the caller should skip it). This is what stops two pages
 * that link the same third from both enqueueing it.
 */
int mi_frontier_mark_seen(mi_frontier *f, const char *url);

/* How many URLs are still to visit (unclaimed). -1 on error. */
long mi_frontier_pending(mi_frontier *f);

/*
 * Return URLs whose claim has gone stale to the queue.
 *
 * A worker that dies mid-page leaves its claim in place, and without this the
 * frontier slowly silts up with URLs no one will ever finish — the failure mode
 * that makes a distributed crawl stop making progress without erroring. A claim
 * older than `lease_seconds` is assumed abandoned and re-queued. Returns the
 * number reclaimed, or -1 on error.
 *
 * Safe to call from any worker, at any time, on any backend; on the in-memory
 * backend there is nothing to reclaim, so it returns 0.
 */
int mi_frontier_reclaim_stale(mi_frontier *f, int lease_seconds);

void mi_frontier_free(mi_frontier *f);

#endif
