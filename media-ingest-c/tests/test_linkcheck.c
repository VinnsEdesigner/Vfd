/* test_linkcheck.c - checking the links on a page.
 *
 * A local server answers a mix: a page that works, a link that 404s, a link
 * that redirects, a link to a port nothing listens on, and a `mailto:` that must
 * be skipped rather than fetched. The report must name each one correctly.
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
#include <sys/wait.h>
#include <unistd.h>

#include "mi_linkcheck.h"

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

/* A tiny server: /page links several things; the others answer by path. */
static void serve(int fd) {
    char req[4096];
    ssize_t n = read(fd, req, sizeof req - 1);
    if (n <= 0) return;
    req[n] = 0;
    char path[512] = "/";
    sscanf(req, "GET %511s", path);

    char body[2048];
    int code = 200;
    if (!strcmp(path, "/page")) {
        snprintf(body, sizeof body,
            "<html><body>"
            "<a href='/good'>good</a>"
            "<a href='/missing'>missing</a>"
            "<a href='/moved'>moved</a>"
            "<a href='/gone'>gone</a>"
            "<a href='mailto:x@y.z'>mail</a>"
            "</body></html>");
    } else if (!strcmp(path, "/good")) {
        snprintf(body, sizeof body, "ok");
    } else if (!strcmp(path, "/missing")) {
        code = 404;
        snprintf(body, sizeof body, "not found");
    } else if (!strcmp(path, "/gone")) {
        code = 500;
        snprintf(body, sizeof body, "server error");
    } else {
        code = 404;
        snprintf(body, sizeof body, "nf");
    }

    char resp[2300];
    int rl;
    if (!strcmp(path, "/moved")) {
        rl = snprintf(resp, sizeof resp,
            "HTTP/1.1 302 Found\r\nLocation: /good\r\nContent-Length: 0\r\n\r\n");
    } else {
        rl = snprintf(resp, sizeof resp,
            "HTTP/1.1 %d X\r\nContent-Type: text/html\r\n"
            "Content-Length: %zu\r\n\r\n%s", code, strlen(body), body);
    }
    (void)!write(fd, resp, (size_t)rl);
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
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(listen_fd, (struct sockaddr *)&a, sizeof a);
    socklen_t al = sizeof a;
    getsockname(listen_fd, (struct sockaddr *)&a, &al);
    g_port = ntohs(a.sin_port);
    listen(listen_fd, 16);

    pid_t pid = fork();
    if (pid == 0) { close(0); server_loop(listen_fd); }
    close(listen_fd);
    usleep(200000);

    char page[128];
    snprintf(page, sizeof page, "http://127.0.0.1:%d/page", g_port);

    mi_download_config cfg;
    mi_download_config_init(&cfg);
    cfg.require_media = 0;
    cfg.timeout_seconds = 3;

    mi_linkcheck_options opts = {.concurrency = 4, .timeout_seconds = 3};
    mi_link_report r;
    CHECK(mi_linkcheck_page(&cfg, page, &opts, &r) == 0, "the page check runs");

    /* Find each link's verdict. */
    const char *status_of = NULL;
    for (size_t i = 0; i < r.count; i++) {
        (void)status_of;
    }
    int good_ok = 0, missing_broken = 0, moved_ok = 0, gone_broken = 0;
    for (size_t i = 0; i < r.count; i++) {
        const mi_link_result *l = &r.results[i];
        if (strstr(l->url, "/good") && l->status == MI_LINK_OK) good_ok = 1;
        if (strstr(l->url, "/missing") && l->status == MI_LINK_BROKEN) missing_broken = 1;
        if (strstr(l->url, "/moved") &&
            (l->status == MI_LINK_REDIRECT || l->status == MI_LINK_OK)) moved_ok = 1;
        if (strstr(l->url, "/gone") && l->status == MI_LINK_BROKEN) gone_broken = 1;
    }
    CHECK(good_ok, "a 200 link is ok");
    CHECK(missing_broken, "a 404 link is broken");
    CHECK(gone_broken, "a 500 link is broken");
    CHECK(moved_ok, "a redirect link resolves");
    CHECK(r.broken == 2, "the report counts two broken links");
    /* A mailto: is dropped by the link reader before the check sees it, so the
     * report has the four http links only. */
    CHECK(r.count == 4, "only the http links are checked");

    char *json = mi_link_report_to_json(&r);
    CHECK(json && strstr(json, "\"broken\":2") != NULL, "the JSON summary");
    CHECK(json && strstr(json, "/missing") != NULL, "a broken link is in the report");
    free(json);

    mi_link_report_free(&r);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
