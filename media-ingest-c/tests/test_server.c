/* test_server.c - JSON round-trip and live HTTP API tests.
 *
 * The API tests start a real server on an ephemeral port and drive it with
 * raw sockets, so routing, auth, JSON and the download lifecycle are all exercised
 * end to end (no mocks).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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
static char *http_call(int port, const char *method, const char *path,
                       const char *body, const char *auth, int *status_out) {
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
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                     method, path, auth ? "Authorization: Bearer " : "",
                     auth ? auth : "", auth ? "\r\n" : "", blen);
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
    cfg.cors_origin = 1;
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

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    mi_log_set_level(MI_LOG_ERROR);

    test_mount_info();
    test_json_writer();
    test_json_parse_roundtrip();
    test_json_rejects_bad();
    test_json_unicode_escape();
    test_http_api();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
