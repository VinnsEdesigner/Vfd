/* test_session.c - the login flow, end to end against a local server.
 *
 * A real login has three moving parts, and this server exercises all three: it
 * sets a pre-login cookie on the GET, requires a CSRF token in the POST, and
 * answers a correct login with a session cookie and a redirect. The test proves
 * the flow carries the token, follows the redirect, and ends with a usable
 * session — which is the whole point.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mi_cookiejar.h"
#include "mi_download.h"
#include "mi_session.h"

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

/*
 * A minimal login server. It speaks just enough HTTP/1.1 for the fetch layer.
 *
 *   GET  /login   -> the form, sets a `pre` cookie, embeds a CSRF token
 *   POST /login   -> if the token and credentials are right, 302 to /home and
 *                    set `sid`; otherwise re-render the form
 *   GET  /home    -> 200, but only when the `sid` cookie is present
 */
static int g_port;
static volatile int g_stop;
static char g_last_post[2048];

static void *server_thread(void *arg) {
    (void)arg;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)g_port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) return NULL;
    listen(fd, 8);

    while (!g_stop) {
        int c = accept(fd, NULL, NULL);
        if (c < 0) continue;
        char req[8192];
        size_t total = 0;
        ssize_t n = 0;
        /* Read until the headers and the whole body have arrived: curl may send
         * the request line and the body in separate packets. */
        while (total < sizeof req - 1 &&
               (n = read(c, req + total, sizeof req - 1 - total)) > 0) {
            total += (size_t)n;
            req[total] = 0;
            char *hdr_end = strstr(req, "\r\n\r\n");
            if (hdr_end) {
                char *cl = strcasestr(req, "Content-Length:");
                long want = 0;
                if (cl) want = atol(cl + 15);
                if (total >= (size_t)(hdr_end - req) + 4 + (size_t)want) break;
            }
        }
        if (total == 0) { close(c); continue; }

        /* Split the request line and any body. */
        char method[8] = {0}, path[256] = {0};
        sscanf(req, "%7s %255s", method, path);
        char *body = strstr(req, "\r\n\r\n");
        body = body ? body + 4 : NULL;
        int has_sid = strstr(req, "sid=") != NULL;

        char resp[4096];
        int rl = 0;
        if (!strcmp(method, "GET") && !strcmp(path, "/login")) {
            const char *html =
                "<html><body><form action=\"/login\" method=\"post\">"
                "<input name=\"username\"><input type=\"password\" name=\"password\">"
                "<input type=\"hidden\" name=\"csrf_token\" value=\"TOK\">"
                "</form></body></html>";
            rl = snprintf(resp, sizeof resp,
                "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                "Set-Cookie: pre=1; Path=/\r\nContent-Length: %zu\r\n\r\n%s",
                strlen(html), html);
        } else if (!strcmp(method, "POST") && !strcmp(path, "/login")) {
            if (body) { snprintf(g_last_post, sizeof g_last_post, "%s", body); }
            int good = body && strstr(body, "csrf_token=TOK") &&
                       strstr(body, "username=alice") && strstr(body, "password=secret");
            if (good) {
                rl = snprintf(resp, sizeof resp,
                    "HTTP/1.1 302 Found\r\nLocation: /home\r\n"
                    "Set-Cookie: sid=SESSION123; Path=/\r\nContent-Length: 0\r\n\r\n");
            } else {
                const char *html =
                    "<html><body><form action=\"/login\" method=\"post\">"
                    "<input name=\"username\"><input type=\"password\" name=\"password\">"
                    "<input type=\"hidden\" name=\"csrf_token\" value=\"TOK\">"
                    "</form><p>Invalid</p></body></html>";
                rl = snprintf(resp, sizeof resp,
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                    "Content-Length: %zu\r\n\r\n%s", strlen(html), html);
            }
        } else if (!strcmp(method, "GET") && !strcmp(path, "/home")) {
            if (has_sid) {
                const char *html = "<html><body><h1>Welcome</h1></body></html>";
                rl = snprintf(resp, sizeof resp,
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                    "Content-Length: %zu\r\n\r\n%s", strlen(html), html);
            } else {
                rl = snprintf(resp, sizeof resp,
                    "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n");
            }
        } else {
            rl = snprintf(resp, sizeof resp,
                "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
        }
        write(c, resp, (size_t)rl);
        close(c);
    }
    close(fd);
    return NULL;
}

int main(void) {
    /* Bind an ephemeral port first, then tell the thread which. */
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(probe, (struct sockaddr *)&a, sizeof a);
    socklen_t al = sizeof a;
    getsockname(probe, (struct sockaddr *)&a, &al);
    g_port = ntohs(a.sin_port);
    close(probe);

    pthread_t th;
    pthread_create(&th, NULL, server_thread, NULL);
    usleep(200000);

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/login", g_port);

    mi_download_config cfg;
    mi_download_config_init(&cfg);
    cfg.require_media = 0;
    cfg.timeout_seconds = 5;

    mi_cookiejar *jar = mi_cookiejar_new();
    mi_login_result r = mi_session_login(jar, &cfg, url, "alice", "secret", NULL);
    CHECK(r.status == MI_LOGIN_OK, "a correct login succeeds");
    CHECK(r.final_url && strstr(r.final_url, "/home") != NULL,
          "the login followed the redirect to /home");
    /* The CSRF token from the page was carried into the POST. */
    CHECK(strstr(g_last_post, "csrf_token=TOK") != NULL, "the CSRF token was posted");
    CHECK(strstr(g_last_post, "username=alice") != NULL, "the username was posted");
    /* The session cookie is now in the jar, and authenticates a later request. */
    CHECK(mi_cookiejar_has(jar, "127.0.0.1", "sid"), "the session cookie is in the jar");
    char *h = mi_cookiejar_header_for_url(jar, url);
    CHECK(h && strstr(h, "sid=SESSION123") != NULL, "and is sent on the next request");
    free(h);
    mi_login_result_free(&r);

    /* A wrong password is reported as rejected, not a crash or a success. */
    mi_cookiejar *jar2 = mi_cookiejar_new();
    r = mi_session_login(jar2, &cfg, url, "alice", "wrong", NULL);
    CHECK(r.status == MI_LOGIN_REJECTED, "a wrong password is rejected");
    mi_login_result_free(&r);
    mi_cookiejar_free(jar2);

    mi_cookiejar_free(jar);
    g_stop = 1;
    /* Nudge the accept so the thread leaves. */
    int wake = socket(AF_INET, SOCK_STREAM, 0);
    connect(wake, (struct sockaddr *)&a, sizeof a);
    close(wake);
    pthread_join(th, NULL);

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
