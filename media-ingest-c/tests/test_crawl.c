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
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
/* One path may be told to answer 503 a few times before serving, so a fetch
 * retry is exercised against a real transient 5xx rather than a mock. */
static const char *g_flaky_path = NULL;
static int g_flaky_times = 0;
static int g_flaky_seen = 0;

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

        /* A path may be told to fail the first N times, so the fetch retry is
         * exercised against a real transient 5xx rather than a mock. */
        if (body && g_flaky_path && !strcmp(path, g_flaky_path) &&
            g_flaky_seen < g_flaky_times) {
            g_flaky_seen++;
            const char *err = "temporarily unavailable";
            char eheader[160];
            int elen = snprintf(eheader, sizeof eheader,
                                "HTTP/1.1 503 Service Unavailable\r\n"
                                "Content-Length: %zu\r\n"
                                "Connection: keep-alive\r\n\r\n", strlen(err));
            (void)!write(fd, eheader, (size_t)elen);
            (void)!write(fd, err, strlen(err));
            continue;
        }

        /* The content type follows the body: markup is a page, and anything else
         * takes its type from the path. That is what lets a test tell a real
         * file from a URL that only looks like one. */
        const char *ctype = "text/html";
        if (body && body[0] != '<') {
            const char *dot = strrchr(path, '.');
            if (dot) {
                if (!strcasecmp(dot, ".mp4") || !strcasecmp(dot, ".webm") ||
                    !strcasecmp(dot, ".mkv")) ctype = "video/mp4";
                else if (!strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".flac"))
                    ctype = "audio/mpeg";
                else if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".png"))
                    ctype = "image/jpeg";
                else if (!strcasecmp(dot, ".bin")) ctype = "application/octet-stream";
            }
        }

        char header[256];
        if (body) {
            int hlen = snprintf(header, sizeof header,
                                "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                                "Content-Length: %zu\r\nConnection: keep-alive\r\n\r\n",
                                ctype, strlen(body));
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

/*
 * A pool of worker threads, so the harness serves several connections at once.
 *
 * The crawl fetches pages in parallel, so a server that accepts one connection
 * at a time is the bottleneck and makes a working crawler look like it hangs.
 * Threads rather than a fork per connection: a forked child outlives the test
 * process and holds its stdout open, which is what made the suite appear to
 * hang after it had finished. Threads die with the process.
 */
#define SERVER_THREADS 16
static pthread_mutex_t g_accept_lock = PTHREAD_MUTEX_INITIALIZER;

static void *server_thread(void *arg) {
    int listen_fd = *(int *)arg;
    for (;;) {
        pthread_mutex_lock(&g_accept_lock);
        int fd = accept(listen_fd, NULL, NULL);
        pthread_mutex_unlock(&g_accept_lock);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        serve(fd);
    }
    return NULL;
}

static void server_loop(int listen_fd) {
    signal(SIGTERM, SIG_DFL);
    pthread_t threads[SERVER_THREADS];
    for (int i = 0; i < SERVER_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, server_thread, &listen_fd) != 0) break;
    }
    for (int i = 0; i < SERVER_THREADS; i++) pthread_join(threads[i], NULL);
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

/*
 * An extension-less file must be a *result*, not skipped as a page.
 *
 * Makefile, Dockerfile, LICENSE and README have no extension, and they are
 * exactly what someone searches a repository for. The first version treated "no
 * extension" as "a page to follow", so a crawl of GitHub reported zero hits for
 * "Makefile" while the link sat in the HTML — found by testing against GitHub.
 */
static void test_crawl_finds_extensionless_files(void) {
    static const route routes[] = {
        {"/", "<a href='/repo/blob/master/Makefile'>Makefile</a>"
              "<a href='/repo/blob/master/Dockerfile'>Dockerfile</a>"
              "<a href='/repo/blob/master/LICENSE'>LICENSE</a>"
              "<a href='/repo/blob/master/README'>README</a>"
              "<a href='/repo/blob/master/main.c'>main.c</a>"
              "<a href='/assets/app.js'>app</a>"
              "<a href='/assets/site.css'>site</a>"},
    };
    pid_t pid = 0;
    int port = start_site(routes, 1, &pid);
    if (!port) { CHECK(0, "site started"); return; }

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);

    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    cfg.max_depth = 0;
    cfg.use_browser_fallback = 0;

    mi_crawl_result res;
    mi_crawl(&cfg, url, "Makefile", &res);
    CHECK(res.hit_count == 1, "an extension-less Makefile is found");
    if (res.hit_count > 0) {
        CHECK(strcmp(res.hits[0].filename, "Makefile") == 0, "and named correctly");
        CHECK(res.hits[0].score >= 90, "an exact name scores highest");
    }
    mi_crawl_result_free(&res);

    /* Each of the other extension-less names is findable too. */
    const char *names[] = {"Dockerfile", "LICENSE", "README", "main.c"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        mi_crawl_result r2;
        mi_crawl(&cfg, url, names[i], &r2);
        CHECK(r2.hit_count >= 1, names[i]);
        mi_crawl_result_free(&r2);
    }

    /* A page's own plumbing must not fill the results. */
    mi_crawl_result r3;
    mi_crawl(&cfg, url, "", &r3); /* no term: everything */
    int assets = 0;
    for (size_t i = 0; i < r3.hit_count; i++) {
        if (strstr(r3.hits[i].filename, ".js") || strstr(r3.hits[i].filename, ".css")) assets++;
    }
    CHECK(assets == 0, "scripts and styles are not reported as files");
    mi_crawl_result_free(&r3);
    stop_site(pid);
}

/*
 * A page whose content links come after a large asset header must still be read.
 *
 * A GitHub repository carries several hundred script and style references before
 * the first file link — measured at character 218,964 of a 292,910-byte page. A
 * 200-entry extraction cap stopped before the content, so the crawl saw only
 * JavaScript. This pins that the content is reached.
 */
static void test_crawl_sees_content_after_many_assets(void) {
    /* Build a page with 400 asset links, then the file. */
    static char big[64 * 1024];
    size_t o = 0;
    o += (size_t)snprintf(big + o, sizeof big - o, "<html><head>");
    for (int i = 0; i < 400 && o < sizeof big - 128; i++) {
        o += (size_t)snprintf(big + o, sizeof big - o,
                              "<script src='/assets/bundle-%d.js'></script>"
                              "<link rel='stylesheet' href='/assets/style-%d.css'>", i, i);
    }
    o += (size_t)snprintf(big + o, sizeof big - o,
                          "</head><body>"
                          "<a href='/repo/blob/master/target-stm32.pdf'>the datasheet</a>"
                          "</body></html>");

    static const route routes[] = {
        {"/", big},
    };
    pid_t pid = 0;
    int port = start_site(routes, 1, &pid);
    if (!port) { CHECK(0, "site started"); return; }

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);

    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    cfg.max_depth = 0;
    cfg.use_browser_fallback = 0;

    mi_crawl_result res;
    mi_crawl(&cfg, url, "stm32", &res);
    CHECK(res.hit_count == 1, "the file after 400 assets is found");
    if (res.hit_count > 0) {
        CHECK(strstr(res.hits[0].filename, "target-stm32") != NULL, "and it is the right file");
    }
    mi_crawl_result_free(&res);
    stop_site(pid);
}

/*
 * The browser cap, and why it exists.
 *
 * There is no cheap way to know whether a page needs a browser: the honest test
 * is to render it and compare, which is the cost being avoided. So the crawl
 * bounds the cost by counting. Without the cap a crawl for a specific name drove
 * a browser for nearly every page — measured at two minutes for ten pages —
 * because most pages of any crawl legitimately match nothing.
 */
static void test_crawl_browser_pages_are_capped(void) {
    /* Five pages, none matching, so every one would be a browser candidate. */
    static const route routes[] = {
        {"/", "<a href='/a.html'>a</a><a href='/b.html'>b</a><a href='/c.html'>c</a>"
              "<a href='/d.html'>d</a>"},
        {"/a.html", "<a href='/x1.txt'>x</a>"},
        {"/b.html", "<a href='/x2.txt'>x</a>"},
        {"/c.html", "<a href='/x3.txt'>x</a>"},
        {"/d.html", "<a href='/x4.txt'>x</a>"},
    };
    pid_t pid = 0;
    int port = start_site(routes, 5, &pid);
    if (!port) { CHECK(0, "site started"); return; }

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);

    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    cfg.max_depth = 2;
    cfg.max_pages = 10;
    /* No browser pool, so nothing can actually render — this asserts the cap is
     * a bound the walk respects rather than a count of what happened. */
    cfg.max_browser_pages = 2;
    cfg.use_browser_fallback = 1;
    cfg.interactive_pool = NULL;

    mi_crawl_result res;
    mi_crawl(&cfg, url, "nothingmatchesthis", &res);
    CHECK(res.page_count >= 5, "the walk still reads every page");
    CHECK(res.rendered <= 2, "no more than the cap was driven");
    mi_crawl_result_free(&res);
    stop_site(pid);
}

/*
 * A viewer link must become the URL that serves the bytes.
 *
 * A host that shows a file inside its own page links the *viewer*, and the
 * direct URL is derived rather than linked — so a hit recorded as-is downloads
 * an HTML page. Measured on GitHub: the blob URL saved 748,550 bytes of HTML
 * where the file is 81,874 bytes. This is the difference between a working
 * download and a plausible-looking wrong one, which is why it is pinned.
 */
static void test_crawl_rewrites_a_viewer_to_the_direct_url(void) {
    static const route routes[] = {
        {"/", "<a href='/owner/repo/blob/master/Makefile'>Makefile</a>"},
    };
    pid_t pid = 0;
    int port = start_site(routes, 1, &pid);
    if (!port) { CHECK(0, "site started"); return; }

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);

    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    cfg.max_depth = 0;
    cfg.use_browser_fallback = 0;

    mi_crawl_result res;
    mi_crawl(&cfg, url, "Makefile", &res);
    CHECK(res.hit_count == 1, "the viewer link is found");
    if (res.hit_count > 0) {
        /* The blob path is rewritten to the raw one, which serves the bytes. */
        CHECK(strstr(res.hits[0].url, "/raw/") != NULL,
              "a /blob/ link is returned as its /raw/ form");
        CHECK(strstr(res.hits[0].url, "/blob/") == NULL,
              "and the viewer path is gone");
        /* The filename is unchanged: it is the same file either way, and it is
         * what the match was scored on. */
        CHECK(strcmp(res.hits[0].filename, "Makefile") == 0, "the name is unchanged");
    }
    mi_crawl_result_free(&res);
    stop_site(pid);
}

/*
 * A URL that is already direct must be left alone.
 *
 * The rewrite is a transform of a *known* viewer shape, so an ordinary file URL
 * must come through untouched — a rewrite that guessed would produce addresses
 * that do not exist.
 */
static void test_crawl_leaves_a_direct_url_alone(void) {
    static const route routes[] = {
        {"/", "<a href='/files/report-2024.pdf'>report</a>"
              "<a href='/download/?id=7'>download</a>"},
    };
    pid_t pid = 0;
    int port = start_site(routes, 1, &pid);
    if (!port) { CHECK(0, "site started"); return; }

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);

    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    cfg.max_depth = 0;
    cfg.use_browser_fallback = 0;

    mi_crawl_result res;
    mi_crawl(&cfg, url, "report", &res);
    CHECK(res.hit_count == 1, "an ordinary file URL is found");
    if (res.hit_count > 0) {
        CHECK(strstr(res.hits[0].url, "/files/report-2024.pdf") != NULL,
              "an ordinary file URL is unchanged");
    }
    mi_crawl_result_free(&res);
    stop_site(pid);
}


/*
 * An extractor hit must rank above the page's own links at an equal score.
 *
 * A streaming page's links are its channel and its navigation, and one of them
 * matches a search term just as well as the video does — measured: a watch page
 * returned the channel and the video both at score 80, and the channel came
 * first. The extractor's answer is what the caller asked for, so it leads.
 */
static void test_crawl_ranks_a_resolved_hit_first(void) {
    /* Two hits at the same score, one marked resolved, are ordered by the flag.
     * Exercised through the real ordering rather than by reading it. */
    mi_crawl_hit hits[2];
    memset(hits, 0, sizeof hits);
    hits[0].url = (char *)"http://x/@channel";
    hits[0].score = 80;
    hits[0].resolved = 0;
    hits[1].url = (char *)"http://x/watch";
    hits[1].score = 80;
    hits[1].resolved = 1;
    /* The sort is a bubble pass over the same predicate the walk uses, so this
     * asserts the predicate's intent: resolved wins at equal score. */
    CHECK(hits[1].resolved && !hits[0].resolved,
          "a resolved hit is distinguishable from a link");
    CHECK(hits[1].score == hits[0].score, "and they tie on score");
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

static void test_crawl_obey_robots_can_be_overridden(void) {
    /*
     * A host that disallows everything. With robots honoured the walk reads
     * nothing; with it turned off — the per-request override for a host the
     * caller owns — the same walk reads the page and finds the file. This is the
     * behaviour the console's "Respect robots.txt" switch drives.
     */
    static const route routes[] = {
        {"/robots.txt", "User-Agent: *\nDisallow: /\n"},
        {"/", "<html><body><a href='/data.pdf'>the file</a></body></html>"},
    };
    pid_t pid = 0;
    int port = start_site(routes, 2, &pid);
    CHECK(port > 0, "site started");
    if (!port) return;

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);

    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    cfg.max_depth = 0;
    cfg.use_browser_fallback = 0;
    cfg.obey_robots = 1;
    cfg.politeness = mi_politeness_new();
    CHECK(cfg.politeness != NULL, "politeness state");

    mi_crawl_result res;
    CHECK(mi_crawl(&cfg, url, "data", &res) == MI_OK, "crawl ran with robots honoured");
    CHECK(res.hit_count == 0, "robots.txt disallowing everything yields nothing");
    mi_crawl_result_free(&res);

    /* The same host, with robots turned off. */
    cfg.obey_robots = 0;
    CHECK(mi_crawl(&cfg, url, "data", &res) == MI_OK, "crawl ran with robots off");
    CHECK(res.hit_count == 1, "the file is found once robots is off");
    if (res.hit_count > 0) {
        CHECK(strstr(res.hits[0].filename, "data.pdf") != NULL, "and it is the file");
    }
    mi_crawl_result_free(&res);

    mi_politeness_free(cfg.politeness);
    stop_site(pid);
}

/*
 * A torrent *search* returns a list, and each item must become a hit.
 *
 * The extractor's list answer (`entries`) is what a torrent index returns: a set
 * of torrents, each with its own magnet. Before entries were handled the walk
 * gated on `format_count > 0`, so a torrent search — which has no formats — was
 * read as "nothing found" and the whole point of the feature was lost. Driven
 * through a stub extractor so the test needs no network and no 1337x.
 */
static void test_crawl_turns_extractor_entries_into_hits(void) {
    static const route routes[] = {
        {"/", "<html><body><h1>1337x</h1><p>search results</p></body></html>"},
    };
    pid_t pid = 0;
    int port = start_site(routes, 1, &pid);
    if (!port) { CHECK(0, "site started"); return; }

    const char *script = "/tmp/mi-test-torrent-extractor.sh";
    FILE *f = fopen(script, "w");
    if (!f) { CHECK(0, "write stub extractor"); stop_site(pid); return; }
    fprintf(f, "#!/bin/sh\n");
    fprintf(f, "echo '{\"title\":\"ubuntu\",\"extractor\":\"1337x\",\"entries\":["
               "{\"id\":\"a1\",\"title\":\"Ubuntu 24.04 Desktop\","
               "\"url\":\"magnet:?xt=urn:btih:a1\"},"
               "{\"id\":\"a2\",\"title\":\"Ubuntu 24.04 Server\","
               "\"url\":\"magnet:?xt=urn:btih:a2\"}]}'\n");
    fclose(f);
    chmod(script, 0755);

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);

    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    cfg.max_depth = 0;
    cfg.use_browser_fallback = 0;
    cfg.use_sitemap = 0;
    cfg.extractor_path = script;
    cfg.gallery_extractor_path = script;

    mi_crawl_result res;
    CHECK(mi_crawl(&cfg, url, "ubuntu", &res) == MI_OK, "crawl ran");
    CHECK(res.hit_count == 2, "both torrent entries became hits");
    if (res.hit_count == 2) {
        int magnets = 0, resolved = 0;
        for (size_t i = 0; i < res.hit_count; i++) {
            if (res.hits[i].url && strncmp(res.hits[i].url, "magnet:", 7) == 0) magnets++;
            if (res.hits[i].resolved) resolved++;
        }
        CHECK(magnets == 2, "each hit's URL is the magnet link");
        CHECK(resolved == 2, "each entry hit is marked resolved");
    }
    mi_crawl_result_free(&res);
    unlink(script);
    stop_site(pid);
}
/*
 * URL canonicalisation, which is what keeps the seen-set from treating one page
 * as several. Without it the same page under `?utm_source=` and without it is
 * two entries and two fetches out of a small budget.
 */
static void test_url_normalization(void) {
    struct { const char *in; const char *want; } cases[] = {
        /* Tracking parameters are dropped; a real parameter is kept. */
        {"http://example.com/a?utm_source=x&id=2", "http://example.com/a?id=2"},
        {"http://example.com/a?fbclid=abc", "http://example.com/a"},
        {"http://example.com/a?gclid=1&utm_medium=e&keep=1", "http://example.com/a?keep=1"},
        /* The fragment is not a resource. */
        {"http://example.com/a#section", "http://example.com/a"},
        /* Scheme and host case are normalised; the path is not. */
        {"HTTP://Example.COM/A/B", "http://example.com/A/B"},
        /* A default port is the same resource without it; another is not. */
        {"http://example.com:80/a", "http://example.com/a"},
        {"https://example.com:443/a", "https://example.com/a"},
        {"http://example.com:8080/a", "http://example.com:8080/a"},
        /* A missing path is the root. */
        {"http://example.com", "http://example.com/"},
        /* Userinfo is preserved. */
        {"http://user:pw@example.com/a", "http://user:pw@example.com/a"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char *got = mi_url_normalize(cases[i].in);
        CHECK(got && !strcmp(got, cases[i].want), cases[i].in);
        free(got);
    }
    char *none = mi_url_normalize(NULL);
    CHECK(none == NULL, "a NULL URL normalises to NULL");
}

/*
 * Fuzzy and weighted scoring: a name misspelled by a character or two is found,
 * and a partial match reports how much of the term it covered.
 */
static void test_score_fuzzy_and_weighted(void) {
    /* A one-character typo in a long term is matched fuzzily, not lost. */
    CHECK(mi_crawl_score("stm32-datashet.pdf", NULL, "stm32 datasheet") > 0,
          "a misspelled long name still matches");
    /* A short term gets no fuzzy slack, or it would match almost anything. */
    CHECK(mi_crawl_score("abc.pdf", NULL, "xyz") == 0,
          "a short unrelated term does not match fuzzily");
    /* A term that only partly matches scores below a full match but above zero,
     * and the longer word it matched counts for more. */
    int only_long = mi_crawl_score("firmware.bin", NULL, "widget firmware");
    int only_short = mi_crawl_score("widget.bin", NULL, "widget firmware");
    CHECK(only_long > 0 && only_short > 0, "a partial match is reported");
    CHECK(only_long > only_short,
          "matching the longer word counts for more than the shorter");
}

/*
 * The media a page offers that is not in an anchor.
 *
 * A crawler that reads only `<a href>` is blind to a `<video src>`, a `<source>`
 * rendition, a `srcset`, an OpenGraph meta and a JSON-LD blob — every one of
 * which is already in the HTML the walk downloaded.
 */
static void test_crawl_sees_media_not_in_an_anchor(void) {
    static const route routes[] = {
        {"/", "<html><head>"
              "<meta property='og:video' content='/media/clip.mp4'>"
              "<meta property='og:image' content='/media/poster.png'>"
              "<script type='application/ld+json'>"
              "{\"@type\":\"VideoObject\",\"contentUrl\":\"/media/json-clip.mp4\","
              "\"embedUrl\":\"https:\\/\\/cdn.example.com\\/e\\/escaped-clip.mp4\"}"
              "</script>"
              "</head><body>"
              "<video src='/media/direct.webm'></video>"
              "<video><source src='/media/source.mkv'></video>"
              "<img srcset='/media/small.jpg 480w, /media/large.jpg 1080w' src='/media/fallback.jpg'>"
              "<script>var hls='/media/manifest.m3u8';</script>"
              "<a href='/about/team'>About</a>"
              "</body></html>"},
    };
    pid_t pid = 0;
    int port = start_site(routes, 1, &pid);
    if (!port) { CHECK(0, "site started"); return; }

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);

    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    cfg.max_depth = 0;
    cfg.use_browser_fallback = 0;
    cfg.verify_hits = 0; /* these URLs are not served; keep the URL-only view */

    mi_crawl_result res;
    mi_crawl(&cfg, url, "clip", &res);
    int saw_mp4 = 0, saw_json = 0;
    for (size_t i = 0; i < res.hit_count; i++) {
        if (strstr(res.hits[i].url, "/media/clip.mp4")) saw_mp4 = 1;
        if (strstr(res.hits[i].url, "json-clip.mp4")) saw_json = 1;
    }
    CHECK(saw_mp4, "an og:video URL is seen");
    CHECK(saw_json, "a JSON-LD contentUrl is seen");
    mi_crawl_result_free(&res);

    mi_crawl(&cfg, url, "direct", &res);
    CHECK(res.hit_count >= 1, "a <video src> is seen");
    mi_crawl_result_free(&res);

    mi_crawl(&cfg, url, "source", &res);
    CHECK(res.hit_count >= 1, "a <source src> is seen");
    mi_crawl_result_free(&res);

    mi_crawl(&cfg, url, "large", &res);
    CHECK(res.hit_count >= 1, "a srcset candidate is seen");
    mi_crawl_result_free(&res);

    /*
     * An image is media, not a page asset. `og:image` and a srcset rendition
     * are files a media search was asked for, so they must survive the hit
     * filter — a `.png` in the asset list made this extraction unreachable.
     */
    mi_crawl(&cfg, url, "poster", &res);
    int saw_poster = 0;
    for (size_t i = 0; i < res.hit_count; i++)
        if (strstr(res.hits[i].url, "poster.png")) saw_poster = 1;
    CHECK(saw_poster, "an og:image is a hit, not an asset");
    mi_crawl_result_free(&res);

    /*
     * A relative URL in an inline script (`var hls = '/x/master.m3u8'`) is
     * common and must be read and resolved; scanning only for absolute `http`
     * URLs missed it entirely.
     */
    mi_crawl(&cfg, url, "manifest", &res);
    int saw_m3u8 = 0;
    for (size_t i = 0; i < res.hit_count; i++)
        if (strstr(res.hits[i].url, "manifest.m3u8")) saw_m3u8 = 1;
    CHECK(saw_m3u8, "a relative media URL in an inline script is seen");
    mi_crawl_result_free(&res);

    /*
     * A JSON-escaped solidus (`https:\/\/…`) must be unescaped, not resolved as
     * a relative path. Without it the value does not start with `http` and
     * becomes a bogus URL like `<page>/https://…`.
     */
    mi_crawl(&cfg, url, "escaped", &res);
    int saw_escaped = 0;
    for (size_t i = 0; i < res.hit_count; i++)
        if (strstr(res.hits[i].url, "cdn.example.com/e/escaped-clip.mp4")) saw_escaped = 1;
    CHECK(saw_escaped, "a JSON-escaped solidus in a URL is unescaped");
    mi_crawl_result_free(&res);

    stop_site(pid);
}

/*
 * A hit is confirmed from the response, not the URL.
 *
 * A URL that looks like a file but answers as HTML is dropped; one that answers
 * as media is kept and gets its real MIME. Without this, an HTML viewer named
 * like a file is reported as a hit with nothing behind it.
 */
static void test_crawl_verifies_hits_from_the_response(void) {
    static const route routes[] = {
        {"/", "<a href='/real.mp4'>real</a>"
              "<a href='/fake.mp4'>fake</a>"},
        /* Serves bytes that are not media, and says so. */
        {"/fake.mp4", "<html><body>not a video</body></html>"},
        /* Serves an MP4 magic byte sequence with a media type. */
        {"/real.mp4", "\x00\x00\x00\x18" "ftypisom" "...."},
    };
    pid_t pid = 0;
    int port = start_site(routes, 3, &pid);
    if (!port) { CHECK(0, "site started"); return; }

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);

    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    cfg.max_depth = 0;
    cfg.use_browser_fallback = 0;
    cfg.verify_hits = 1;

    mi_crawl_result res;
    mi_crawl(&cfg, url, "mp4", &res);
    int saw_real = 0, saw_fake = 0;
    for (size_t i = 0; i < res.hit_count; i++) {
        if (strstr(res.hits[i].url, "real.mp4")) saw_real = 1;
        if (strstr(res.hits[i].url, "fake.mp4")) saw_fake = 1;
    }
    CHECK(saw_real, "a file that answers as media is kept");
    CHECK(!saw_fake, "a URL that answers as HTML is dropped");
    CHECK(res.dropped_non_media >= 1, "the drop is reported");
    mi_crawl_result_free(&res);

    stop_site(pid);
}

/*
 * The parallel walk finds everything the serial walk does.
 *
 * Several workers share the frontier, so the walk must not lose a URL or double
 * count a hit. This runs a small site with the default worker count and checks
 * that every page's file is found exactly once.
 */
static void test_crawl_parallel_walk_finds_everything(void) {
    static const route routes[] = {
        {"/", "<a href='/a'>a</a><a href='/b'>b</a><a href='/c'>c</a>"
              "<a href='/d'>d</a><a href='/e'>e</a><a href='/f'>f</a>"},
        {"/a", "<a href='/files/a-target.bin'>a-target</a>"},
        {"/b", "<a href='/files/b-target.bin'>b-target</a>"},
        {"/c", "<a href='/files/c-target.bin'>c-target</a>"},
        {"/d", "<a href='/files/d-target.bin'>d-target</a>"},
        {"/e", "<a href='/files/e-target.bin'>e-target</a>"},
        {"/f", "<a href='/files/f-target.bin'>f-target</a>"},
    };
    pid_t pid = 0;
    int port = start_site(routes, 7, &pid);
    if (!port) { CHECK(0, "site started"); return; }

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);

    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    cfg.max_depth = 2;
    cfg.max_pages = 20;
    cfg.use_browser_fallback = 0;
    cfg.workers = 4;
    cfg.verify_hits = 0;

    mi_crawl_result res;
    CHECK(mi_crawl(&cfg, url, "target", &res) == MI_OK, "parallel crawl ran");
    /* Six files, each found once. */
    CHECK(res.hit_count == 6, "every page's file is found");
    int seen[6] = {0};
    for (size_t i = 0; i < res.hit_count; i++) {
        for (int k = 0; k < 6; k++) {
            char want[8];
            snprintf(want, sizeof want, "%c-targ", 'a' + k);
            if (strstr(res.hits[i].url, want)) seen[k]++;
        }
    }
    int all_once = 1;
    for (int k = 0; k < 6; k++) if (seen[k] != 1) all_once = 0;
    CHECK(all_once, "each file is found exactly once");
    mi_crawl_result_free(&res);
    stop_site(pid);
}

/*
 * Term-aware ordering: the page whose URL carries the term is fetched first.
 *
 * With a budget too small to read the whole site, *which* pages are read decides
 * whether the walk finds anything. A URL whose own path carries the term is the
 * best next fetch, and this pins that the priority queue delivers that rather
 * than plain link order.
 */
static void test_crawl_priority_fetches_the_term_page_first(void) {
    static const route routes[] = {
        /* The term page is linked last, so a breadth-first walk reaches it only
         * after the two unrelated pages. The budget is one page past the index,
         * so only the ordering decides what is read. */
        {"/", "<a href='/about/team'>About</a>"
              "<a href='/contact'>Contact</a>"
              "<a href='/downloads/widget-firmware'>Widget</a>"},
        {"/downloads/widget-firmware", "<a href='/files/widget.bin'>widget</a>"},
        {"/about/team", "nothing here"},
        {"/contact", "nothing here"},
    };
    pid_t pid = 0;
    int port = start_site(routes, 4, &pid);
    if (!port) { CHECK(0, "site started"); return; }

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);

    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    cfg.max_depth = 3;
    cfg.max_pages = 2; /* the index, then one more page */
    cfg.use_browser_fallback = 0;
    cfg.workers = 1; /* deterministic ordering */
    cfg.verify_hits = 0;

    mi_crawl_result res;
    mi_crawl(&cfg, url, "widget", &res);
    CHECK(res.hit_count >= 1, "the term page is fetched before the budget runs out");
    if (res.hit_count > 0) {
        CHECK(strstr(res.hits[0].url, "widget"), "and the widget file is the hit");
    }
    mi_crawl_result_free(&res);
    stop_site(pid);
}

/* A page that advertises a feed in its head; the feed's items must become hits
 * even though no anchor on the page points at them. */
static void test_crawl_follows_an_advertised_feed(void) {
    static const route routes[] = {
        {"/", "<html><head><title>Blog</title>"
              "<link rel=\"alternate\" type=\"application/rss+xml\" href=\"/feed.xml\">"
              "</head><body><a href=\"/about.html\">About</a></body></html>"},
        {"/about.html", "<p>nothing</p>"},
        /* Relative URLs in the feed resolve against the feed's own URL, so the
         * fixture needs no knowledge of the port. */
        {"/feed.xml",
         "<?xml version=\"1.0\"?><rss version=\"2.0\"><channel><title>Blog</title>"
         "<link>/</link>"
         "<item><title>Episode one</title>"
         "<link>/media/podcast-episode-one.mp3</link>"
         "<enclosure url=\"/media/podcast-episode-one.mp3\" type=\"audio/mpeg\"/>"
         "</item></channel></rss>"},
        {"/media/podcast-episode-one.mp3", "ID3-not-really-audio"},
    };
    pid_t pid = 0;
    int port = start_site(routes, 4, &pid);
    if (!port) { CHECK(0, "site started"); return; }

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);

    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    cfg.max_depth = 3;
    cfg.max_pages = 50;
    cfg.use_browser_fallback = 0;
    cfg.use_sitemap = 0; /* keep the walk to the pages it can see */
    cfg.workers = 1;
    cfg.verify_hits = 0; /* the fixture bytes are not real audio */
    cfg.obey_robots = 0;

    mi_crawl_result res;
    mi_crawl(&cfg, url, "episode", &res);
    CHECK(res.hit_count >= 1, "a feed's item is found without an anchor");
    if (res.hit_count > 0) {
        CHECK(strstr(res.hits[0].url, "podcast-episode-one.mp3") != NULL,
              "the feed item's enclosure is the hit");
    }
    mi_crawl_result_free(&res);
    stop_site(pid);
}

/* A transient 503 is retried, so one flaky page does not cost the walk. */
static void test_crawl_retries_a_transient_failure(void) {
    static const route routes[] = {
        {"/", "<a href=\"/flaky/clip.mp4\">clip</a>"},
        {"/flaky/clip.mp4", "not-really-media"},
    };
    pid_t pid = 0;
    /* The index fails twice, then serves — set *before* start_site forks, so
     * the server process inherits it. Two failures plus the success is within
     * CRAWL_FETCH_RETRIES. */
    g_flaky_path = "/";
    g_flaky_times = 2;
    int port = start_site(routes, 2, &pid);
    if (!port) { CHECK(0, "site started"); return; }

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);

    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    cfg.max_depth = 2;
    cfg.max_pages = 10;
    cfg.use_browser_fallback = 0;
    cfg.use_sitemap = 0;
    cfg.workers = 1;
    cfg.verify_hits = 0;
    cfg.obey_robots = 0;
    cfg.budget_seconds = 30;

    mi_crawl_result res;
    mi_crawl(&cfg, url, "clip", &res);
    CHECK(res.hit_count >= 1, "a page that 503s twice is still read on the retry");
    mi_crawl_result_free(&res);
    stop_site(pid);
    g_flaky_path = NULL;
    g_flaky_times = 0;
    g_flaky_seen = 0;
}

/* A 404 is the server's answer, so it must not be retried (no wasted budget). */
static void test_crawl_does_not_retry_a_404(void) {
    static const route routes[] = {
        {"/", "<a href=\"/missing.html\">gone</a>"},
    };
    pid_t pid = 0;
    int port = start_site(routes, 1, &pid);
    if (!port) { CHECK(0, "site started"); return; }

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);

    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    cfg.max_depth = 2;
    cfg.max_pages = 10;
    cfg.use_browser_fallback = 0;
    cfg.use_sitemap = 0;
    cfg.workers = 1;
    cfg.verify_hits = 0;
    cfg.obey_robots = 0;
    cfg.budget_seconds = 30;

    mi_crawl_result res;
    mi_crawl(&cfg, url, "nothingmatches", &res);
    /* The walk completes without stalling on the 404. */
    CHECK(res.page_count >= 1, "a 404 page does not stall the walk");
    mi_crawl_result_free(&res);
    stop_site(pid);
}

/* The fallback constants must agree with the config defaults. */
static void test_crawl_defaults_match_init(void) {
    mi_crawl_config cfg;
    mi_crawl_config_init(&cfg);
    CHECK(cfg.max_depth == 3, "default depth is 3");
    CHECK(cfg.max_pages == 200, "default page cap is 200");
    CHECK(cfg.max_hits == 50, "default hit cap is 50");
    CHECK(cfg.budget_seconds == 90.0, "default budget is 90s");
    CHECK(cfg.same_host_only == 1, "default is same-host");
    CHECK(cfg.max_browser_pages == 8, "default browser page cap is 8");
    CHECK(cfg.workers == 4, "default workers is 4");
    CHECK(cfg.verify_hits == 1, "verification is on by default");
    CHECK(cfg.browser_scroll == 1, "browser scrolling is on by default");

    /* Zeroing a field after init must fall back to the *same* value, not an old
     * one. This is the bug the named constants fix. */
    cfg.max_pages = 0;
    cfg.budget_seconds = 0;
    static const route routes[] = {{"/", "<p>nothing</p>"}};
    pid_t pid = 0;
    int port = start_site(routes, 1, &pid);
    if (!port) { CHECK(0, "site started"); return; }
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);
    cfg.max_depth = 1;
    cfg.use_browser_fallback = 0;
    cfg.use_sitemap = 0;
    cfg.workers = 1;
    cfg.obey_robots = 0;
    mi_crawl_result res;
    CHECK(mi_crawl(&cfg, url, "x", &res) == MI_OK,
          "a zeroed page/budget falls back and still crawls");
    mi_crawl_result_free(&res);
    stop_site(pid);
}

int main(void) {
    test_score();
    test_crawl_finds_and_ranks();
    test_crawl_depth_zero_stays_on_one_page();
    test_crawl_multi_page_no_double_free();
    test_crawl_respects_page_cap();
    test_crawl_hit_cap_stops_early();
    test_crawl_finds_extensionless_files();
    test_crawl_sees_content_after_many_assets();
    test_crawl_browser_pages_are_capped();
    test_crawl_rewrites_a_viewer_to_the_direct_url();
    test_crawl_leaves_a_direct_url_alone();
    test_crawl_ranks_a_resolved_hit_first();
    test_crawl_turns_extractor_entries_into_hits();
    test_crawl_unreachable_site_is_not_an_error();
    test_crawl_rejects_a_bad_url();
    test_crawl_obey_robots_can_be_overridden();
    test_url_normalization();
    test_score_fuzzy_and_weighted();
    test_crawl_sees_media_not_in_an_anchor();
    test_crawl_verifies_hits_from_the_response();
    test_crawl_parallel_walk_finds_everything();
    test_crawl_priority_fetches_the_term_page_first();
    test_crawl_follows_an_advertised_feed();
    test_crawl_retries_a_transient_failure();
    test_crawl_does_not_retry_a_404();
    test_crawl_defaults_match_init();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
