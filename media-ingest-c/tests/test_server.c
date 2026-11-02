/* test_server.c - JSON round-trip and live HTTP API tests.
 *
 * The API tests start a real server on an ephemeral port and drive it with
 * raw sockets, so routing, auth, JSON and the download lifecycle are all exercised
 * end to end (no mocks).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
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
    free(resp);

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

    /* delete an unknown download */
    resp = http_call(port, "DELETE", "/api/v1/downloads/deadbeef", NULL, "testtoken", &status);
    CHECK(status == 404, "delete unknown download 404");
    free(resp);

    mi_server_stop(srv);
    pthread_join(th, NULL);
    mi_server_free(srv);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    mi_log_set_level(MI_LOG_ERROR);

    test_json_writer();
    test_json_parse_roundtrip();
    test_json_rejects_bad();
    test_json_unicode_escape();
    test_http_api();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
