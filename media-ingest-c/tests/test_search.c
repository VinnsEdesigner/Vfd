/* test_search.c - submitting a site's own search form.
 *
 * Two shapes are tested: a GET search (the common one, term in the query) and a
 * POST search with a CSRF token (the one that needs the form module). The server
 * answers the search with a page of result links, which is what the module
 * returns.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mi_search.h"

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

static int g_port;

static void respond(int fd, const char *body, int code) {
    char resp[4096];
    int rl = snprintf(resp, sizeof resp,
        "HTTP/1.1 %d X\r\nContent-Type: text/html\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
        code, strlen(body), body);
    (void)!write(fd, resp, (size_t)rl);
}

static void serve(int fd) {
    char req[4096];
    ssize_t n = read(fd, req, sizeof req - 1);
    if (n <= 0) return;
    req[n] = 0;
    char method[8] = {0}, path[1024] = {0};
    sscanf(req, "%7s %1023s", method, path);

    if (!strcmp(path, "/get")) {
        /* A GET search form. */
        respond(fd, "<form action='/results' method='get'>"
                    "<input type='text' name='q'><input type='submit'></form>", 200);
    } else if (!strcmp(path, "/post")) {
        /* A POST search form with a CSRF token. */
        respond(fd, "<form action='/results' method='post'>"
                    "<input type='text' name='query'>"
                    "<input type='hidden' name='csrf_token' value='TOK'>"
                    "<input type='submit'></form>", 200);
    } else if (!strncmp(path, "/results", 8)) {
        /* The results page: two links. */
        respond(fd, "<html><body>"
                    "<a href='/doc/one'>One</a>"
                    "<a href='/doc/two'>Two</a>"
                    "</body></html>", 200);
    } else {
        respond(fd, "not found", 404);
    }
}

static void server_loop(int listen_fd) {
    signal(SIGTERM, SIG_DFL);
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) break;
        serve(fd);
        close(fd);
    }
    _exit(0);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int lf = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lf, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(lf, (struct sockaddr *)&a, sizeof a);
    socklen_t al = sizeof a;
    getsockname(lf, (struct sockaddr *)&a, &al);
    g_port = ntohs(a.sin_port);
    listen(lf, 16);

    pid_t pid = fork();
    if (pid == 0) { close(0); server_loop(lf); }
    close(lf);
    usleep(200000);

    mi_download_config cfg;
    mi_download_config_init(&cfg);
    cfg.require_media = 0;
    cfg.timeout_seconds = 3;

    /* A GET search. */
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/get", g_port);
    mi_search_result r;
    CHECK(mi_search_site(&cfg, url, "hello world", NULL, &r) == 0, "a GET search runs");
    CHECK(r.count == 2, "the result links are returned");
    CHECK(r.count == 2 && strstr(r.urls[0], "/doc/one") != NULL, "the first result");
    mi_search_result_free(&r);

    /* A POST search with a CSRF token. */
    snprintf(url, sizeof url, "http://127.0.0.1:%d/post", g_port);
    CHECK(mi_search_site(&cfg, url, "find me", NULL, &r) == 0, "a POST search runs");
    CHECK(r.count == 2, "the POST search returns its links too");
    mi_search_result_free(&r);

    /* A page with no search form is reported, not a crash. */
    snprintf(url, sizeof url, "http://127.0.0.1:%d/results", g_port);
    CHECK(mi_search_site(&cfg, url, "x", NULL, &r) == -1, "a page with no form fails");
    CHECK(r.detail != NULL, "and says why");
    mi_search_result_free(&r);

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
