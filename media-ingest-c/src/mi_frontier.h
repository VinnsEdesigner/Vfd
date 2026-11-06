/* mi_frontier.h - the set of URLs to visit, and the set already visited.
 *
 * A crawl is a breadth-first walk over this. Keeping it behind an interface
 * lets the same walk run either in one process (an in-memory queue) or shared
 * across several (a Turso table), without the walk knowing which.
 *
 * The distributed backend is the point: two crawlers pointed at the same
 * `run_id` share one frontier, so a large site is walked once rather than once
 * per process. Claiming the next URL is a single atomic statement, so two
 * workers never take the same page.
 */
#ifndef MI_FRONTIER_H
#define MI_FRONTIER_H

#include <stddef.h>

typedef struct mi_frontier mi_frontier;

/* An in-process queue. The backend a single-process crawl uses. */
mi_frontier *mi_frontier_new_memory(void);

/*
 * A frontier shared through a Turso database.
 *
 * `url` is the database's https endpoint, `token` its auth token, and `run_id`
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
 * available, 0 when the frontier is empty, -1 on error. On the distributed
 * backend the claim is atomic, so two workers never get the same URL.
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

void mi_frontier_free(mi_frontier *f);

#endif
