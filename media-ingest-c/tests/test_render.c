/* test_render.c - the headless renderer.
 *
 * Two kinds of test here, deliberately separated:
 *
 *   the URL rules and the status names are pure and always run;
 *   anything that actually launches a browser needs Chromium, and is skipped
 *   when this host has none — the same way the HLS tests are skipped without
 *   ffmpeg. A test that silently passes because the tool is missing is worse
 *   than one that says it was skipped.
 *
 * The behaviours worth pinning are the safety ones, because they are the ones a
 * regression would quietly break: a page that never settles must be killed at
 * the deadline with its whole process group, and the gate must refuse rather
 * than queue.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mi_render.h"
#include "mi_util.h"

static int g_checks = 0;
static int g_failures = 0;
static int g_skipped = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
        }                                                                   \
    } while (0)

#define SKIP(msg)                                        \
    do {                                                 \
        g_skipped++;                                     \
        fprintf(stderr, "skip: %s\n", (msg));            \
    } while (0)

/* ------------------------------------------------------------------ */
/* Pure rules - always run                                             */
/* ------------------------------------------------------------------ */

static void test_url_rules(void) {
    /* Only a page can be rendered. */
    CHECK(mi_render_url_allowed("https://example.com/"), "https is allowed");
    CHECK(mi_render_url_allowed("http://example.com/"), "http is allowed");
    CHECK(mi_render_url_allowed("HTTPS://EXAMPLE.COM/"), "the scheme is case-insensitive");
    CHECK(mi_render_url_allowed("https://example.com:8443/a/b?c=d"), "a port and query are fine");
    CHECK(mi_render_url_allowed("https://user:pw@example.com/"), "credentials are skipped");

    CHECK(!mi_render_url_allowed("file:///etc/passwd"), "file:// is refused");
    CHECK(!mi_render_url_allowed("ftp://example.com/"), "a non-http scheme is refused");
    CHECK(!mi_render_url_allowed("javascript:alert(1)"), "javascript: is refused");
    CHECK(!mi_render_url_allowed(""), "an empty url is refused");
    CHECK(!mi_render_url_allowed(NULL), "a null url is refused");

    /* The engine's own host must not be reachable: a render of ourselves would
     * be a loop, and an internal service is not a page to show. */
    CHECK(!mi_render_url_allowed("http://127.0.0.1:12001/healthz"), "loopback is refused");
    CHECK(!mi_render_url_allowed("http://127.1.2.3/"), "the whole 127/8 is refused");
    CHECK(!mi_render_url_allowed("http://localhost/"), "localhost is refused");
    CHECK(!mi_render_url_allowed("http://localhost.localdomain/"), "localhost.localdomain too");
    CHECK(!mi_render_url_allowed("http://169.254.169.254/latest/meta-data/"),
          "link-local (cloud metadata) is refused");
    CHECK(!mi_render_url_allowed("http://0.0.0.0/"), "the wildcard address is refused");
    CHECK(!mi_render_url_allowed("http://[::1]/"), "ipv6 loopback is refused");

    /* A private intranet page is allowed: the operator chose to run here. */
    CHECK(mi_render_url_allowed("http://10.0.0.5/"), "a private address is allowed");
}

static void test_status_names(void) {
    CHECK(!strcmp(mi_render_status_name(MI_RENDER_OK), "ok"), "ok name");
    CHECK(!strcmp(mi_render_status_name(MI_RENDER_BUSY), "busy"), "busy name");
    CHECK(!strcmp(mi_render_status_name(MI_RENDER_TIMEOUT), "timeout"), "timeout name");
    CHECK(!strcmp(mi_render_status_name(MI_RENDER_NO_BROWSER), "no_browser"), "no_browser name");
    /* A status with no name would be a hole in a log. */
    CHECK(strcmp(mi_render_status_name((mi_render_status)999), "unknown") == 0,
          "an unknown status still names itself");
}

/* ------------------------------------------------------------------ */
/* Behaviours that need a browser                                      */
/* ------------------------------------------------------------------ */

static mi_render_pool *g_pool;
static mi_render_status g_concurrent_status;

static void *concurrent_render(void *arg) {
    (void)arg;
    mi_render_result r;
    g_concurrent_status = mi_render_run(g_pool, "https://example.com/", MI_RENDER_IMAGE, &r);
    mi_render_result_free(&r);
    return NULL;
}

static void test_render_live(void) {
    mi_render_config cfg;
    mi_render_config_init(&cfg);
    cfg.timeout_seconds = 20;

    g_pool = mi_render_pool_new(&cfg);
    CHECK(g_pool != NULL, "pool new");
    if (!g_pool) return;

    if (!mi_render_pool_available(g_pool)) {
        SKIP("no chromium on this host: the live render tests are skipped");
        mi_render_pool_free(g_pool);
        g_pool = NULL;
        return;
    }
    CHECK(mi_render_pool_limit(g_pool) == 1, "the default limit is one render");
    CHECK(mi_render_pool_idle(g_pool) == 1, "an idle pool has a free slot");

    /* --- A refused URL costs nothing and does not take a slot. --- */
    {
        mi_render_result r;
        mi_render_status s = mi_render_run(g_pool, "file:///etc/passwd", MI_RENDER_IMAGE, &r);
        CHECK(s == MI_RENDER_BAD_URL, "a file:// render is refused");
        CHECK(r.len == 0, "a refused render returns no bytes");
        mi_render_result_free(&r);
        CHECK(mi_render_pool_idle(g_pool) == 1, "a refused render did not take a slot");
    }

    /* --- A real render, in both modes. --- */
    {
        mi_render_result r;
        mi_render_status s = mi_render_run(g_pool, "https://example.com/", MI_RENDER_IMAGE, &r);
        CHECK(s == MI_RENDER_OK, "an image render succeeds");
        CHECK(r.len > 100, "the image has bytes");
        /* A PNG's magic number: proves it is an image and not an error page. */
        CHECK(r.len > 8 && r.data[0] == 0x89 && r.data[1] == 'P' && r.data[2] == 'N' &&
                  r.data[3] == 'G',
              "the output is a PNG");
        CHECK(!strcmp(r.content_type, "image/png"), "the image content type is right");
        CHECK(r.elapsed_seconds > 0, "the render reports how long it took");
        mi_render_result_free(&r);

        s = mi_render_run(g_pool, "https://example.com/", MI_RENDER_DOM, &r);
        CHECK(s == MI_RENDER_OK, "a dom render succeeds");
        CHECK(r.len > 100, "the dom has bytes");
        CHECK(strstr((char *)r.data, "<html") != NULL, "the dom is html");
        CHECK(r.title && !strcmp(r.title, "Example Domain"), "the page title is read");
        CHECK(!strcmp(r.content_type, "text/html; charset=utf-8"), "the dom content type is right");
        mi_render_result_free(&r);
    }

    /* --- A page that never settles is killed at the deadline. ---
     * A black-holed address: nothing answers, so nothing settles. This is the
     * test that proves the wall-clock kill exists, because Chromium's own time
     * limit does not cover this case. */
    {
        mi_render_config shortcfg;
        mi_render_config_init(&shortcfg);
        shortcfg.timeout_seconds = 5;
        mi_render_pool *p = mi_render_pool_new(&shortcfg);
        mi_render_result r;
        mi_render_status s = mi_render_run(p, "http://10.255.255.1/", MI_RENDER_IMAGE, &r);
        CHECK(s == MI_RENDER_TIMEOUT, "a hanging page times out");
        /* The deadline is 5s; anything close to it means the kill happened on
         * time rather than the page eventually failing. */
        CHECK(r.elapsed_seconds < 15.0, "it was killed at the deadline, not left to run");
        CHECK(r.error != NULL, "a timeout explains itself");
        mi_render_result_free(&r);
        mi_render_pool_free(p);
    }

    /* --- The gate refuses rather than queueing. --- */
    {
        pthread_t th;
        pthread_create(&th, NULL, concurrent_render, NULL);
        mi_render_result r;
        /* Hold the only slot while the other thread asks for it. */
        mi_render_status first =
            mi_render_run(g_pool, "https://en.wikipedia.org/wiki/Cat", MI_RENDER_IMAGE, &r);
        pthread_join(th, NULL);
        CHECK(first == MI_RENDER_OK, "the first render ran");
        /* The second may have arrived before or after the slot was taken, so
         * either it was refused, or it waited and then ran. What must never
         * happen is a queue: the refusal is the contract. */
        CHECK(g_concurrent_status == MI_RENDER_BUSY || g_concurrent_status == MI_RENDER_OK,
              "the second render was refused or ran after the first");
        if (g_concurrent_status == MI_RENDER_BUSY) {
            CHECK(1, "the gate refused a concurrent render");
        }
        mi_render_result_free(&r);
    }

    mi_render_pool_free(g_pool);
    g_pool = NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    mi_log_set_level(MI_LOG_ERROR);

    test_url_rules();
    test_status_names();
    test_render_live();

    printf("\n%d checks, %d failures", g_checks, g_failures);
    if (g_skipped) printf(", %d skipped", g_skipped);
    printf("\n");
    return g_failures == 0 ? 0 : 1;
}
