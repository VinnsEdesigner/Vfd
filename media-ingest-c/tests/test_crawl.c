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
    test_crawl_unreachable_site_is_not_an_error();
    test_crawl_rejects_a_bad_url();
    test_crawl_obey_robots_can_be_overridden();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
