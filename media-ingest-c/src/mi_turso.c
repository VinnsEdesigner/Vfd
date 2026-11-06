#include "mi_turso.h"
#include "mi_util.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The HTTP pipeline lives at https://<host>/v2/pipeline, but the URL an operator
 * is given (and copies from the dashboard) is libsql://<host>. Both mean the same
 * host, so the scheme is normalised here rather than made the caller's problem.
 * A URL that already names http(s) is left alone.
 */
static char *http_base(const char *url) {
    if (!url) return NULL;
    if (!strncmp(url, "libsql://", 9)) return mi_asprintf("https://%s", url + 9);
    if (!strncmp(url, "wss://", 6)) return mi_asprintf("https://%s", url + 6);
    if (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8)) return mi_strdup(url);
    /* A bare host: assume https. */
    return mi_asprintf("https://%s", url);
}

typedef struct {
    char *data;
    size_t len;
} resp_buf;

static size_t write_resp(char *ptr, size_t size, size_t nmemb, void *userdata) {
    resp_buf *b = userdata;
    size_t n = size * nmemb;
    char *grown = mi_realloc(b->data, b->len + n + 1);
    if (!grown) return 0;
    b->data = grown;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

/* Append a JSON string literal (quotes and control characters escaped). */
static void json_escape(mi_json_buf *b, const char *s) {
    mi_json_raw(b, "\"");
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
            case '"': mi_json_raw(b, "\\\""); break;
            case '\\': mi_json_raw(b, "\\\\"); break;
            case '\n': mi_json_raw(b, "\\n"); break;
            case '\r': mi_json_raw(b, "\\r"); break;
            case '\t': mi_json_raw(b, "\\t"); break;
            default:
                if (*p < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof esc, "\\u%04x", *p);
                    mi_json_raw(b, esc);
                } else {
                    char ch[2] = {(char)*p, 0};
                    mi_json_raw(b, ch);
                }
        }
    }
    mi_json_raw(b, "\"");
}

/* Build the {"requests":[...]} body for a pipeline of statements. */
static void build_body(mi_json_buf *body, const mi_turso_stmt *stmts, size_t nstmts) {
    mi_json_raw(body, "{\"requests\":[");
    for (size_t i = 0; i < nstmts; i++) {
        if (i) mi_json_raw(body, ",");
        mi_json_raw(body, "{\"type\":\"execute\",\"stmt\":{\"sql\":");
        json_escape(body, stmts[i].sql);
        mi_json_raw(body, ",\"args\":[");
        for (size_t j = 0; j < stmts[i].nargs; j++) {
            if (j) mi_json_raw(body, ",");
            mi_json_raw(body, "{\"type\":");
            json_escape(body, stmts[i].args[j].type);
            mi_json_raw(body, ",\"value\":");
            json_escape(body, stmts[i].args[j].value);
            mi_json_raw(body, "}");
        }
        mi_json_raw(body, "]}}");
    }
    mi_json_raw(body, "]}");
}

/*
 * Send a pipeline and return the parsed document, or NULL with `err` filled.
 * The caller frees the document.
 */
static mi_json *post_pipeline(const char *url, const char *token,
                              const mi_turso_stmt *stmts, size_t nstmts,
                              int *status, char *err, size_t errcap) {
    mi_json_buf body;
    mi_json_init(&body);
    build_body(&body, stmts, nstmts);

    char *base = http_base(url);
    if (!base) { mi_json_free(&body); return NULL; }
    char *endpoint = mi_asprintf("%s/v2/pipeline", base);
    free(base);
    if (!endpoint) { mi_json_free(&body); return NULL; }

    CURL *curl = curl_easy_init();
    if (!curl) { free(endpoint); mi_json_free(&body); return NULL; }
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    char *auth = mi_asprintf("Authorization: Bearer %s", token ? token : "");
    if (auth) headers = curl_slist_append(headers, auth);

    resp_buf rb = {0};
    curl_easy_setopt(curl, CURLOPT_URL, endpoint);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, mi_json_data(&body));
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.len);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_resp);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (status) *status = (int)code;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(auth);
    free(endpoint);
    mi_json_free(&body);

    if (rc != CURLE_OK) {
        if (err && errcap) snprintf(err, errcap, "request failed: %s", curl_easy_strerror(rc));
        free(rb.data);
        return NULL;
    }
    if (code != 200) {
        if (err && errcap) snprintf(err, errcap, "turso returned HTTP %ld", code);
        free(rb.data);
        return NULL;
    }
    mi_json *doc = rb.data ? mi_json_parse(rb.data) : NULL;
    free(rb.data);
    if (!doc && err && errcap) snprintf(err, errcap, "turso returned unparseable JSON");
    return doc;
}

/* Extract results[i].response.result.rows, or NULL. */
static const mi_json *rows_at(const mi_json *doc, size_t i) {
    const mi_json *results = mi_json_get(doc, "results");
    if (!results || results->type != MI_JSON_ARRAY || i >= results->count) return NULL;
    const mi_json *r = results->items[i];
    const mi_json *resp = mi_json_get(r, "response");
    return mi_json_get(resp, "result") ? mi_json_get(mi_json_get(resp, "result"), "rows")
                                       : NULL;
}

/* A libSQL error in results[i] ({"type":"error","error":{...}}). */
static const char *error_at(const mi_json *doc, size_t i) {
    const mi_json *results = mi_json_get(doc, "results");
    if (!results || results->type != MI_JSON_ARRAY || i >= results->count) return NULL;
    const mi_json *jerr = mi_json_get(results->items[i], "error");
    if (!jerr) return NULL;
    return mi_json_get_string(jerr, "message", "turso reported an error");
}

/* Re-serialise the rows of the last statement into an owned JSON array. */
static mi_json *rows_to_owned(const mi_json *rows) {
    if (!rows) return NULL;
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_emit_value(&b, rows);
    mi_json *out = mi_json_parse(mi_json_data(&b));
    mi_json_free(&b);
    return out;
}

mi_json *mi_turso_pipeline(const char *url, const char *token,
                           const mi_turso_stmt *stmts, size_t nstmts,
                           int *status, char *err, size_t errcap) {
    if (status) *status = 0;
    if (err && errcap) err[0] = '\0';
    if (!url || !*url || !stmts || nstmts == 0) {
        if (err && errcap) snprintf(err, errcap, "missing url or statements");
        return NULL;
    }
    mi_json *doc = post_pipeline(url, token, stmts, nstmts, status, err, errcap);
    if (!doc) return NULL;
    /* Any statement erroring fails the call. */
    for (size_t i = 0; i < nstmts; i++) {
        const char *m = error_at(doc, i);
        if (m) {
            if (err && errcap) snprintf(err, errcap, "%s", m);
            mi_json_value_free(doc);
            return NULL;
        }
    }
    mi_json *out = rows_to_owned(rows_at(doc, nstmts - 1));
    mi_json_value_free(doc);
    if (!out && err && errcap) snprintf(err, errcap, "turso returned no rows");
    return out;
}

mi_json *mi_turso_execute(const char *url, const char *token, const char *sql,
                          const mi_turso_arg *args, size_t nargs,
                          int *status, char *err, size_t errcap) {
    mi_turso_stmt stmt = {.sql = sql, .args = args, .nargs = nargs};
    return mi_turso_pipeline(url, token, &stmt, 1, status, err, errcap);
}

size_t mi_turso_row_count(const mi_json *rows) {
    if (!rows || rows->type != MI_JSON_ARRAY) return 0;
    return rows->count;
}

const char *mi_turso_cell_text(const mi_json *rows, size_t row, size_t col) {
    if (!rows || rows->type != MI_JSON_ARRAY || row >= rows->count) return NULL;
    const mi_json *r = rows->items[row];
    if (!r || r->type != MI_JSON_ARRAY || col >= r->count) return NULL;
    /* A libSQL HTTP cell is {"type":"text","value":"..."}; a null column is
     * {"type":"null","value":null}. */
    const mi_json *cell = r->items[col];
    if (!cell || cell->type != MI_JSON_OBJECT) return NULL;
    const char *t = mi_json_get_string(cell, "type", NULL);
    if (!t || !strcmp(t, "null")) return NULL;
    return mi_json_get_string(cell, "value", NULL);
}
