/* test_rtc.c - the WebRTC capture pipeline.
 *
 * What runs always: the URL rules, the status names, and the config defaults.
 * What needs a browser, Xvfb and the GStreamer plugins: actually opening a
 * session, and it says so when they are missing rather than passing silently.
 *
 * The behaviours worth pinning are the ones a regression would break quietly:
 * a refused URL must cost nothing, the gate must refuse rather than queue, a
 * real session must produce an SDP offer a browser could answer, and the offer
 * must be complete before it is handed over — because with no signalling socket
 * there is no second chance to send the candidates.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mi_rtc.h"
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
    /* The same rules as a render and the JPEG live path, because all three fetch
     * a page on the caller's behalf. */
    CHECK(mi_rtc_url_allowed("https://example.com/"), "https is allowed");
    CHECK(mi_rtc_url_allowed("http://example.com/"), "http is allowed");
    CHECK(!mi_rtc_url_allowed("file:///etc/passwd"), "file:// is refused");
    CHECK(!mi_rtc_url_allowed("http://127.0.0.1/"), "loopback is refused");
    CHECK(!mi_rtc_url_allowed("http://localhost/"), "localhost is refused");
    CHECK(!mi_rtc_url_allowed(""), "an empty url is refused");
    CHECK(!mi_rtc_url_allowed(NULL), "a null url is refused");
}

static void test_config(void) {
    mi_rtc_config cfg;
    mi_rtc_config_init(&cfg);
    CHECK(cfg.max_concurrent == 1, "one session at a time by default");
    CHECK(cfg.width > 0 && cfg.height > 0, "a viewport is set by default");
    /* A frame rate the eye reads as live, and one every extra frame costs an
     * encode. */
    CHECK(cfg.fps >= 20 && cfg.fps <= 30, "the default frame rate is a live one");
    CHECK(cfg.bitrate_kbps > 0, "a bitrate is set by default");
    CHECK(cfg.idle_timeout_seconds > 0, "an idle timeout is set by default");
    /* Gathering has to be bounded, or a host with a STUN server that never
     * answers would wait forever for an offer. */
    CHECK(cfg.gather_timeout_ms > 0, "gathering has a deadline by default");

    mi_rtc_pool *pool = mi_rtc_pool_new(&cfg);
    CHECK(pool != NULL, "a pool can be made");
    CHECK(mi_rtc_pool_limit(pool) == 1, "the limit is reported");
    CHECK(mi_rtc_pool_active(pool) == 0, "a fresh pool has no sessions");
    /* Availability is answerable either way; on a host with everything it is 1. */
    int avail = mi_rtc_pool_available(pool);
    CHECK(avail == 0 || avail == 1, "availability is answerable");
    mi_rtc_pool_free(pool);
}

static void test_live(void) {
    mi_rtc_config cfg;
    mi_rtc_config_init(&cfg);
    cfg.startup_timeout_seconds = 30;
    cfg.idle_timeout_seconds = 120;
    /* Small, so the test is quick and the encode is cheap. */
    cfg.width = 640;
    cfg.height = 480;

    mi_rtc_pool *pool = mi_rtc_pool_new(&cfg);
    if (!pool) return;
    if (!mi_rtc_pool_available(pool)) {
        SKIP("no browser, Xvfb or GStreamer webrtc plugins: live tests skipped");
        mi_rtc_pool_free(pool);
        return;
    }

    char err[200];
    mi_rtc *rtc = NULL;

    /* A refused URL costs nothing and takes no slot. */
    mi_rtc_status s = mi_rtc_open(pool, "file:///etc/passwd", &rtc, err, sizeof err);
    CHECK(s == MI_RTC_BAD_URL, "a file:// session is refused");
    CHECK(rtc == NULL, "a refused session yields no handle");
    CHECK(mi_rtc_pool_active(pool) == 0, "a refused session took no slot");

    /* A real session: it must produce an offer a browser could answer. */
    s = mi_rtc_open(pool, "https://example.com/", &rtc, err, sizeof err);
    CHECK(s == MI_RTC_OK, "a WebRTC session opens");
    if (s != MI_RTC_OK) {
        fprintf(stderr, "  (open failed: status=%d err=%s)\n", (int)s, err);
        mi_rtc_pool_free(pool);
        return;
    }
    CHECK(mi_rtc_pool_active(pool) == 1, "the session holds a slot");

    /* The handle is returned before the offer exists; that is the point of the
     * split, because startup is not instant. */
    CHECK(mi_rtc_offer(rtc) == NULL, "no offer yet, immediately after open");

    /* Waiting is what produces it. */
    int ok = mi_rtc_wait_offer(rtc, 0);
    CHECK(ok == 0, "the offer arrives within the deadline");
    if (ok != 0) {
        fprintf(stderr, "  (wait failed: %s)\n", mi_rtc_close_reason(rtc));
        mi_rtc_close(rtc);
        mi_rtc_pool_free(pool);
        return;
    }

    /* With no socket to trickle on, the offer has to be final: gathering must
     * have completed so the candidates are already in the SDP. */
    CHECK(mi_rtc_gathering_done(rtc), "gathering finished before the offer was returned");

    char *offer = mi_rtc_offer(rtc);
    CHECK(offer != NULL, "an offer was produced");
    if (offer) {
        CHECK(strstr(offer, "v=0") != NULL, "the offer is SDP");
        CHECK(strstr(offer, "m=video") != NULL, "it offers video");
        CHECK(strstr(offer, "a=ice-ufrag") != NULL, "it carries ICE credentials");
        CHECK(strstr(offer, "a=ice-pwd") != NULL, "and an ICE password");
        CHECK(strstr(offer, "VP8") != NULL, "and names the VP8 codec");
        CHECK(strstr(offer, "UDP/TLS/RTP/SAVPF") != NULL, "over DTLS-SRTP");
        /* A candidate in the SDP is what makes the handshake socket-free. */
        CHECK(strstr(offer, "a=candidate") != NULL, "and its gathered candidates");
    }

    /* The viewport is the X display's size, which is what the browser sees. */
    int w = 0, h = 0;
    mi_rtc_viewport(rtc, &w, &h);
    CHECK(w == 640 && h == 480, "the viewport matches the display");

    /* Pumping must keep working, and the pipeline must not have failed. */
    int alive = 1;
    for (int i = 0; i < 10 && alive; i++) {
        if (mi_rtc_pump(rtc, 50) != 0) alive = 0;
    }
    CHECK(alive, "the session stays alive while pumped");
    CHECK(mi_rtc_state(rtc) != NULL, "the connection state is reportable");
    CHECK(!mi_rtc_connected(rtc), "it is not connected without a browser answering");

    free(offer);
    mi_rtc_close(rtc);
    CHECK(mi_rtc_pool_active(pool) == 0, "closing releases the slot");

    /* Nothing may be left running: a browser or an X server per session would
     * accumulate, and a container's PID 1 does not reap. */
    {
        int leftover = 0;
        for (;;) {
            int st = 0;
            pid_t got = waitpid(-1, &st, WNOHANG);
            if (got <= 0) break;
            leftover++;
        }
        CHECK(leftover == 0, "no browser or display was left behind");
    }

    mi_rtc_pool_free(pool);
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
