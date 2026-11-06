/* mi_turso.h - a minimal Turso (libSQL) HTTP client.
 *
 * A shared crawl frontier has to live somewhere both a phone and a server can
 * reach, and Turso speaks libSQL over HTTP — no native driver, no daemon. This
 * is the smallest thing that can run a statement and read its rows, written by
 * hand rather than pulled in, so the engine keeps its "libc + libcurl" footprint.
 *
 * The wire protocol is the libSQL HTTP pipeline: POST {url}/v2/pipeline with
 * {"requests":[{"type":"execute","stmt":{"sql":...,"args":[...]}}, ...]}, and
 * the rows come back nested under results[i].response.result.rows.
 *
 * Several statements in one call run on one connection, which is what makes
 * `SELECT changes()` meaningful after an INSERT or UPDATE: a second HTTP call
 * would be a different connection and `changes()` would read 0.
 */
#ifndef MI_TURSO_H
#define MI_TURSO_H

#include <stddef.h>
#include "mi_json.h"

/* A bound parameter. `type` is the libSQL type name: "text", "integer", "null". */
typedef struct {
    const char *type;
    const char *value;
} mi_turso_arg;

/* One statement and its arguments, for a pipeline call. */
typedef struct {
    const char *sql;
    const mi_turso_arg *args;
    size_t nargs;
} mi_turso_stmt;

#define MI_TURSO_TEXT(v) ((mi_turso_arg){"text", (v)})
#define MI_TURSO_INT(v) ((mi_turso_arg){"integer", (v)})

/*
 * Run one statement and return its `rows` as a JSON array (owned by the caller),
 * or NULL on failure with `err` filled and `status` set to the HTTP status.
 *
 * `rows` is an array of arrays: each row is the list of column values, each of
 * which is an object like {"type":"text","value":"..."}. Use the helpers below.
 */
mi_json *mi_turso_execute(const char *url, const char *token, const char *sql,
                          const mi_turso_arg *args, size_t nargs,
                          int *status, char *err, size_t errcap);

/*
 * Run several statements in one call, on one connection, and return the rows of
 * the *last* one. This is what lets a claim be atomic: an UPDATE followed by a
 * SELECT in the same call reads what the UPDATE did.
 */
mi_json *mi_turso_pipeline(const char *url, const char *token,
                           const mi_turso_stmt *stmts, size_t nstmts,
                           int *status, char *err, size_t errcap);

/*
 * Read cell (row, col) as a string, or NULL.
 *
 * An integer cell is returned as its decimal text, so a caller does not have to
 * care which type the column declared.
 */
const char *mi_turso_cell_text(const mi_json *rows, size_t row, size_t col);

/* Number of rows, or 0. */
size_t mi_turso_row_count(const mi_json *rows);

#endif
