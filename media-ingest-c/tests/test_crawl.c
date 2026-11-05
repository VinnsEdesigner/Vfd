/* test_crawl.c - the site crawler.
 *
 * The scoring is tested directly because it decides what a user sees first, and
 * the walk is tested against a real local HTTP server so the parts that only go
 * wrong over a network — link resolution, depth, the page cap, the time budget —
 * are exercised rather than assumed.
 *
 * The double-free this suite exists to prevent: the crawl's frontier owns the
 * URL strings, and the walk must not free one it has taken, or `frontier_free`
 * frees it a second time. That crashed the server on the first real request.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mi_crawl.h"
#include "mi_util.h"

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

/* ------------------------------------------------------------------ */
/* A tiny HTTP server that serves a fixed tree of pages                */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *path; /* what the request asks for */
    const char *body; /* the response body, or NULL for 404 */
} route;

static const route *g_routes = NULL;
static int g_route_count = 0;

static void serve(int fd) {
    /*
     * A connection may carry several requests: libcurl reuses one for the whole
     * crawl. Reading a single request and closing makes the second request on
     * that connection look like a dead server, which is what the first version
     * of this harness did — every page after the first "failed".
     */
    for (;;) {
        char req[2048];
        ssize_t got = read(fd, req, sizeof req - 1);
        if (got <= 0) break;
        req[got] = '\0';

        char path[512] = "/";
        if (sscanf(req, "GET %511s", path) != 1) break;
        /* Drop any query, so "/x?y" matches "/x". */
        char *q = strchr(path, '?');
        if (q) *q = '\0';

        const char *body = NULL;
        for (int i = 0; i < g_route_count; i++) {
            if (!strcmp(g_routes[i].path, path)) { body = g_routes[i].body; break; }
        }

        char header[256];
        if (body) {
            int hlen = snprintf(header, sizeof header,
                                "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                                "Content-Length: %zu\r\nConnection: keep-alive\r\n\r\n",
                                strlen(body));
            (void)!write(fd, header, (size_t)hlen);
            (void)!write(fd, body, strlen(body));
        } else {
            const char *nf = "not found";
            int hlen = snprintf(header, sizeof header,
                                "HTTP/1.1 404 Not Found\r\nContent-Length: %zu\r\n"
                                "Connection: keep-alive\r\n\r\n", strlen(nf));
            (void)!write(fd, header, (size_t)hlen);
            (void)!write(fd, nf, strlen(nf));
        }
    }
    close(fd);
}

static void server_loop(int listen_fd) {
    signal(SIGTERM, SIG_DFL);
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        serve(fd);
    }
    _exit(0);
}

/** Start a server for `routes`. Returns the port, or 0 on failure. */
static int start_site(const route *routes, int count, pid_t *out_pid) {
    g_routes = routes;
    g_route_count = count;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return 0;
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0) { close(listen_fd); return 0; }
    socklen_t alen = sizeof addr;
    getsockname(listen_fd, (struct sockaddr *)&addr, &alen);
    listen(listen_fd, 16);

    pid_t pid = fork();
    if (pid == 0) {
        close(STDIN_FILENO);
        server_loop(listen_fd);
    }
    *out_pid = pid;
    close(listen_fd);
    return ntohs(addr.sin_port);
}

static void stop_site(pid_t pid) {
    if (pid > 0) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); }
}

/* ------------------------------------------------------------------ */
/* Scoring                                                             */
/* ------------------------------------------------------------------ */

static void test_score(void) {
    /*
     * A filename carries an extension, so an exact folded match is not usually
     * reachable — "stm32f103c8.pdf" folds to "stm32f103c8pdf" while the term
     * folds to "stm32f103c8". The substring tier is therefore the top score a
     * real filename reaches, and that is what is asserted.
     */
    CHECK(mi_crawl_score("stm32f103c8.pdf", NULL, "stm32f103c8") >= 75,
          "a filename containing the term scores high");
    /* The exact tier is reachable when the name has no extension. */
    CHECK(mi_crawl_score("stm32f103c8", NULL, "stm32f103c8") >= 90,
          "an exact name match scores highest");
    /* Separators are what people vary, so they must not matter. */
    CHECK(mi_crawl_score("STM32-Datasheet.pdf", NULL, "stm32 datasheet") >= 45,
          "separators do not prevent a match");
    CHECK(mi_crawl_score("stm32_datasheet.pdf", NULL, "stm32-datasheet") >= 45,
          "underscore and hyphen are equivalent");
    /* A term in a different order still matches. */
    CHECK(mi_crawl_score("datasheet-stm32f4.pdf", NULL, "stm32 datasheet") >= 20,
          "word order does not prevent a match");
    /* The link text can carry the match when the filename does not. */
    CHECK(mi_crawl_score("download.php", "STM32F103C8 datasheet", "stm32f103c8") > 0,
          "the link text is searched too");
    /* An unrelated file must not match. */
    CHECK(mi_crawl_score("photo.jpg", "Holiday snap", "stm32 datasheet") == 0,
          "an unrelated file does not match");
    /* No term means everything is equally interesting. */
    CHECK(mi_crawl_score("anything.bin", NULL, "") > 0, "no term matches everything");
    CHECK(mi_crawl_score(NULL, NULL, "x") == 0, "a NULL filename does not crash");
}

/* ------------------------------------------------------------------ */
/* The walk                                                            */
/* ------------------------------------------------------------------ */

static void test_crawl_finds_and_ranks(void) {
    /* Three pages: an index that links a section, and a section that links two
     * files — one a match, one not. */
    static const route routes[] = {
        {"/", "<html><body>"
              "<a href='/section.html'>Section</a>"
              "<a href='/other.html'>Other</a>"
              "</body></html>"},
        {"/section.html", "<html><body>"
              "<a href='/files/stm32f103-datasheet.pdf'>STM32F103 datasheet</a>"
              "<a href='/files/holiday.jpg'>A photo</a>"
              "</body></html>"},
        {"/other.html", "<html><body><a href='/files/notes.txt'>Notes</a></body></html>"},
    };
    pid_t pid = 0;
    int port = start_site(routes, 3, &pid);
    CHECK(port > 0, "site started");
    if (!port) return;

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);

    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    cfg.max_depth = 2;
    cfg.max_pages = 10;
    cfg.use_browser_fallback = 0; /* no browser in the unit test */

    mi_crawl_result res;
    CHECK(mi_crawl(&cfg, url, "stm32 datasheet", &res) == MI_OK, "crawl ran");
    CHECK(res.hit_count == 1, "found exactly the matching file");
    if (res.hit_count > 0) {
        CHECK(strstr(res.hits[0].filename, "stm32f103-datasheet") != NULL,
              "the hit is the datasheet");
        CHECK(res.hits[0].score > 0, "the hit is scored");
        CHECK(res.hits[0].found_on != NULL, "the hit names the page it was on");
        /* The link text is what a person would read. */
        CHECK(res.hits[0].title && strstr(res.hits[0].title, "STM32F103") != NULL,
              "the link text is kept as the title");
    }
    CHECK(res.page_count >= 3, "followed the index into its sections");
    mi_crawl_result_free(&res);
    stop_site(pid);
}

static void test_crawl_depth_zero_stays_on_one_page(void) {
    static const route routes[] = {
        {"/", "<a href='/deep.html'>deep</a>"},
        {"/deep.html", "<a href='/files/target-stm32.pdf'>target</a>"},
    };
    pid_t pid = 0;
    int port = start_site(routes, 2, &pid);
    if (!port) { CHECK(0, "site started"); return; }

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);

    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    cfg.max_depth = 0; /* do not follow */
    cfg.use_browser_fallback = 0;

    mi_crawl_result res;
    mi_crawl(&cfg, url, "stm32", &res);
    CHECK(res.hit_count == 0, "depth 0 does not follow the link");
    CHECK(res.page_count == 1, "depth 0 reads one page");
    mi_crawl_result_free(&res);
    stop_site(pid);
}

/*
 * The regression this suite exists for.
 *
 * The frontier owns its URL strings. The walk takes an entry, reads it, and must
 * NOT free the URL — `frontier_free` frees every entry it holds. Freeing it in
 * both places is a double free, which aborted the server on the first real
 * request. A walk over several pages is what makes it happen, so this reads
 * enough pages to exercise both the take and the free.
 */
static void test_crawl_multi_page_no_double_free(void) {
    static const route routes[] = {
        {"/", "<a href='/a.html'>a</a><a href='/b.html'>b</a><a href='/c.html'>c</a>"},
        {"/a.html", "<a href='/files/a-stm32.pdf'>a</a>"},
        {"/b.html", "<a href='/files/b-stm32.pdf'>b</a>"},
        {"/c.html", "<a href='/files/c-stm32.pdf'>c</a>"},
    };
    pid_t pid = 0;
    int port = start_site(routes, 4, &pid);
    if (!port) { CHECK(0, "site started"); return; }

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);

    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    cfg.max_depth = 2;
    cfg.max_pages = 10;
    cfg.use_browser_fallback = 0;

    mi_crawl_result res;
    CHECK(mi_crawl(&cfg, url, "stm32", &res) == MI_OK, "multi-page crawl ran");
    CHECK(res.hit_count == 3, "found a file on each section");
    /* Reaching here at all is the assertion: a double free would have aborted
     * the process before this line. */
    CHECK(res.page_count == 4, "read every page");
    mi_crawl_result_free(&res);
    stop_site(pid);
}

static void test_crawl_respects_page_cap(void) {
    static const route routes[] = {
        {"/", "<a href='/a.html'>a</a><a href='/b.html'>b</a><a href='/c.html'>c</a>"
              "<a href='/d.html'>d</a><a href='/e.html'>e</a>"},
        {"/a.html", "<a href='/files/x-stm32.pdf'>x</a>"},
        {"/b.html", "<a href='/files/y-stm32.pdf'>y</a>"},
        {"/c.html", "<a href='/files/z-stm32.pdf'>z</a>"},
        {"/d.html", "<a href='/files/w-stm32.pdf'>w</a>"},
        {"/e.html", "<a href='/files/v-stm32.pdf'>v</a>"},
    };
    pid_t pid = 0;
    int port = start_site(routes, 6, &pid);
    if (!port) { CHECK(0, "site started"); return; }

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);

    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    cfg.max_depth = 2;
    cfg.max_pages = 2; /* the index plus one section */
    cfg.use_browser_fallback = 0;

    mi_crawl_result res;
    mi_crawl(&cfg, url, "stm32", &res);
    CHECK(res.page_count <= 2, "the page cap is honoured");
    mi_crawl_result_free(&res);
    stop_site(pid);
}

static void test_crawl_hit_cap_stops_early(void) {
    static const route routes[] = {
        {"/", "<a href='/files/one-stm32.pdf'>1</a><a href='/files/two-stm32.pdf'>2</a>"
              "<a href='/files/three-stm32.pdf'>3</a><a href='/files/four-stm32.pdf'>4</a>"},
    };
    pid_t pid = 0;
    int port = start_site(routes, 1, &pid);
    if (!port) { CHECK(0, "site started"); return; }

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);

    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    cfg.max_hits = 2;
    cfg.use_browser_fallback = 0;

    mi_crawl_result res;
    mi_crawl(&cfg, url, "stm32", &res);
    CHECK(res.hit_count == 2, "stops once it has enough");
    mi_crawl_result_free(&res);
    stop_site(pid);
}

static void test_crawl_unreachable_site_is_not_an_error(void) {
    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    cfg.use_browser_fallback = 0;
    cfg.budget_seconds = 3;
    /* A port nothing is listening on: the walk runs, finds nothing, and says so
     * rather than failing. "The site is unreachable" is a result. */
    mi_crawl_result res;
    CHECK(mi_crawl(&cfg, "http://127.0.0.1:9/", "x", &res) == MI_OK,
          "an unreachable host is a completed walk with no hits");
    CHECK(res.hit_count == 0, "no hits from an unreachable host");
    CHECK(res.failed >= 1, "the failure is counted");
    mi_crawl_result_free(&res);
}

static void test_crawl_rejects_a_bad_url(void) {
    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    mi_crawl_result res;
    CHECK(mi_crawl(&cfg, "file:///etc/passwd", "x", &res) == MI_ERR_ARG,
          "a non-http URL is refused");
    CHECK(mi_crawl(&cfg, "http://127.0.0.1:12000/", "x", NULL) == MI_ERR_ARG,
          "a NULL result is refused");
}

int main(void) {
    test_score();
    test_crawl_finds_and_ranks();
    test_crawl_depth_zero_stays_on_one_page();
    test_crawl_multi_page_no_double_free();
    test_crawl_respects_page_cap();
    test_crawl_hit_cap_stops_early();
    test_crawl_unreachable_site_is_not_an_error();
    test_crawl_rejects_a_bad_url();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
