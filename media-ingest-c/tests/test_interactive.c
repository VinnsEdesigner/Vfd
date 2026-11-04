/* test_interactive.c - the live page session.
 *
 * As with the renderer's tests, the pure parts always run and anything that
 * needs a browser is skipped when the host has none, saying so rather than
 * passing silently. The behaviours worth pinning are the ones a regression
 * would quietly break: a refused URL must cost nothing, the gate must refuse
 * rather than queue, and a live session must actually produce frames.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "mi_interactive.h"
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

#define SKIP(msg)                             \
    do {                                      \
        g_skipped++;                          \
        fprintf(stderr, "skip: %s\n", (msg)); \
    } while (0)

static void test_url_rules(void) {
    /* The same rules as the renderer: a live session drives a real page, so it
     * may reach exactly what a render may. */
    CHECK(mi_interactive_url_allowed("https://example.com/"), "https is allowed");
    CHECK(mi_interactive_url_allowed("http://example.com/"), "http is allowed");
    CHECK(!mi_interactive_url_allowed("file:///etc/passwd"), "file:// is refused");
    CHECK(!mi_interactive_url_allowed("http://127.0.0.1/"), "loopback is refused");
    CHECK(!mi_interactive_url_allowed("http://localhost/"), "localhost is refused");
    CHECK(!mi_interactive_url_allowed(""), "an empty url is refused");
    CHECK(!mi_interactive_url_allowed(NULL), "a null url is refused");
}

static void test_config(void) {
    mi_interactive_config cfg;
    mi_interactive_config_init(&cfg);
    CHECK(cfg.max_concurrent == 1, "one session at a time by default");
    CHECK(cfg.idle_timeout_seconds > 0, "an idle timeout is set by default");
    CHECK(cfg.viewport_width > 0 && cfg.viewport_height > 0, "a viewport is set by default");

    /* A pool exists even without a browser, and reports that honestly. */
    mi_interactive_pool *pool = mi_interactive_pool_new(&cfg);
    CHECK(pool != NULL, "a pool can be made");
    CHECK(mi_interactive_pool_limit(pool) == 1, "the limit is reported");
    CHECK(mi_interactive_pool_active(pool) == 0, "a fresh pool has no sessions");
    CHECK(mi_interactive_pool_available(pool) == 1 || mi_interactive_pool_available(pool) == 0,
          "availability is answerable");
    mi_interactive_pool_free(pool);
}

static void test_live(void) {
    mi_interactive_config cfg;
    mi_interactive_config_init(&cfg);
    cfg.startup_timeout_seconds = 20;
    cfg.idle_timeout_seconds = 60;

    mi_interactive_pool *pool = mi_interactive_pool_new(&cfg);
    if (!pool) return;
    if (!mi_interactive_pool_available(pool)) {
        SKIP("no chromium on this host: the live session tests are skipped");
        mi_interactive_pool_free(pool);
        return;
    }

    char err[200];
    mi_interactive *it = NULL;

    /* A refused URL costs nothing and does not take a slot. */
    mi_it_status s = mi_interactive_open(pool, "file:///etc/passwd", 800, 600, 0, &it, err, sizeof err);
    CHECK(s == MI_IT_BAD_URL, "a file:// session is refused");
    CHECK(it == NULL, "a refused session yields no handle");
    CHECK(mi_interactive_pool_active(pool) == 0, "a refused session took no slot");

    /* A real session: open, pump for a frame, then close. */
    s = mi_interactive_open(pool, "https://example.com/", 800, 600, 0, &it, err, sizeof err);
    CHECK(s == MI_IT_OK, "a live session opens");
    if (s != MI_IT_OK) fprintf(stderr, "  (open failed: status=%d err=%s)\n", (int)s, err);
    if (s == MI_IT_OK) {
        CHECK(mi_interactive_pool_active(pool) == 1, "the session holds a slot");

        /*
         * The keepalive, which is what frees the slot of a client that vanished
         * without a close. A session that has just opened has been heard from, so
         * it must not be considered silent — if the clock started at allocation
         * instead, the startup exchange itself would count as silence and a
         * healthy session would be dropped the moment it opened.
         */
        CHECK(mi_interactive_silence_limit(it) > 0, "a silence limit is configured");
        CHECK(mi_interactive_silence(it) < 1.0, "a fresh session is not silent");
        CHECK(mi_interactive_should_ping(it) == 0, "a fresh session is not pinged at once");
        mi_interactive_note_heard(it);
        CHECK(mi_interactive_silence(it) < 1.0, "being heard resets the silence clock");

        /* Pump until a frame arrives or the deadline passes. example.com is
         * small and static, so a frame should come within a few seconds. */
        int frames = 0;
        int states = 0;
        for (int i = 0; i < 100 && frames == 0; i++) {
            mi_it_event ev = mi_interactive_pump(it, 100);
            if (ev == MI_IT_EV_FRAME || ev == MI_IT_EV_SHOT) frames++;
            else if (ev == MI_IT_EV_STATE) states++;
            else if (ev == MI_IT_EV_CLOSED) break;
        }
        CHECK(frames > 0, "the session produced a frame");
        if (frames > 0) {
            size_t len = 0;
            const unsigned char *f = mi_interactive_frame(it, &len);
            CHECK(f != NULL && len > 100, "the frame has bytes");
            /* A JPEG starts with FF D8. This proves it is an image and not an
             * error page or an empty buffer. */
            CHECK(f && len > 2 && f[0] == 0xFF && f[1] == 0xD8, "the frame is a JPEG");
            CHECK(mi_interactive_frame_width(it) == 800, "the frame is the width we asked for");
            CHECK(mi_interactive_frame_height(it) == 600, "and the height");
        }
        CHECK(states > 0 || frames > 0, "the session reported a state or a frame");

        const char *st = mi_interactive_state(it);
        CHECK(st != NULL && strstr(st, "\"url\"") != NULL, "the state carries the page url");

        mi_interactive_close(it);
        CHECK(mi_interactive_pool_active(pool) == 0, "closing releases the slot");
    }

    /* No child of THIS process may be left behind. The test process is the
     * direct parent of every browser it spawned, so anything still around is
     * something close() failed to kill and reap — the leak the whole
     * kill-the-group-then-reap dance exists to prevent. */
    {
        int leftover = 0;
        for (;;) {
            int st = 0;
            pid_t got = waitpid(-1, &st, WNOHANG);
            if (got <= 0) break;
            leftover++;
        }
        CHECK(leftover == 0, "no browser process was left behind");
    }

    /* The gate refuses rather than queueing. Two sessions with a limit of one:
     * the second must be refused, not made to wait. */
    {
        mi_interactive *a = NULL, *b = NULL;
        mi_it_status sa = mi_interactive_open(pool, "https://example.com/", 320, 240, 0, &a,
                                              err, sizeof err);
        if (sa == MI_IT_OK) {
            mi_it_status sb = mi_interactive_open(pool, "https://example.com/", 320, 240, 0, &b,
                                                  err, sizeof err);
            CHECK(sb == MI_IT_BUSY, "a second session is refused, not queued");
            CHECK(b == NULL, "the refused session yields no handle");
            mi_interactive_close(a);
        } else {
            SKIP("could not open a session to test the gate");
        }
    }

    mi_interactive_pool_free(pool);
}

int main(void) {
    mi_log_set_level(MI_LOG_ERROR);
    test_url_rules();
    test_config();
    test_live();
    printf("\n%d checks, %d failures", g_checks, g_failures);
    if (g_skipped) printf(", %d skipped", g_skipped);
    printf("\n");
    return g_failures == 0 ? 0 : 1;
}
