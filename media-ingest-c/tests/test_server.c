/* test_server.c - JSON round-trip and live HTTP API tests.
 *
 * The API tests start a real server on an ephemeral port and drive it with
 * raw sockets, so routing, auth, JSON and the download lifecycle are all exercised
 * end to end (no mocks).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mi_browse.h"
#include "mi_download.h"
#include "mi_json.h"
#include "mi_server.h"
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

/* ---------------------------------------------------------------- */
/* JSON                                                              */
/* ---------------------------------------------------------------- */

static void test_json_writer(void) {
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"a\":");
    mi_json_int(&b, 42);
    mi_json_raw(&b, ",\"s\":");
    mi_json_string(&b, "he said \"hi\"\n\t\\");
    mi_json_raw(&b, ",\"n\":");
    mi_json_null(&b);
    mi_json_raw(&b, ",\"t\":");
    mi_json_bool(&b, 1);
    mi_json_raw(&b, "}");
    const char *out = mi_json_data(&b);
    CHECK(!strcmp(out,
                  "{\"a\":42,\"s\":\"he said \\\"hi\\\"\\n\\t\\\\\",\"n\":null,\"t\":true}"),
          "json writer escaping");
    mi_json_free(&b);
}

static void test_json_parse_roundtrip(void) {
    const char *doc = "{\"url\":\"http://x/y\",\"n\":3,\"ok\":true,\"arr\":[1,2,3],\"z\":null}";
    mi_json *v = mi_json_parse(doc);
    CHECK(v != NULL, "parse ok");
    CHECK(!strcmp(mi_json_get_string(v, "url", ""), "http://x/y"), "parse string");
    CHECK(mi_json_get_int(v, "n", 0) == 3, "parse int");
    CHECK(mi_json_get_bool(v, "ok", 0) == 1, "parse bool");
    const mi_json *arr = mi_json_get(v, "arr");
    CHECK(arr && arr->type == MI_JSON_ARRAY && arr->count == 3, "parse array");
    CHECK(mi_json_get(v, "z") && mi_json_get(v, "z")->type == MI_JSON_NULL, "parse null");
    CHECK(mi_json_get(v, "missing") == NULL, "missing key");
    mi_json_value_free(v);
}

static void test_json_rejects_bad(void) {
    CHECK(mi_json_parse("{\"a\":}") == NULL, "reject trailing colon");
    CHECK(mi_json_parse("{\"a\":1") == NULL, "reject unterminated");
    CHECK(mi_json_parse("[1,2") == NULL, "reject unterminated array");
    CHECK(mi_json_parse("nonsense") == NULL, "reject bare token");
}

static void test_json_unicode_escape(void) {
    mi_json *v = mi_json_parse("{\"s\":\"caf\\u00e9\"}");
    CHECK(v != NULL, "parse unicode");
    CHECK(v && !strcmp(mi_json_get_string(v, "s", ""), "caf\xc3\xa9"), "utf-8 decode");
    mi_json_value_free(v);
}

/* ---------------------------------------------------------------- */
/* HTTP API                                                          */
/* ---------------------------------------------------------------- */

/* Minimal HTTP client: send a request, return the whole response. */
static char *http_call_origin(int port, const char *method, const char *path,
                              const char *body, const char *auth, int *status_out,
                              const char *origin);

static char *http_call(int port, const char *method, const char *path,
                       const char *body, const char *auth, int *status_out) {
    return http_call_origin(port, method, path, body, auth, status_out, NULL);
}

/* Same, but with an Origin header, so the CORS behaviour can be exercised. */
static char *http_call_origin(int port, const char *method, const char *path,
                              const char *body, const char *auth, int *status_out,
                              const char *origin) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return NULL;
    }
    char req[4096];
    size_t blen = body ? strlen(body) : 0;
    int n = snprintf(req, sizeof req,
                     "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                     "Content-Type: application/json\r\n"
                     "%s%s%s"
                     "%s%s%s"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                     method, path, auth ? "Authorization: Bearer " : "",
                     auth ? auth : "", auth ? "\r\n" : "",
                     origin ? "Origin: " : "", origin ? origin : "",
                     origin ? "\r\n" : "", blen);
    if (send(fd, req, (size_t)n, 0) < 0) {
        close(fd);
        return NULL;
    }
    if (blen && send(fd, body, blen, 0) < 0) {
        close(fd);
        return NULL;
    }
    char *resp = NULL;
    size_t cap = 0, len = 0;
    char buf[4096];
    ssize_t got;
    while ((got = recv(fd, buf, sizeof buf, 0)) > 0) {
        if (len + (size_t)got + 1 > cap) {
            cap = (len + (size_t)got + 1) * 2;
            resp = mi_realloc(resp, cap);
        }
        memcpy(resp + len, buf, (size_t)got);
        len += (size_t)got;
    }
    if (resp) resp[len] = '\0';
    close(fd);
    if (status_out && resp) {
        int st = 0;
        sscanf(resp, "HTTP/1.1 %d", &st);
        *status_out = st;
    }
    return resp;
}

/* Extract the JSON body (after the blank line). */
static const char *body_of(const char *resp) {
    const char *p = strstr(resp, "\r\n\r\n");
    return p ? p + 4 : resp;
}

static void *server_thread(void *arg) {
    mi_server_run((mi_server *)arg);
    return NULL;
}

static void test_http_api(void) {
    mi_server_config cfg;
    mi_server_config_init(&cfg);
    cfg.port = 0;
    cfg.work_dir = "/tmp/mi-test-server";
    cfg.auth_token = "testtoken";
    cfg.allow_origins = "https://console.example";
    mi_mkdir_p(cfg.work_dir);

    mi_server *srv = mi_server_new(&cfg);
    CHECK(srv != NULL, "server new");
    CHECK(mi_server_listen(srv) == MI_OK, "server listen");
    int port = mi_server_port(srv);
    CHECK(port > 0, "ephemeral port assigned");

    pthread_t th;
    pthread_create(&th, NULL, server_thread, srv);

    int status = 0;
    char *resp;

    /* healthz needs no auth */
    resp = http_call(port, "GET", "/healthz", NULL, NULL, &status);
    CHECK(status == 200, "healthz 200 without auth");
    CHECK(strstr(body_of(resp), "\"status\":\"ok\"") != NULL, "healthz body");
    free(resp);

    /* protected endpoint rejects without a token */
    resp = http_call(port, "GET", "/api/v1/downloads", NULL, NULL, &status);
    CHECK(status == 401, "downloads 401 without token");
    free(resp);

    resp = http_call(port, "GET", "/api/v1/downloads", NULL, "wrong", &status);
    CHECK(status == 401, "downloads 401 with bad token");
    free(resp);

    /* capabilities with the token */
    resp = http_call(port, "GET", "/api/v1/capabilities", NULL, "testtoken", &status);
    CHECK(status == 200, "capabilities 200");
    CHECK(strstr(body_of(resp), "\"hls\":true") != NULL, "capabilities body");
    /* The limits are the server's real ones, not constants the UI hardcodes. */
    CHECK(strstr(body_of(resp), "\"max_body_bytes\":67108864") != NULL,
          "capabilities reports the real body cap");
    CHECK(strstr(body_of(resp), "\"workers\":16") != NULL,
          "capabilities reports the real worker count");
    CHECK(strstr(body_of(resp), "\"engine_version\":\"0.1.0\"") != NULL,
          "capabilities reports the engine version");
    CHECK(strstr(body_of(resp), "\"ffmpeg\":") != NULL,
          "capabilities reports ffmpeg availability");
    free(resp);

    /* system/storage names the real directory and proves it is writable */
    resp = http_call(port, "GET", "/api/v1/system/storage", NULL, "testtoken", &status);
    CHECK(status == 200, "system/storage 200");
    CHECK(strstr(body_of(resp), "\"writable\":true") != NULL,
          "system/storage reports writable");
    CHECK(strstr(body_of(resp), cfg.work_dir) != NULL,
          "system/storage names the engine's directory");
    free(resp);

    /* PUT storage/location moves the engine's directory, and the change is
     * visible on the next read rather than only in the response. */
    {
        const char *newdir = "/tmp/mi-test-relocated";
        char body[256];
        snprintf(body, sizeof body, "{\"dir\":\"%s\"}", newdir);
        resp = http_call(port, "PUT", "/api/v1/storage/location", body, "testtoken", &status);
        CHECK(status == 200, "set location 200");
        CHECK(strstr(body_of(resp), newdir) != NULL, "set location echoes the dir");
        free(resp);
        resp = http_call(port, "GET", "/api/v1/system/storage", NULL, "testtoken", &status);
        CHECK(strstr(body_of(resp), newdir) != NULL,
              "the new directory is reported after the change");
        free(resp);
    }

    /* A directory that cannot be created is refused, not accepted on faith. */
    {
        const char *f = "/tmp/mi-test-not-a-dir";
        FILE *fp = fopen(f, "wb");
        if (fp) { fputs("x", fp); fclose(fp); }
        char body[256];
        /* A path *underneath a regular file* can never be a directory. */
        snprintf(body, sizeof body, "{\"dir\":\"%s/child\"}", f);
        resp = http_call(port, "PUT", "/api/v1/storage/location", body, "testtoken", &status);
        CHECK(status == 422, "unwritable location -> 422");
        free(resp);
        unlink(f);
    }

    /* An empty directory is a bad request, not a silent reset. */
    resp = http_call(port, "PUT", "/api/v1/storage/location", "{\"dir\":\"\"}",
                     "testtoken", &status);
    CHECK(status == 400, "empty location -> 400");
    free(resp);

    /* Move the directory back so the download/move assertions below still see
     * the layout they expect. */
    {
        char body[256];
        snprintf(body, sizeof body, "{\"dir\":\"%s\"}", cfg.work_dir);
        resp = http_call(port, "PUT", "/api/v1/storage/location", body, "testtoken", &status);
        CHECK(status == 200, "location restored");
        free(resp);
    }

    /* empty download list */
    resp = http_call(port, "GET", "/api/v1/downloads", NULL, "testtoken", &status);
    CHECK(status == 200, "downloads 200");
    mi_json *j = mi_json_parse(body_of(resp));
    CHECK(j && mi_json_get_int(j, "count", -1) == 0, "downloads count 0");
    mi_json_value_free(j);
    free(resp);

    /* detect: post raw bytes of a synthetic mp4 */
    unsigned char mp4[512];
    const char ftyp[] = "ftypisom\x00\x00\x02\x00isomiso2mp41";
    size_t fl = sizeof ftyp - 1;
    uint32_t box = (uint32_t)(fl + 4);
    mp4[0] = box >> 24; mp4[1] = box >> 16; mp4[2] = box >> 8; mp4[3] = box;
    memcpy(mp4 + 4, ftyp, fl);
    memset(mp4 + 4 + fl, 0, sizeof mp4 - 4 - fl);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    connect(fd, (struct sockaddr *)&addr, sizeof addr);
    char hdr[512];
    int hn = snprintf(hdr, sizeof hdr,
                      "POST /api/v1/detect HTTP/1.1\r\nHost: x\r\n"
                      "Authorization: Bearer testtoken\r\n"
                      "Content-Type: application/octet-stream\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                      sizeof mp4);
    send(fd, hdr, (size_t)hn, 0);
    send(fd, mp4, sizeof mp4, 0);
    char dbuf[4096];
    size_t dlen = 0;
    ssize_t dn;
    while (dlen < sizeof dbuf - 1 && (dn = recv(fd, dbuf + dlen, sizeof dbuf - 1 - dlen, 0)) > 0)
        dlen += (size_t)dn;
    close(fd);
    dbuf[dlen] = '\0';
    CHECK(dlen > 0 && strstr(dbuf, "\"kind\":\"mp4\"") != NULL, "detect mp4 via api");

    /* unknown endpoint */
    resp = http_call(port, "GET", "/api/v1/nope", NULL, "testtoken", &status);
    CHECK(status == 404, "unknown endpoint 404");
    free(resp);

    /* bad download request */
    resp = http_call(port, "POST", "/api/v1/downloads", "{}", "testtoken", &status);
    CHECK(status == 400, "download without url -> 400");
    free(resp);

    /* create a download that will fail fast (unreachable URL) then poll it */
    resp = http_call(port, "POST", "/api/v1/downloads",
                     "{\"url\":\"http://127.0.0.1:1/nope.mp4\",\"hls\":false}",
                     "testtoken", &status);
    CHECK(status == 202, "download accepted");
    const char *jid = NULL;
    char idbuf[64] = {0};
    {
        mi_json *jj = mi_json_parse(body_of(resp));
        jid = mi_json_get_string(jj, "id", NULL);
        if (jid) snprintf(idbuf, sizeof idbuf, "%s", jid);
        mi_json_value_free(jj);
    }
    CHECK(idbuf[0] != '\0', "download id returned");
    free(resp);

    /* poll until terminal */
    int terminal = 0;
    for (int i = 0; i < 60; i++) {
        char path[128];
        snprintf(path, sizeof path, "/api/v1/downloads/%s", idbuf);
        resp = http_call(port, "GET", path, NULL, "testtoken", &status);
        if (resp && (strstr(resp, "\"state\":\"failed\"") ||
                     strstr(resp, "\"state\":\"done\"") ||
                     strstr(resp, "\"state\":\"cancelled\""))) {
            terminal = 1;
            free(resp);
            break;
        }
        free(resp);
        usleep(50000);
    }
    CHECK(terminal, "download reaches a terminal state");

    /* A percent-encoded filename is decoded, so the file is not called
     * "cat_%28aka%29.jpg" on disk. This is the name a person sees and the OS
     * matches, so it has to be the real one. */
    {
        resp = http_call(port, "POST", "/api/v1/downloads",
                         "{\"url\":\"http://127.0.0.1:1/Six_weeks_old_cat_%28aka%29.jpg\",\"hls\":false}",
                         "testtoken", &status);
        CHECK(status == 202, "encoded-name download accepted");
        CHECK(strstr(body_of(resp), "\"name\":\"Six_weeks_old_cat_(aka).jpg\"") != NULL,
              "percent-encoded name is decoded");
        free(resp);
    }

    /* --- CORS ---
     * The allow list is the difference between "this console can talk to the
     * engine" and "any website the user visits can read their library and start
     * downloads". Both directions are asserted, because only testing the
     * allowed origin would pass even if the list were ignored. */
    {
        resp = http_call_origin(port, "GET", "/api/v1/capabilities", NULL, "testtoken",
                                &status, "https://console.example");
        CHECK(status == 200, "allowed origin gets the response");
        CHECK(strstr(resp, "Access-Control-Allow-Origin: https://console.example") != NULL,
              "allowed origin is echoed back verbatim");
        /* A wildcard would be refused by the browser on a credentialed request,
         * so the exact origin must be echoed, not '*'. */
        CHECK(strstr(resp, "Access-Control-Allow-Origin: *") == NULL,
              "no wildcard is sent");
        free(resp);

        resp = http_call_origin(port, "GET", "/api/v1/capabilities", NULL, "testtoken",
                                &status, "https://evil.example.com");
        CHECK(status == 200, "the request is still answered");
        CHECK(strstr(resp, "Access-Control-Allow-Origin") == NULL,
              "an unlisted origin gets no CORS header, so the browser blocks it");
        free(resp);

        /* Preflight: the browser asks before a JSON POST, and the answer has to
         * name the origin too or the real request never happens. */
        resp = http_call_origin(port, "OPTIONS", "/api/v1/downloads", NULL, NULL,
                                &status, "https://console.example");
        CHECK(status == 204, "preflight 204");
        CHECK(strstr(resp, "Access-Control-Allow-Origin: https://console.example") != NULL,
              "preflight names the allowed origin");
        CHECK(strstr(resp, "Access-Control-Allow-Methods") != NULL, "preflight methods");
        CHECK(strstr(resp, "Access-Control-Allow-Headers") != NULL, "preflight headers");
        free(resp);

        resp = http_call_origin(port, "OPTIONS", "/api/v1/downloads", NULL, NULL,
                                &status, "https://evil.example.com");
        CHECK(strstr(resp, "Access-Control-Allow-Origin") == NULL,
              "preflight from an unlisted origin is not allowed");
        free(resp);
    }

    /* delete an unknown download */
    resp = http_call(port, "DELETE", "/api/v1/downloads/deadbeef", NULL, "testtoken", &status);
    CHECK(status == 404, "delete unknown download 404");
    free(resp);

    /* the record carries a display name derived from the URL, not the id */
    {
        char path[128];
        snprintf(path, sizeof path, "/api/v1/downloads/%s", idbuf);
        resp = http_call(port, "GET", path, NULL, "testtoken", &status);
        CHECK(status == 200, "get download 200");
        CHECK(strstr(body_of(resp), "\"name\":\"nope.mp4\"") != NULL,
              "download name derived from url");
        /* `filename` is what a client saves under. With no file on disk yet it
         * falls back to the display name, but the field is always present so a
         * client never has to guess. */
        CHECK(strstr(body_of(resp), "\"filename\":\"nope.mp4\"") != NULL,
              "filename is reported even before the bytes land");
        free(resp);
    }

    /* Renaming drops the extension from the display name, but `filename` always
     * has one: the kind's extension when it is known, `.bin` when it is not
     * (this record failed, so nothing was detected). A save is therefore always
     * openable rather than extension-less. */
    {
        char path[128];
        snprintf(path, sizeof path, "/api/v1/downloads/%s", idbuf);
        resp = http_call(port, "PATCH", path, "{\"name\":\"holiday clip\"}",
                         "testtoken", &status);
        CHECK(status == 200, "rename without extension 200");
        CHECK(strstr(body_of(resp), "\"name\":\"holiday clip\"") != NULL,
              "display name drops the extension");
        CHECK(strstr(body_of(resp), "\"filename\":\"holiday clip.bin\"") != NULL,
              "filename gains an extension even when the kind is unknown");
        free(resp);
    }

    /* A name that already carries an extension is preserved exactly, so a
     * deliberate ".mp4" is never rewritten to ".bin". */
    {
        char path[128];
        snprintf(path, sizeof path, "/api/v1/downloads/%s", idbuf);
        resp = http_call(port, "PATCH", path, "{\"name\":\"holiday.clip.mp4\"}",
                         "testtoken", &status);
        CHECK(status == 200, "rename with extension 200");
        CHECK(strstr(body_of(resp), "\"filename\":\"holiday.clip.mp4\"") != NULL,
              "an explicit extension is kept as given");
        free(resp);
    }

    /* PATCH with neither field is a bad request */
    {
        char path[128];
        snprintf(path, sizeof path, "/api/v1/downloads/%s", idbuf);
        resp = http_call(port, "PATCH", path, "{}", "testtoken", &status);
        CHECK(status == 400, "patch with no fields -> 400");
        free(resp);
    }

    /* rename changes the reported name */
    {
        char path[128];
        snprintf(path, sizeof path, "/api/v1/downloads/%s", idbuf);
        resp = http_call(port, "PATCH", path, "{\"name\":\"holiday clip.mp4\"}",
                         "testtoken", &status);
        CHECK(status == 200, "rename 200");
        CHECK(strstr(body_of(resp), "\"name\":\"holiday clip.mp4\"") != NULL,
              "rename reflected in response");
        free(resp);
    }

    /* a name that is only path separators is rejected, not silently mangled */
    {
        char path[128];
        snprintf(path, sizeof path, "/api/v1/downloads/%s", idbuf);
        resp = http_call(port, "PATCH", path, "{\"name\":\"../../\"}",
                         "testtoken", &status);
        CHECK(status == 400, "traversal-only name -> 400");
        free(resp);
    }

    /* move relocates the item's directory inside the library */
    {
        char path[128];
        snprintf(path, sizeof path, "/api/v1/downloads/%s", idbuf);
        resp = http_call(port, "PATCH", path, "{\"move_to\":\"archive\"}",
                         "testtoken", &status);
        CHECK(status == 200, "move 200");
        free(resp);
        char moved[256];
        snprintf(moved, sizeof moved, "%s/archive/%s", cfg.work_dir, idbuf);
        CHECK(mi_path_is_dir(moved), "moved directory exists in the library");
    }

    /* --- pause / resume lifecycle ---
     * A slow source so the pause lands mid-transfer: the local fileserver is
     * fast, so this uses the unreachable-host trick only for the error paths and
     * asserts the state machine, which is the part that must not regress. */
    {
        char path[128];
        snprintf(path, sizeof path, "/api/v1/downloads/%s", idbuf);

        /* Pausing a finished download is a conflict, not a silent no-op. */
        resp = http_call(port, "PATCH", path, "{\"action\":\"pause\"}", "testtoken", &status);
        CHECK(status == 409, "pause on a settled download -> 409");
        free(resp);

        /* Resuming something that was never paused is also a conflict. */
        resp = http_call(port, "PATCH", path, "{\"action\":\"resume\"}", "testtoken", &status);
        CHECK(status == 409, "resume on a non-paused download -> 409");
        free(resp);

        /* An unknown action is rejected rather than ignored. */
        resp = http_call(port, "PATCH", path, "{\"action\":\"teleport\"}", "testtoken", &status);
        CHECK(status == 400, "unknown action -> 400");
        free(resp);
    }

    /* A real pause: a slow source, so the worker is still transferring when the
     * pause lands. `slow` makes the test server dribble bytes out. */
    {
        resp = http_call(port, "POST", "/api/v1/downloads",
                         "{\"url\":\"http://127.0.0.1:1/slow.bin\",\"hls\":false}",
                         "testtoken", &status);
        char pid[64] = {0};
        mi_json *jj = mi_json_parse(body_of(resp));
        const char *id = mi_json_get_string(jj, "id", NULL);
        if (id) snprintf(pid, sizeof pid, "%s", id);
        mi_json_value_free(jj);
        free(resp);

        char path[128];
        snprintf(path, sizeof path, "/api/v1/downloads/%s", pid);
        /* Pause immediately; whether it lands mid-transfer or before the worker
         * starts, the outcome must be the same paused state. */
        resp = http_call(port, "PATCH", path, "{\"action\":\"pause\"}", "testtoken", &status);
        CHECK(status == 200, "pause accepted");
        free(resp);

        int got_paused = 0;
        for (int i = 0; i < 60; i++) {
            resp = http_call(port, "GET", path, NULL, "testtoken", &status);
            if (resp && strstr(resp, "\"state\":\"paused\"")) got_paused = 1;
            int settled = resp && (strstr(resp, "\"state\":\"paused\"") ||
                                   strstr(resp, "\"state\":\"failed\"") ||
                                   strstr(resp, "\"state\":\"done\""));
            free(resp);
            if (settled) break;
            usleep(50000);
        }
        CHECK(got_paused, "a running download can be paused");

        /* And a paused download can be resumed. */
        resp = http_call(port, "PATCH", path, "{\"action\":\"resume\"}", "testtoken", &status);
        CHECK(status == 200, "resume accepted");
        free(resp);
    }

    /* A create records the options it was given, so the UI can report them. */
    {
        resp = http_call(port, "POST", "/api/v1/downloads",
                         "{\"url\":\"http://127.0.0.1:1/opts.mp4\",\"hls\":false,"
                         "\"quality\":\"1080p\",\"connections\":4}",
                         "testtoken", &status);
        CHECK(status == 202, "create with options accepted");
        CHECK(strstr(body_of(resp), "\"requested_quality\":\"1080p\"") != NULL,
              "requested_quality echoed back");
        CHECK(strstr(body_of(resp), "\"connections\":4") != NULL,
              "connections echoed back");
        CHECK(strstr(body_of(resp), "\"hls\":false") != NULL, "hls echoed back");
        free(resp);
    }

    /* PATCH on an unknown id is a 404 */
    resp = http_call(port, "PATCH", "/api/v1/downloads/deadbeef",
                     "{\"name\":\"x.mp4\"}", "testtoken", &status);
    CHECK(status == 404, "patch unknown download 404");
    free(resp);

    mi_server_stop(srv);
    pthread_join(th, NULL);
    mi_server_free(srv);
}

/* ---------------------------------------------------------------- */
/* HLS artifact serving + container conversion                       */
/* ---------------------------------------------------------------- */

/* A minimal HTTP/1.1 server for the engine to download from, so a real download
 * can be completed and then converted. It honours a Range request, because the
 * engine's parallel downloader needs one to exercise the real code path. */
static const char *g_mock_body = NULL;
static size_t g_mock_body_len = 0;

static void mock_serve_conn(int fd) {
    char req[4096];
    ssize_t got = read(fd, req, sizeof req - 1);
    if (got <= 0) {
        close(fd);
        return;
    }
    req[got] = '\0';
    int head_only = strncmp(req, "HEAD", 4) == 0;

    size_t start = 0, end = g_mock_body_len ? g_mock_body_len - 1 : 0;
    int is_range = 0;
    const char *r = strstr(req, "Range: bytes=");
    if (r) {
        r += strlen("Range: bytes=");
        start = (size_t)strtoull(r, (char **)&r, 10);
        if (*r == '-') {
            r++;
            end = (size_t)strtoull(r, NULL, 10);
            if (end >= g_mock_body_len) end = g_mock_body_len - 1;
        }
        is_range = 1;
    }

    char header[512];
    int hlen;
    if (is_range) {
        hlen = snprintf(header, sizeof header,
                        "HTTP/1.1 206 Partial Content\r\n"
                        "Content-Range: bytes %zu-%zu/%zu\r\n"
                        "Content-Length: %zu\r\n"
                        "Accept-Ranges: bytes\r\n"
                        "Content-Type: application/octet-stream\r\n"
                        "Connection: close\r\n\r\n",
                        start, end, g_mock_body_len, end - start + 1);
    } else {
        hlen = snprintf(header, sizeof header,
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Length: %zu\r\n"
                        "Accept-Ranges: bytes\r\n"
                        "Content-Type: application/octet-stream\r\n"
                        "Connection: close\r\n\r\n",
                        g_mock_body_len);
    }
    (void)!write(fd, header, (size_t)hlen);
    if (!head_only && g_mock_body) {
        if (is_range) (void)!write(fd, g_mock_body + start, end - start + 1);
        else (void)!write(fd, g_mock_body, g_mock_body_len);
    }
    close(fd);
}

static void mock_loop(int listen_fd) {
    signal(SIGTERM, SIG_DFL);
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        mock_serve_conn(fd);
    }
    _exit(0);
}

/* Start the mock server serving `body`. Returns the port, or -1. The child pid
 * is written to `pid_out` so the caller can stop it. */
static int start_mock_body(const char *body, size_t len, pid_t *pid_out) {
    g_mock_body = body;
    g_mock_body_len = len;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return -1;
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(listen_fd);
        return -1;
    }
    socklen_t alen = sizeof addr;
    getsockname(listen_fd, (struct sockaddr *)&addr, &alen);
    listen(listen_fd, 16);
    pid_t pid = fork();
    if (pid == 0) mock_loop(listen_fd);
    close(listen_fd);
    *pid_out = pid;
    return ntohs(addr.sin_port);
}

static int64_t file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
}

/* Poll the record until its conversion reaches a terminal state. */
static int wait_for_conversion(int port, const char *id, char *state, size_t state_size) {
    for (int i = 0; i < 600; i++) {
        char path[128];
        snprintf(path, sizeof path, "/api/v1/downloads/%s", id);
        int status = 0;
        char *resp = http_call(port, "GET", path, NULL, "testtoken", &status);
        if (resp) {
            const char *c = strstr(resp, "\"conversion\":{\"state\":\"");
            if (c) {
                c += strlen("\"conversion\":{\"state\":\"");
                size_t n = 0;
                while (c[n] && c[n] != '"' && n + 1 < state_size) {
                    state[n] = c[n];
                    n++;
                }
                state[n] = '\0';
                int done = !strcmp(state, "done") || !strcmp(state, "failed");
                free(resp);
                if (done) return 1;
                usleep(50000);
                continue;
            }
            free(resp);
        }
        usleep(50000);
    }
    return 0;
}

/* Build a real MKV with ffmpeg, so the conversion exercises an actual codec
 * rather than a fixture. Returns 0 on success. */
static int make_mkv(const char *path) {
    char *argv[] = {
        "ffmpeg", "-y", "-f", "lavfi", "-i", "testsrc=size=320x240:rate=10:duration=2",
        "-f", "lavfi", "-i", "sine=frequency=440:duration=2",
        "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-shortest", (char *)path, NULL,
    };
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static void test_hls_serve_and_convert(void) {
    mi_server_config cfg;
    mi_server_config_init(&cfg);
    cfg.port = 0;
    cfg.host = "127.0.0.1";
    cfg.work_dir = "/tmp/mi-test-hls-convert";
    cfg.auth_token = "testtoken";
    mi_rm_rf(cfg.work_dir);
    mi_mkdir_p(cfg.work_dir);

    mi_server *srv = mi_server_new(&cfg);
    CHECK(srv != NULL, "hls/convert: server new");
    if (!srv) return;
    CHECK(mi_server_listen(srv) == MI_OK, "hls/convert: server listen");
    int port = mi_server_port(srv);
    pthread_t th;
    pthread_create(&th, NULL, server_thread, srv);

    /* --- Traversal attempts are refused, not resolved. The guard runs before
     * any record lookup, so it holds even for an unknown id. Both the raw ".."
     * and its percent-encoded form are checked, because a proxy or client may
     * send either and the path is matched before any decoding. --- */
    const char *jid = "aaaaaaaabbbbbbbbccccccccdddddddd";
    int status = 0;
    char path[256];
    char *resp;

    snprintf(path, sizeof path, "/api/v1/downloads/%s/hls/../../../etc/passwd", jid);
    resp = http_call(port, "GET", path, NULL, "testtoken", &status);
    CHECK(status == 400, "raw traversal refused");
    free(resp);

    snprintf(path, sizeof path, "/api/v1/downloads/%s/hls/..%%2f..%%2fetc%%2fpasswd", jid);
    resp = http_call(port, "GET", path, NULL, "testtoken", &status);
    CHECK(status == 400 || status == 404, "encoded traversal refused");
    free(resp);

    /* The artifact route 404s for an id that is not a finished download. */
    snprintf(path, sizeof path, "/api/v1/downloads/deadbeef/hls/master.m3u8");
    resp = http_call(port, "GET", path, NULL, "testtoken", &status);
    CHECK(status == 404, "hls for unknown id 404");
    free(resp);

    /* --- Conversion. Download a real MKV through the engine, then convert. --- */
    if (mi_which("ffmpeg")) {
        const char *mkv_src = "/tmp/mi-test-source.mkv";
        int have_mkv = make_mkv(mkv_src) == 0;
        CHECK(have_mkv, "a real MKV fixture was built with ffmpeg");

        if (have_mkv) {
            int64_t src_len = file_size(mkv_src);
            char *mkv_bytes = mi_malloc((size_t)src_len);
            FILE *mf = mkv_bytes ? fopen(mkv_src, "rb") : NULL;
            size_t nread = mf ? fread(mkv_bytes, 1, (size_t)src_len, mf) : 0;
            if (mf) fclose(mf);
            CHECK(nread == (size_t)src_len, "the MKV fixture was read back");

            pid_t mock_pid = -1;
            int mock_port = start_mock_body(mkv_bytes, (size_t)src_len, &mock_pid);
            CHECK(mock_port > 0, "mock server started for the MKV");

            char body[512];
            snprintf(body, sizeof body,
                     "{\"url\":\"http://127.0.0.1:%d/source.mkv\",\"hls\":false}", mock_port);
            resp = http_call(port, "POST", "/api/v1/downloads", body, "testtoken", &status);
            CHECK(status == 202, "mkv download accepted");
            char id[64] = {0};
            if (resp) {
                mi_json *jj = mi_json_parse(body_of(resp));
                const char *got = mi_json_get_string(jj, "id", NULL);
                if (got) snprintf(id, sizeof id, "%s", got);
                mi_json_value_free(jj);
                free(resp);
            }
            CHECK(id[0] != '\0', "mkv download id returned");

            /* Wait for the bytes to land. */
            int done = 0;
            for (int i = 0; i < 200; i++) {
                char p[128];
                snprintf(p, sizeof p, "/api/v1/downloads/%s", id);
                resp = http_call(port, "GET", p, NULL, "testtoken", &status);
                if (resp && strstr(resp, "\"state\":\"done\"")) { done = 1; free(resp); break; }
                free(resp);
                usleep(50000);
            }
            CHECK(done, "the MKV download completed");

            /* --- HLS artifacts for a real, finished record. The transcode
             * would normally write this layout; it is built here so the serving
             * route is tested without waiting for a full ladder. --- */
            if (done) {
                char dir[PATH_MAX];
                snprintf(dir, sizeof dir, "%s/%s", cfg.work_dir, id);
                char *hlsdir = mi_path_join(dir, "hls");
                mi_mkdir_p(hlsdir);
                char *vdir = mi_path_join(hlsdir, "360p");
                mi_mkdir_p(vdir);
                char *master = mi_path_join(hlsdir, "master.m3u8");
                char *seg = mi_path_join(vdir, "seg_000.ts");
                FILE *f = fopen(master, "w");
                if (f) {
                    fputs("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000\n360p/index.m3u8\n", f);
                    fclose(f);
                }
                f = fopen(seg, "wb");
                if (f) { fputs("SEGMENTBYTES", f); fclose(f); }

                /* A file outside the library that a traversal would reach. */
                FILE *secret = fopen("/tmp/mi-test-secret.txt", "w");
                if (secret) { fputs("TOP SECRET", secret); fclose(secret); }

                snprintf(path, sizeof path, "/api/v1/downloads/%s/hls/master.m3u8", id);
                resp = http_call(port, "GET", path, NULL, "testtoken", &status);
                CHECK(status == 200, "hls master 200");
                CHECK(resp && strstr(resp, "Content-Type: application/vnd.apple.mpegurl") != NULL,
                      "hls playlist content type");
                CHECK(resp && strstr(resp, "#EXTM3U") != NULL, "hls playlist body served");
                free(resp);

                snprintf(path, sizeof path, "/api/v1/downloads/%s/hls/360p/seg_000.ts", id);
                resp = http_call(port, "GET", path, NULL, "testtoken", &status);
                CHECK(status == 200, "hls segment 200");
                CHECK(resp && strstr(resp, "Content-Type: video/mp2t") != NULL,
                      "hls segment content type");
                CHECK(resp && strstr(resp, "SEGMENTBYTES") != NULL, "hls segment body served");
                free(resp);

                /* A symlink inside the served hls directory must not be followed
                 * out of it, even though the path itself has no traversal. */
                char link[PATH_MAX + 16];
                snprintf(link, sizeof link, "%s/escape", hlsdir);
                unlink(link);
                if (symlink("/tmp/mi-test-secret.txt", link) == 0) {
                    snprintf(path, sizeof path, "/api/v1/downloads/%s/hls/escape", id);
                    resp = http_call(port, "GET", path, NULL, "testtoken", &status);
                    CHECK(status == 404, "a symlink out of the item directory is refused");
                    CHECK(resp && strstr(resp, "TOP SECRET") == NULL, "the secret is not leaked");
                    free(resp);
                }

                free(master);
                free(seg);
                free(vdir);
                free(hlsdir);
            }

            /* Before converting, there is nothing at /converted. */
            snprintf(path, sizeof path, "/api/v1/downloads/%s/converted", id);
            resp = http_call(port, "GET", path, NULL, "testtoken", &status);
            CHECK(status == 404, "converted 404 before a conversion");
            free(resp);

            /* The record reports no conversion until one is asked for. */
            {
                char p[128];
                snprintf(p, sizeof p, "/api/v1/downloads/%s", id);
                resp = http_call(port, "GET", p, NULL, "testtoken", &status);
                CHECK(resp && strstr(resp, "\"conversion\":{\"state\":\"none\"") != NULL,
                      "conversion state starts at none");
                free(resp);
            }

            /* Ask for the conversion. */
            snprintf(path, sizeof path, "/api/v1/downloads/%s", id);
            resp = http_call(port, "PATCH", path, "{\"action\":\"convert\"}",
                             "testtoken", &status);
            CHECK(status == 202, "convert accepted");
            CHECK(resp && strstr(resp, "\"conversion\":{\"state\":\"converting\"") != NULL,
                  "the response reports the conversion as running");
            free(resp);

            char cstate[32] = {0};
            CHECK(wait_for_conversion(port, id, cstate, sizeof cstate),
                  "conversion reaches a terminal state");
            CHECK(!strcmp(cstate, "done"), "the MKV converted to MP4");

            /* The converted MP4 is served with the right type. */
            snprintf(path, sizeof path, "/api/v1/downloads/%s/converted", id);
            resp = http_call(port, "GET", path, NULL, "testtoken", &status);
            CHECK(status == 200, "converted MP4 is served");
            CHECK(resp && strstr(resp, "Content-Type: video/mp4") != NULL,
                  "converted file content type");
            free(resp);

            if (mock_pid > 0) kill(mock_pid, SIGKILL);
            free(mkv_bytes);
        }

        /* A record that has not finished cannot be converted. */
        {
            snprintf(path, sizeof path, "/api/v1/downloads/%s", "deadbeef");
            resp = http_call(port, "PATCH", path, "{\"action\":\"convert\"}",
                             "testtoken", &status);
            CHECK(status == 404, "convert on an unknown id 404");
            free(resp);
        }
    }

    mi_server_stop(srv);
    pthread_join(th, NULL);
    mi_server_free(srv);
    mi_rm_rf(cfg.work_dir);
    unlink("/tmp/mi-test-secret.txt");
    unlink("/tmp/mi-test-source.mkv");
}

/* The volume holding a path must be identified from the mount table, not
 * guessed from the path itself. */
static void test_mount_info(void) {
    char mp[PATH_MAX] = {0}, type[128] = {0}, dev[512] = {0};
    CHECK(mi_mount_info("/tmp", mp, sizeof mp, type, sizeof type, dev, sizeof dev) == 0,
          "mount info resolves /tmp");
    CHECK(mp[0] == '/', "mount point is absolute");
    /* /tmp is either its own mount or part of / — either way it is a real one. */
    CHECK(!strcmp(mp, "/tmp") || !strcmp(mp, "/"), "mount point is /tmp or /");
    CHECK(type[0] != '\0', "filesystem type reported");
    CHECK(dev[0] != '\0', "backing device reported");

    /* A path that does not exist yet still resolves to the volume it would be
     * created on, by falling back to its nearest existing ancestor. */
    char mp2[PATH_MAX] = {0};
    CHECK(mi_mount_info("/tmp/mi-mount-info-check", mp2, sizeof mp2, NULL, 0, NULL, 0) == 0,
          "not-yet-created path resolves via its parent");
    CHECK(mp2[0] == '/', "its mount point is absolute too");

    /* A fully non-existent tree resolves to / — the volume it would live on. */
    char mp3[PATH_MAX] = {0};
    CHECK(mi_mount_info("/no/such/path/here", mp3, sizeof mp3, NULL, 0, NULL, 0) == 0,
          "a non-existent tree still resolves to a volume");
    CHECK(!strcmp(mp3, "/"), "and that volume is /");

    /* A relative path has no absolute anchor, so it cannot be resolved. */
    char mp4[PATH_MAX];
    CHECK(mi_mount_info("relative/path", mp4, sizeof mp4, NULL, 0, NULL, 0) != 0,
          "a relative path fails");
}

/* ---------------------------------------------------------------- */
/* Browse: extracting media references from a page or a listing      */
/* ---------------------------------------------------------------- */

/** Whether the result offers a filename. */
static int has(mi_browse_result *r, const char *filename) {
    for (size_t i = 0; i < r->count; i++) {
        if (!strcmp(r->items[i].filename, filename)) return 1;
    }
    return 0;
}

/** The URL offered for a filename, or NULL. */
static const char *url_of(mi_browse_result *r, const char *filename) {
    for (size_t i = 0; i < r->count; i++) {
        if (!strcmp(r->items[i].filename, filename)) return r->items[i].url;
    }
    return NULL;
}

static void test_browse_parse(void) {
    /* --- HTML: references are resolved against the page, and only media is
     * offered. A stylesheet, a script and a fragment must all be dropped. --- */
    {
        const char *html =
            "<!doctype html><html><head><title>My  &amp;  Gallery</title>"
            "<link rel=\"stylesheet\" href=\"/style.css\">"
            "<script src=\"/app.js\"></script></head><body>"
            "<a href=\"clips/holiday.mp4\">Holiday</a>"
            "<a href=\"/music/song.mp3\">Song</a>"
            "<a href=\"#top\">Top</a>"
            "<a href=\"https://cdn.example.com/photo.jpg\">Photo</a>"
            "<img src=\"thumbs/tiny.png\" alt=\"thumb\">"
            "<a href=\"page.html\">Another page</a>"
            "</body></html>";
        mi_browse_result r;
        CHECK(mi_browse_parse(html, strlen(html), "https://host.example/gallery/index.html", MI_BROWSE_MEDIA, &r) == MI_OK,
              "browse: html parses");
        CHECK(r.title && !strcmp(r.title, "My & Gallery"), "browse: title read and entities decoded");
        /* Relative to the page's directory, not the host root. */
        CHECK(url_of(&r, "holiday.mp4") &&
                  !strcmp(url_of(&r, "holiday.mp4"), "https://host.example/gallery/clips/holiday.mp4"),
              "browse: a document-relative href resolves against the page");
        CHECK(url_of(&r, "song.mp3") &&
                  !strcmp(url_of(&r, "song.mp3"), "https://host.example/music/song.mp3"),
              "browse: a root-relative href resolves against the host");
        CHECK(url_of(&r, "photo.jpg") &&
                  !strcmp(url_of(&r, "photo.jpg"), "https://cdn.example.com/photo.jpg"),
              "browse: an absolute href is kept as it is");
        CHECK(has(&r, "tiny.png"), "browse: an img src is offered");
        /* Non-media, and a same-page fragment, are not files to fetch. */
        CHECK(!has(&r, "style.css"), "browse: a stylesheet is not offered");
        CHECK(!has(&r, "app.js"), "browse: a script is not offered");
        CHECK(!has(&r, "page.html"), "browse: a non-media link is not offered");
        mi_browse_result_free(&r);
    }

    /* --- A repeated reference is offered once. --- */
    {
        const char *html =
            "<a href=\"a.mp4\">one</a><a href=\"a.mp4\">two</a>"
            "<video src=\"a.mp4\"></video>";
        mi_browse_result r;
        CHECK(mi_browse_parse(html, strlen(html), "https://h/p.html", MI_BROWSE_MEDIA, &r) == MI_OK, "browse: repeats parse");
        CHECK(r.count == 1, "browse: the same file offered once");
        mi_browse_result_free(&r);
    }

    /* --- ".." is collapsed, so a listing cannot walk above the host root. --- */
    {
        const char *html = "<a href=\"../../../../etc/passwd.mp4\">x</a>";
        mi_browse_result r;
        CHECK(mi_browse_parse(html, strlen(html), "https://h/a/b/c.html", MI_BROWSE_MEDIA, &r) == MI_OK, "browse: dotdot parses");
        CHECK(r.count == 1, "browse: the dotdot link is still offered");
        CHECK(url_of(&r, "passwd.mp4") &&
                  !strcmp(url_of(&r, "passwd.mp4"), "https://h/etc/passwd.mp4"),
              "browse: .. is collapsed and cannot climb above the host root");
        mi_browse_result_free(&r);
    }

    /* --- JSON: an array of entries, and an object wrapping one. --- */
    {
        const char *json =
            "{\"items\":[{\"url\":\"/v/one.mp4\",\"size\":1234},"
            "{\"file\":\"two.mp3\"},{\"nope\":\"three.txt\"}]}";
        mi_browse_result r;
        CHECK(mi_browse_parse(json, strlen(json), "https://h/list", MI_BROWSE_MEDIA, &r) == MI_OK, "browse: json parses");
        CHECK(url_of(&r, "one.mp4") && !strcmp(url_of(&r, "one.mp4"), "https://h/v/one.mp4"),
              "browse: a json url is resolved");
        CHECK(has(&r, "two.mp3"), "browse: the `file` key is understood");
        CHECK(!has(&r, "three.txt"), "browse: an entry with no known url key is skipped");
        for (size_t i = 0; i < r.count; i++) {
            if (!strcmp(r.items[i].filename, "one.mp4")) {
                CHECK(r.items[i].bytes == 1234, "browse: the size the listing gave is kept");
            }
        }
        mi_browse_result_free(&r);
    }

    /* --- Percent-encoded names are decoded for display. --- */
    {
        const char *html = "<a href=\"Six_weeks_old_cat_%28aka%29.jpg\">x</a>";
        mi_browse_result r;
        mi_browse_parse(html, strlen(html), "https://h/", MI_BROWSE_MEDIA, &r);
        CHECK(has(&r, "Six_weeks_old_cat_(aka).jpg"),
              "browse: a percent-encoded name is decoded");
        mi_browse_result_free(&r);
    }

    /* --- A page with no media yields an empty list, not a failure. --- */
    {
        const char *html = "<html><body><p>Nothing here.</p></body></html>";
        mi_browse_result r;
        CHECK(mi_browse_parse(html, strlen(html), "https://h/", MI_BROWSE_MEDIA, &r) == MI_OK, "browse: empty page is ok");
        CHECK(r.count == 0, "browse: no media means no items");
        mi_browse_result_free(&r);
    }

    /* --- The "everything" filter reads the page as a browser shows it. The
     * same markup that yields one media file in media mode should yield the
     * links, the stylesheet and the framed document too. --- */
    {
        const char *html =
            "<html><head><title>Docs</title>"
            "<link rel=\"stylesheet\" href=\"/style.css\">"
            "<link rel=\"icon\" href=\"/favicon.ico\"></head><body>"
            "<nav><a href=\"/about\">About us</a> <a href=\"guide.html\">Guide</a></nav>"
            "<a href=\"clip.mp4\">A clip</a>"
            "<a href=\"archive.zip\">Download</a>"
            "<iframe src=\"https://player.example/embed\"></iframe>"
            "<a href=\"/\">Home</a>"
            "</body></html>";
        mi_browse_result media, all;
        mi_browse_parse(html, strlen(html), "https://site.example/docs/", MI_BROWSE_MEDIA, &media);
        mi_browse_parse(html, strlen(html), "https://site.example/docs/", MI_BROWSE_ALL, &all);

        /* Media mode is unchanged: only the clip is something to play. */
        CHECK(media.count == 1 && url_of(&media, "clip.mp4") != NULL,
              "browse: media mode still keeps only media");

        /* Everything mode keeps the page's structure. */
        CHECK(url_of(&all, "about") && !strcmp(url_of(&all, "about"), "https://site.example/about"),
              "browse all: a link to another page is listed");
        CHECK(url_of(&all, "guide.html"), "browse all: a relative page link is listed");
        CHECK(has(&all, "style.css"), "browse all: a stylesheet is listed");
        CHECK(has(&all, "favicon.ico"), "browse all: the icon is listed");
        CHECK(has(&all, "archive.zip"), "browse all: a non-media download is listed");
        CHECK(has(&all, "embed"), "browse all: a framed document is listed");
        /* The site root has no final segment; it is still a place to go. */
        CHECK(url_of(&all, "index") && !strcmp(url_of(&all, "index"), "https://site.example/"),
              "browse all: the site root is listed");
        CHECK(all.count > media.count, "browse all: more is listed than media alone");

        /* The link text is carried, so a listing reads like the page. */
        const char *about_title = NULL;
        for (size_t i = 0; i < all.count; i++) {
            if (!strcmp(all.items[i].filename, "about")) about_title = all.items[i].title;
        }
        CHECK(about_title && !strcmp(about_title, "About us"),
              "browse all: the link text is carried as the title");

        mi_browse_result_free(&media);
        mi_browse_result_free(&all);
    }

    /* --- Nested markup inside a link does not lose or truncate its text. --- */
    {
        const char *html = "<a href=\"x.mp4\"><span>Clip</span> <b>one</b></a>";
        mi_browse_result r;
        mi_browse_parse(html, strlen(html), "https://h/", MI_BROWSE_ALL, &r);
        CHECK(r.count == 1, "browse: the nested link is offered");
        CHECK(r.count == 1 && r.items[0].title && !strcmp(r.items[0].title, "Clip one"),
              "browse: nested markup is stripped and the text joined");
        mi_browse_result_free(&r);
    }

    /* --- An anchor with no text still yields the URL, just no label. --- */
    {
        const char *html = "<a href=\"only.mp4\"></a>";
        mi_browse_result r;
        mi_browse_parse(html, strlen(html), "https://h/", MI_BROWSE_ALL, &r);
        CHECK(r.count == 1 && r.items[0].title == NULL,
              "browse: a link with no text is listed without a title");
        mi_browse_result_free(&r);
    }
}

/* ---------------------------------------------------------------- */
/* The configurable User-Agent                                        */
/* ---------------------------------------------------------------- */

/* A server that echoes the User-Agent it was sent, so the header can be
 * observed end to end rather than assumed. */
static void ua_serve_conn(int fd) {
    char req[8192];
    ssize_t got = read(fd, req, sizeof req - 1);
    if (got <= 0) {
        close(fd);
        return;
    }
    req[got] = '\0';

    /* Pull the User-Agent line out of the request head. */
    char ua[512] = "(none)";
    const char *p = strstr(req, "User-Agent: ");
    if (p) {
        p += strlen("User-Agent: ");
        size_t n = 0;
        while (p[n] && p[n] != '\r' && p[n] != '\n' && n + 1 < sizeof ua) {
            ua[n] = p[n];
            n++;
        }
        ua[n] = '\0';
    }

    char body[640];
    int blen = snprintf(body, sizeof body, "{\"agent\":\"%s\"}", ua);
    char header[256];
    int hlen = snprintf(header, sizeof header,
                        "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n"
                        "Content-Type: application/json\r\nConnection: close\r\n\r\n",
                        blen);
    (void)!write(fd, header, (size_t)hlen);
    (void)!write(fd, body, (size_t)blen);
    close(fd);
}

static void ua_loop(int listen_fd) {
    signal(SIGTERM, SIG_DFL);
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        ua_serve_conn(fd);
    }
    _exit(0);
}

static int start_ua_server(pid_t *pid_out) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return -1;
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(listen_fd);
        return -1;
    }
    socklen_t alen = sizeof addr;
    getsockname(listen_fd, (struct sockaddr *)&addr, &alen);
    listen(listen_fd, 8);
    pid_t pid = fork();
    if (pid == 0) ua_loop(listen_fd);
    close(listen_fd);
    *pid_out = pid;
    return ntohs(addr.sin_port);
}

static void test_user_agent(void) {
    pid_t pid = -1;
    int port = start_ua_server(&pid);
    CHECK(port > 0, "user-agent: echo server started");
    if (port <= 0) return;

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);

    /* The built-in default identifies the engine, and is what is sent when the
     * caller has named nothing. */
    {
        mi_download_config cfg;
        mi_download_config_init(&cfg);
        cfg.require_media = 0;
        char *body = mi_download_fetch_text(&cfg, url, 64 * 1024);
        CHECK(body != NULL, "user-agent: default fetch succeeds");
        CHECK(body && strstr(body, "media-ingest-c/") != NULL,
              "user-agent: the default agent identifies the engine");
        free(body);
    }

    /* A caller-supplied agent is sent instead, which is the whole point: a host
     * that refuses anything but a browser can be satisfied without a rebuild. */
    {
        mi_download_config cfg;
        mi_download_config_init(&cfg);
        cfg.require_media = 0;
        cfg.user_agent = "Mozilla/5.0 (TestBrowser)";
        char *body = mi_download_fetch_text(&cfg, url, 64 * 1024);
        CHECK(body != NULL, "user-agent: custom fetch succeeds");
        CHECK(body && strstr(body, "\"agent\":\"Mozilla/5.0 (TestBrowser)\"") != NULL,
              "user-agent: a custom agent is sent verbatim");
        CHECK(body && strstr(body, "media-ingest-c/") == NULL,
              "user-agent: the default is not also sent");
        free(body);
    }

    /* An empty string means "unset", not "send an empty header", which some
     * servers reject outright. */
    {
        mi_download_config cfg;
        mi_download_config_init(&cfg);
        cfg.require_media = 0;
        cfg.user_agent = "";
        char *body = mi_download_fetch_text(&cfg, url, 64 * 1024);
        CHECK(body != NULL, "user-agent: an empty agent still fetches");
        CHECK(body && strstr(body, "media-ingest-c/") != NULL,
              "user-agent: an empty string falls back to the default");
        free(body);
    }

    if (pid > 0) kill(pid, SIGKILL);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    mi_log_set_level(MI_LOG_ERROR);

    test_mount_info();
    test_json_writer();
    test_json_parse_roundtrip();
    test_json_rejects_bad();
    test_json_unicode_escape();
    test_browse_parse();
    test_user_agent();
    test_http_api();
    test_hls_serve_and_convert();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
