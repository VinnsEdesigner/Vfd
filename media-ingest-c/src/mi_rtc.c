/* mi_rtc.c - a live page as raw video over WebRTC, with input on a data channel.
 *
 * THE SHAPE
 *
 *   chromium on Xvfb --paints--> X display --ximagesrc--> vp8enc --> webrtcbin
 *                                                                     |
 *   input (navigate/mouse/key/touch) <-- data channel <---------------+
 *                                     --> CDP pipe --> chromium
 *
 * Two channels, two jobs, and neither is a WebSocket: X carries pixels out, the
 * WebRTC data channel carries input in, and the CDP pipe is how that input
 * reaches the page. Signalling is plain HTTP (see mi_rtc.h) because WebRTC
 * cannot bootstrap itself.
 *
 * The pipeline is driven by a GLib main loop that `mi_rtc_pump` runs. That is the
 * shape GStreamer wants: `webrtcbin`'s signals (negotiation needed, ICE candidate,
 * data channel) are delivered on the main context, so a session that is not
 * pumped does not negotiate, does not send, and never notices a browser has gone.
 *
 * WHY GATHERING IS AWAITED
 *
 * With no socket there is nothing to trickle ICE on, so the offer is only handed
 * over once gathering has finished and its candidates are in the SDP. That is one
 * request instead of a stream of them, at the cost of a short wait at startup.
 */
#define _GNU_SOURCE
#include "mi_rtc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>

#include "mi_json.h"
#include "mi_render.h" /* the shared URL rules */
#include "mi_util.h"

/* Chromium's names, in the order they are usually found. */
static const char *const BROWSERS[] = {
    "chromium", "chromium-browser", "google-chrome", "google-chrome-stable",
    "chrome", NULL,
};

/* The label of the data channel that carries input. One name, agreed by both
 * ends, so Vyzor can find the right channel whichever end opened it. */
static const char *const INPUT_LABEL = "input";

/* The CDP pipe: chromium reads fd 3 and writes fd 4. */
#define CDP_READ_FD 3
#define CDP_WRITE_FD 4
/* A cap on queued CDP bytes, so a console that floods input cannot grow the
 * Vyzor's memory without bound. */
#define CDP_WRITE_MAX (1024u * 1024u)

struct mi_rtc_pool {
    mi_rtc_config cfg;
    char *browser;
    char *xvfb;
    pthread_mutex_t lock;
    int in_use;
    /* Displays are numbered, so two sessions do not collide. */
    int next_display;
    pthread_mutex_t reap_lock;
    /* GStreamer is initialised once per process, not per session. */
    int gst_ready;
};

struct mi_rtc {
    mi_rtc_pool *pool;
    /* The browser, and the X server it paints into. */
    pid_t chrome_pid;
    pid_t xvfb_pid;
    char display[32];
    char *profile;
    char *url;

    /* The CDP pipe: how input reaches the page. */
    int wfd; /* parent -> browser (the child's fd 3) */
    int rfd; /* browser -> parent (the child's fd 4) */
    char cdp_session[64];
    int cdp_next_id;
    int cdp_attached;
    int cdp_resp_id;
    int cdp_resp_ok;
    char *cdp_resp_json;
    unsigned char *rbuf;
    size_t rbuf_len, rbuf_cap;
    unsigned char *wbuf;
    size_t wbuf_len, wbuf_cap;

    /* The GStreamer side. */
    GstElement *pipeline;
    GstElement *webrtc;
    GMainContext *context;
    guint bus_watch;

    /* Negotiation. */
    char *offer;
    int offer_ready;
    int gathering_done;
    int answer_set;
    int connected;
    int failed;
    /* The input data channel. Vyzor owns the mapping from the console's
     * vocabulary to CDP, so there is nothing for a caller to hook. */
    GstWebRTCDataChannel *channel;
    int channel_ready;

    int width, height;
    double last_activity;
    int closed;
    char close_reason[200];
};

int mi_rtc_url_allowed(const char *url) {
    /* One set of URL rules for every path that fetches a page on the caller's
     * behalf: a render, a JPEG live session, and this. Duplicating them would
     * mean one could drift and quietly become the permissive one. */
    return mi_render_url_allowed(url);
}

void mi_rtc_config_init(mi_rtc_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->max_concurrent = 1;
    cfg->v8_heap_mb = 256;
    cfg->width = 1280;
    cfg->height = 800;
    /* 25 is what a viewer perceives as live, and every extra frame costs an
     * encode. ximagesrc will happily produce more than this. */
    cfg->fps = 25;
    /* VP8 at 1280x800 needs a few hundred kbit/s to look right. */
    cfg->bitrate_kbps = 2500;
    cfg->idle_timeout_seconds = 180;
    cfg->startup_timeout_seconds = 25;
    /* Gathering without STUN is instant; with one it waits for a reply. Three
     * seconds is generous for a reply and short enough not to stall a start. */
    cfg->gather_timeout_ms = 3000;
}

/* ------------------------------------------------------------------ */
/* Finding the tools                                                   */
/* ------------------------------------------------------------------ */

static int file_is_executable(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

static char *find_browser(const char *hint) {
    if (hint && *hint) return file_is_executable(hint) ? mi_strdup(hint) : NULL;
    for (size_t i = 0; BROWSERS[i]; i++) {
        if (mi_which(BROWSERS[i])) return mi_strdup(BROWSERS[i]);
    }
    const char *home = getenv("HOME");
    if (home) {
        const char *cands[] = {
            "/.cache/ms-playwright/chromium-1248/chrome-linux64/chrome",
            "/.cache/ms-playwright/chromium-1200/chrome-linux64/chrome", NULL};
        for (size_t i = 0; cands[i]; i++) {
            char *p = mi_asprintf("%s%s", home, cands[i]);
            if (p && file_is_executable(p)) return p;
            free(p);
        }
    }
    return NULL;
}

/*
 * Whether GStreamer has everything the pipeline needs.
 *
 * Checked by asking for the element rather than by looking for a library, because
 * a plugin can be installed and still fail to load — that is exactly how a missing
 * `libnice` presents: `webrtcbin` exists, but it cannot do ICE, and the failure
 * appears later as a null pad rather than as a missing package.
 */
static int gst_has_element(const char *name) {
    GstElementFactory *f = gst_element_factory_find(name);
    if (!f) return 0;
    gst_object_unref(f);
    return 1;
}

static int gst_stack_ready(void) {
    return gst_has_element("webrtcbin") && gst_has_element("ximagesrc") &&
           gst_has_element("vp8enc") && gst_has_element("rtpvp8pay") &&
           gst_has_element("videoconvert");
}

/* ------------------------------------------------------------------ */
/* The pool                                                            */
/* ------------------------------------------------------------------ */

mi_rtc_pool *mi_rtc_pool_new(const mi_rtc_config *cfg) {
    /* Chromium's helpers reparent when the browser dies; as a subreaper they come
     * here and one waitpid loop clears them, instead of piling up as zombies
     * behind a container's PID 1 that never reaps. */
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0) {
        mi_logf(MI_LOG_ERROR, "rtc: could not become a subreaper: %s", strerror(errno));
    }
    mi_rtc_pool *pool = mi_calloc(1, sizeof *pool);
    if (!pool) return NULL;
    pool->cfg = *cfg;
    if (pool->cfg.max_concurrent <= 0) pool->cfg.max_concurrent = 1;
    if (pool->cfg.width <= 0) pool->cfg.width = 1280;
    if (pool->cfg.height <= 0) pool->cfg.height = 800;
    if (pool->cfg.fps <= 0) pool->cfg.fps = 25;
    if (pool->cfg.bitrate_kbps <= 0) pool->cfg.bitrate_kbps = 2500;
    if (pool->cfg.startup_timeout_seconds <= 0) pool->cfg.startup_timeout_seconds = 25;
    if (pool->cfg.gather_timeout_ms <= 0) pool->cfg.gather_timeout_ms = 3000;
    pool->next_display = 90;

    pool->browser = find_browser(cfg->chromium_path);
    pool->xvfb = (cfg->xvfb_path && *cfg->xvfb_path)
                     ? (file_is_executable(cfg->xvfb_path) ? mi_strdup(cfg->xvfb_path) : NULL)
                     : (mi_which("Xvfb") ? mi_strdup("Xvfb") : NULL);

    if (!gst_is_initialized()) gst_init(NULL, NULL);
    pool->gst_ready = gst_stack_ready();

    if (!pool->browser) {
        mi_logf(MI_LOG_INFO, "rtc: no browser found; WebRTC sessions are unavailable");
    } else if (!pool->xvfb) {
        mi_logf(MI_LOG_INFO, "rtc: no Xvfb found; a raw capture needs a display");
    } else if (!pool->gst_ready) {
        mi_logf(MI_LOG_INFO,
                "rtc: GStreamer is missing webrtcbin/ximagesrc/vp8enc/rtpvp8pay; "
                "WebRTC sessions are unavailable");
    } else {
        mi_logf(MI_LOG_INFO, "rtc: ready (limit %d, %dx%d @ %d fps, %d kbit/s)",
                pool->cfg.max_concurrent, pool->cfg.width, pool->cfg.height, pool->cfg.fps,
                pool->cfg.bitrate_kbps);
    }

    pthread_mutex_init(&pool->lock, NULL);
    pthread_mutex_init(&pool->reap_lock, NULL);
    return pool;
}

void mi_rtc_pool_free(mi_rtc_pool *pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->lock);
    while (pool->in_use > 0) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    pthread_mutex_unlock(&pool->lock);
    free(pool->browser);
    free(pool->xvfb);
    pthread_mutex_destroy(&pool->lock);
    pthread_mutex_destroy(&pool->reap_lock);
    free(pool);
}

int mi_rtc_pool_available(const mi_rtc_pool *pool) {
    return pool && pool->browser && pool->xvfb && pool->gst_ready;
}

int mi_rtc_pool_limit(const mi_rtc_pool *pool) { return pool ? pool->cfg.max_concurrent : 0; }

int mi_rtc_pool_active(const mi_rtc_pool *pool) {
    if (!pool) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&pool->lock);
    int n = pool->in_use;
    pthread_mutex_unlock((pthread_mutex_t *)&pool->lock);
    return n;
}

static int pool_take(mi_rtc_pool *pool) {
    pthread_mutex_lock(&pool->lock);
    int ok = pool->in_use < pool->cfg.max_concurrent;
    if (ok) pool->in_use++;
    pthread_mutex_unlock(&pool->lock);
    return ok;
}

static void pool_release(mi_rtc_pool *pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->lock);
    if (pool->in_use > 0) pool->in_use--;
    pthread_mutex_unlock(&pool->lock);
}

static void reap_children(mi_rtc_pool *pool) {
    pthread_mutex_lock(&pool->reap_lock);
    for (;;) {
        int status = 0;
        if (waitpid(-1, &status, WNOHANG) <= 0) break;
    }
    pthread_mutex_unlock(&pool->reap_lock);
}

static double mono_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void set_close_reason(mi_rtc *rtc, const char *why) {
    snprintf(rtc->close_reason, sizeof rtc->close_reason, "%s", why);
    rtc->closed = 1;
}

/* ------------------------------------------------------------------ */
/* The CDP pipe (input)                                                */
/* ------------------------------------------------------------------ */

static void cdp_queue(mi_rtc *rtc, const char *json, size_t len) {
    if (rtc->closed) return;
    if (rtc->wbuf_len + len + 1 > CDP_WRITE_MAX) {
        set_close_reason(rtc, "the console sent more commands than the browser could take");
        return;
    }
    if (rtc->wbuf_len + len + 1 > rtc->wbuf_cap) {
        size_t cap = (rtc->wbuf_len + len + 1) * 2;
        unsigned char *grown = mi_realloc(rtc->wbuf, cap);
        if (!grown) return;
        rtc->wbuf = grown;
        rtc->wbuf_cap = cap;
    }
    memcpy(rtc->wbuf + rtc->wbuf_len, json, len);
    rtc->wbuf_len += len;
    rtc->wbuf[rtc->wbuf_len++] = '\0'; /* CDP frames messages with a NUL */
}

static void cdp_flush(mi_rtc *rtc) {
    while (!rtc->closed && rtc->wbuf_len > 0 && rtc->wfd >= 0) {
        ssize_t n = write(rtc->wfd, rtc->wbuf, rtc->wbuf_len);
        if (n > 0) {
            memmove(rtc->wbuf, rtc->wbuf + n, rtc->wbuf_len - (size_t)n);
            rtc->wbuf_len -= (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n < 0 && errno == EINTR) continue;
        set_close_reason(rtc, "the browser stopped reading");
        return;
    }
}

/* Send one command. `params` is a raw JSON object or NULL. Returns its id. */
static int cdp_command(mi_rtc *rtc, const char *method, const char *params) {
    if (rtc->closed) return 0;
    int id = ++rtc->cdp_next_id;
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"id\":");
    mi_json_int(&b, id);
    mi_json_raw(&b, ",\"method\":");
    mi_json_string(&b, method);
    if (rtc->cdp_session[0]) {
        mi_json_raw(&b, ",\"sessionId\":");
        mi_json_string(&b, rtc->cdp_session);
    }
    if (params) {
        mi_json_raw(&b, ",\"params\":");
        mi_json_raw(&b, params);
    }
    mi_json_raw(&b, "}");
    cdp_queue(rtc, mi_json_data(&b), b.len);
    mi_json_free(&b);
    cdp_flush(rtc);
    return id;
}

/* Drain the pipe, splitting on NUL, and remember the last response. */
static void cdp_read(mi_rtc *rtc) {
    if (rtc->rfd < 0) return;
    unsigned char chunk[16384];
    for (;;) {
        ssize_t n = read(rtc->rfd, chunk, sizeof chunk);
        if (n > 0) {
            if (rtc->rbuf_len + (size_t)n > rtc->rbuf_cap) {
                size_t cap = (rtc->rbuf_len + (size_t)n) * 2;
                unsigned char *grown = mi_realloc(rtc->rbuf, cap);
                if (!grown) return;
                rtc->rbuf = grown;
                rtc->rbuf_cap = cap;
            }
            memcpy(rtc->rbuf + rtc->rbuf_len, chunk, (size_t)n);
            rtc->rbuf_len += (size_t)n;
            continue;
        }
        if (n == 0) { set_close_reason(rtc, "the browser closed the connection"); return; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        set_close_reason(rtc, "the browser connection failed");
        return;
    }
    /* Split complete NUL-terminated messages out of the buffer. */
    size_t start = 0;
    for (size_t i = 0; i < rtc->rbuf_len; i++) {
        if (rtc->rbuf[i] != '\0') continue;
        const char *msg = (const char *)rtc->rbuf + start;
        mi_json *root = mi_json_parse(msg);
        if (root) {
            int64_t id = mi_json_get_int(root, "id", 0);
            if (id > 0) {
                rtc->cdp_resp_id = (int)id;
                const mi_json *err = mi_json_get(root, "error");
                rtc->cdp_resp_ok = err == NULL;
                free(rtc->cdp_resp_json);
                rtc->cdp_resp_json = mi_strdup(msg);
            }
            mi_json_value_free(root);
        }
        start = i + 1;
    }
    if (start > 0) {
        memmove(rtc->rbuf, rtc->rbuf + start, rtc->rbuf_len - start);
        rtc->rbuf_len -= start;
    }
}

/* Read until the response with `id` arrives, or the deadline passes.
 * Returns 1 on success, 0 on an error response, -1 on timeout. */
static int cdp_wait(mi_rtc *rtc, int id, int timeout_ms) {
    double deadline = mono_seconds() + timeout_ms / 1000.0;
    while (mono_seconds() < deadline && !rtc->closed) {
        cdp_read(rtc);
        if (rtc->cdp_resp_id == id) return rtc->cdp_resp_ok ? 1 : 0;
        struct timespec t = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000};
        nanosleep(&t, NULL);
    }
    return -1;
}

/* A string member of the last response's `result`. Allocated; free it. */
static char *cdp_result_string(mi_rtc *rtc, const char *key) {
    if (!rtc->cdp_resp_json) return NULL;
    mi_json *root = mi_json_parse(rtc->cdp_resp_json);
    if (!root) return NULL;
    const char *v = mi_json_get_string(mi_json_get(root, "result"), key, NULL);
    char *out = v ? mi_strdup(v) : NULL;
    mi_json_value_free(root);
    return out;
}

/* The page target's id, from a Target.getTargets result. */
static int cdp_page_target(mi_rtc *rtc, char *out, size_t outcap) {
    if (!rtc->cdp_resp_json) return -1;
    mi_json *root = mi_json_parse(rtc->cdp_resp_json);
    if (!root) return -1;
    const mi_json *infos = mi_json_get(mi_json_get(root, "result"), "targetInfos");
    int found = -1;
    if (infos && infos->type == MI_JSON_ARRAY) {
        for (size_t i = 0; i < infos->count; i++) {
            const mi_json *t = infos->items[i];
            const char *type = mi_json_get_string(t, "type", NULL);
            const char *tid = mi_json_get_string(t, "targetId", NULL);
            if (type && tid && !strcmp(type, "page")) {
                snprintf(out, outcap, "%s", tid);
                found = 0;
                break;
            }
        }
    }
    mi_json_value_free(root);
    return found;
}

/*
 * Attach to the page, once, and enable the domains input needs.
 *
 * Retried by the caller until the browser is up: the startup page does not exist
 * the instant the process is forked. Flattened, so commands carry a sessionId and
 * this page's events arrive on the same pipe without a target-routing dance.
 */
static int rtc_cdp_attach(mi_rtc *rtc) {
    if (rtc->cdp_attached) return 0;

    char target[64] = {0};
    int rid = cdp_command(rtc, "Target.getTargets", NULL);
    if (cdp_wait(rtc, rid, 500) < 0) return -1;
    if (cdp_page_target(rtc, target, sizeof target) != 0 || !target[0]) return -1;

    char params[256];
    snprintf(params, sizeof params, "{\"targetId\":\"%s\",\"flatten\":true}", target);
    rid = cdp_command(rtc, "Target.attachToTarget", params);
    if (cdp_wait(rtc, rid, 2000) != 1) return -1;
    char *sid = cdp_result_string(rtc, "sessionId");
    if (!sid || !*sid) { free(sid); return -1; }
    snprintf(rtc->cdp_session, sizeof rtc->cdp_session, "%s", sid);
    free(sid);

    /* Page for navigation, Runtime so a page's own scripts run. */
    rid = cdp_command(rtc, "Page.enable", NULL);
    cdp_wait(rtc, rid, 2000);
    rid = cdp_command(rtc, "Runtime.enable", NULL);
    cdp_wait(rtc, rid, 2000);

    /* The viewport, so the page lays itself out at the size being captured. */
    char vp[160];
    snprintf(vp, sizeof vp,
             "{\"width\":%d,\"height\":%d,\"deviceScaleFactor\":1,\"mobile\":false}",
             rtc->width, rtc->height);
    rid = cdp_command(rtc, "Emulation.setDeviceMetricsOverride", vp);
    cdp_wait(rtc, rid, 2000);

    /*
     * Focus emulation.
     *
     * A browser with no window manager reports the page as unfocused and a
     * clicked field never takes the caret, so typing goes nowhere. This tells the
     * page it is focused, which is what a real window would do.
     */
    rid = cdp_command(rtc, "Emulation.setFocusEmulationEnabled", "{\"enabled\":true}");
    cdp_wait(rtc, rid, 2000);

    rtc->cdp_attached = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Input dispatch (console vocabulary -> CDP)                          */
/* ------------------------------------------------------------------ */

static void rtc_input(mi_rtc *rtc, const mi_json *m) {
    const char *type = mi_json_get_string(m, "type", "");
    if (!type || !*type) return;

    if (!strcmp(type, "navigate")) {
        const char *url = mi_json_get_string(m, "url", NULL);
        /* A navigation is a fetch on the caller's behalf, so it obeys the same
         * rules as opening one. */
        if (!url || !mi_rtc_url_allowed(url)) return;
        mi_json_buf b;
        mi_json_init(&b);
        mi_json_raw(&b, "{\"url\":");
        mi_json_string(&b, url);
        mi_json_raw(&b, "}");
        cdp_command(rtc, "Page.navigate", mi_json_data(&b));
        mi_json_free(&b);
        free(rtc->url);
        rtc->url = mi_strdup(url);
    } else if (!strcmp(type, "reload")) {
        cdp_command(rtc, "Page.reload", NULL);
    } else if (!strcmp(type, "resize")) {
        int w = (int)mi_json_get_number(m, "width", rtc->width);
        int h = (int)mi_json_get_number(m, "height", rtc->height);
        if (w < 64) w = 64;
        if (h < 64) h = 64;
        if (w > 4096) w = 4096;
        if (h > 4096) h = 4096;
        char params[160];
        snprintf(params, sizeof params,
                 "{\"width\":%d,\"height\":%d,\"deviceScaleFactor\":1,\"mobile\":false}", w, h);
        cdp_command(rtc, "Emulation.setDeviceMetricsOverride", params);
    } else if (!strcmp(type, "mouse") || !strcmp(type, "wheel")) {
        int wheel = !strcmp(type, "wheel");
        mi_json_buf b;
        mi_json_init(&b);
        mi_json_raw(&b, "{\"type\":");
        mi_json_string(&b, wheel ? "mouseWheel" : mi_json_get_string(m, "event", "mouseMoved"));
        mi_json_raw(&b, ",\"x\":");
        mi_json_double(&b, mi_json_get_number(m, "x", 0));
        mi_json_raw(&b, ",\"y\":");
        mi_json_double(&b, mi_json_get_number(m, "y", 0));
        if (wheel) {
            mi_json_raw(&b, ",\"deltaX\":");
            mi_json_double(&b, mi_json_get_number(m, "deltaX", 0));
            mi_json_raw(&b, ",\"deltaY\":");
            mi_json_double(&b, mi_json_get_number(m, "deltaY", 0));
        } else {
            const char *button = mi_json_get_string(m, "button", NULL);
            if (button && *button) {
                mi_json_raw(&b, ",\"button\":");
                mi_json_string(&b, button);
            }
            mi_json_raw(&b, ",\"clickCount\":");
            mi_json_int(&b, mi_json_get_int(m, "clickCount", 0));
            mi_json_raw(&b, ",\"buttons\":");
            mi_json_int(&b, mi_json_get_int(m, "buttons", 0));
        }
        mi_json_raw(&b, ",\"modifiers\":");
        mi_json_int(&b, mi_json_get_int(m, "modifiers", 0));
        mi_json_raw(&b, "}");
        cdp_command(rtc, "Input.dispatchMouseEvent", mi_json_data(&b));
        mi_json_free(&b);
    } else if (!strcmp(type, "key")) {
        mi_json_buf b;
        mi_json_init(&b);
        mi_json_raw(&b, "{\"type\":");
        mi_json_string(&b, mi_json_get_string(m, "event", "keyDown"));
        const char *key = mi_json_get_string(m, "key", NULL);
        const char *code = mi_json_get_string(m, "code", NULL);
        const char *text = mi_json_get_string(m, "text", NULL);
        if (key) { mi_json_raw(&b, ",\"key\":"); mi_json_string(&b, key); }
        if (code) { mi_json_raw(&b, ",\"code\":"); mi_json_string(&b, code); }
        int vk = (int)mi_json_get_int(m, "vk", 0);
        if (vk > 0) { mi_json_raw(&b, ",\"windowsVirtualKeyCode\":"); mi_json_int(&b, vk); }
        if (text) { mi_json_raw(&b, ",\"text\":"); mi_json_string(&b, text); }
        mi_json_raw(&b, ",\"modifiers\":");
        mi_json_int(&b, mi_json_get_int(m, "modifiers", 0));
        mi_json_raw(&b, "}");
        cdp_command(rtc, "Input.dispatchKeyEvent", mi_json_data(&b));
        mi_json_free(&b);
    } else if (!strcmp(type, "text")) {
        const char *text = mi_json_get_string(m, "text", NULL);
        if (!text) return;
        mi_json_buf b;
        mi_json_init(&b);
        mi_json_raw(&b, "{\"text\":");
        mi_json_string(&b, text);
        mi_json_raw(&b, "}");
        cdp_command(rtc, "Input.insertText", mi_json_data(&b));
        mi_json_free(&b);
    } else if (!strcmp(type, "touch")) {
        const mi_json *pts = mi_json_get(m, "points");
        if (!pts || pts->type != MI_JSON_ARRAY) return;
        mi_json_buf b;
        mi_json_init(&b);
        mi_json_raw(&b, "{\"type\":");
        mi_json_string(&b, mi_json_get_string(m, "event", "touchStart"));
        mi_json_raw(&b, ",\"touchPoints\":[");
        int count = 0;
        for (size_t i = 0; i < pts->count; i++) {
            const mi_json *p = pts->items[i];
            if (!p || p->type != MI_JSON_OBJECT) continue;
            if (count) mi_json_raw(&b, ",");
            mi_json_raw(&b, "{\"x\":");
            mi_json_double(&b, mi_json_get_number(p, "x", 0));
            mi_json_raw(&b, ",\"y\":");
            mi_json_double(&b, mi_json_get_number(p, "y", 0));
            mi_json_raw(&b, ",\"id\":");
            mi_json_int(&b, count);
            mi_json_raw(&b, "}");
            count++;
        }
        mi_json_raw(&b, "],\"modifiers\":");
        mi_json_int(&b, mi_json_get_int(m, "modifiers", 0));
        mi_json_raw(&b, "}");
        if (count > 0) cdp_command(rtc, "Input.dispatchTouchEvent", mi_json_data(&b));
        mi_json_free(&b);
    }
}

/* ------------------------------------------------------------------ */
/* The input data channel                                              */
/* ------------------------------------------------------------------ */

static void on_channel_message(GstWebRTCDataChannel *channel, gchar *data, gpointer user_data) {
    mi_rtc *rtc = user_data;
    (void)channel;
    if (!rtc || !data) return;
    rtc->last_activity = mono_seconds();
    /* The console speaks its own vocabulary, not CDP; the mapping is above. */
    mi_json *m = mi_json_parse(data);
    if (m) {
        rtc_input(rtc, m);
        mi_json_value_free(m);
    }
}

static void on_channel_open(GstWebRTCDataChannel *channel, gpointer user_data) {
    mi_rtc *rtc = user_data;
    (void)channel;
    if (!rtc) return;
    rtc->channel_ready = 1;
    rtc->last_activity = mono_seconds();
    mi_logf(MI_LOG_INFO, "rtc: input channel open");
}

static void on_channel_close(GstWebRTCDataChannel *channel, gpointer user_data) {
    mi_rtc *rtc = user_data;
    (void)channel;
    if (!rtc) return;
    rtc->channel_ready = 0;
    rtc->last_activity = mono_seconds();
}

static void attach_channel(mi_rtc *rtc, GstWebRTCDataChannel *channel) {
    if (rtc->channel && rtc->channel != channel) gst_object_unref(rtc->channel);
    rtc->channel = g_object_ref(channel);
    g_signal_connect(channel, "on-message-string", G_CALLBACK(on_channel_message), rtc);
    g_signal_connect(channel, "on-open", G_CALLBACK(on_channel_open), rtc);
    g_signal_connect(channel, "on-close", G_CALLBACK(on_channel_close), rtc);
    gboolean ready = FALSE;
    g_object_get(channel, "ready-state", &ready, NULL);
    if (ready) rtc->channel_ready = 1;
}

/* The browser opened a channel on the peer connection. */
static void on_data_channel(GstElement *webrtc, GstWebRTCDataChannel *channel,
                            gpointer user_data) {
    mi_rtc *rtc = user_data;
    (void)webrtc;
    if (!rtc || !channel) return;
    gchar *label = NULL;
    g_object_get(channel, "label", &label, NULL);
    /* Only the input channel is ours; a browser may open others. */
    if (label && !strcmp(label, INPUT_LABEL)) attach_channel(rtc, channel);
    g_free(label);
}

static int mi_rtc_channel_open(mi_rtc *rtc) {
    if (!rtc || !rtc->webrtc) return -1;
    if (rtc->channel) return 0;
    GstWebRTCDataChannel *ch = NULL;
    g_signal_emit_by_name(rtc->webrtc, "create-data-channel", INPUT_LABEL, NULL, &ch);
    if (!ch) return -1;
    attach_channel(rtc, ch);
    gst_object_unref(ch); /* attach_channel holds its own reference */
    return 0;
}

/* ------------------------------------------------------------------ */
/* GStreamer callbacks                                                 */
/* ------------------------------------------------------------------ */

/* A local ICE candidate.
 *
 * Nothing needs to send these: gathering is awaited before the offer is handed
 * over, so they are already in the SDP. All this does is note that the session is
 * alive, which is what keeps a gathering browser from being reaped as idle.
 */
static void on_ice_candidate(GstElement *webrtc, guint mline_index, gchar *candidate,
                             gpointer user_data) {
    mi_rtc *rtc = user_data;
    (void)webrtc; (void)mline_index; (void)candidate;
    rtc->last_activity = mono_seconds();
}

/*
 * The gathering state, which is what makes a socket-free handshake possible.
 *
 * `COMPLETE` means every candidate this host can offer is already in the SDP, so
 * the offer is final and can be sent as one document with no follow-up messages.
 *
 * A *property*, not a signal: there is no `on-ice-gathering-state-changed` to
 * connect to, and doing so logs a GLib critical. `notify::` is how a property
 * change is watched.
 */
static void on_gathering_state(GObject *object, GParamSpec *pspec, gpointer user_data) {
    mi_rtc *rtc = user_data;
    (void)pspec;
    GstWebRTCICEGatheringState st = GST_WEBRTC_ICE_GATHERING_STATE_NEW;
    g_object_get(object, "ice-gathering-state", &st, NULL);
    if (st == GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) rtc->gathering_done = 1;
    rtc->last_activity = mono_seconds();
}

/* The connection state, which is how "media is flowing" is known. Also a
 * property, for the same reason. */
static void on_connection_state(GObject *object, GParamSpec *pspec, gpointer user_data) {
    mi_rtc *rtc = user_data;
    (void)pspec;
    GstWebRTCPeerConnectionState state = GST_WEBRTC_PEER_CONNECTION_STATE_NEW;
    g_object_get(object, "connection-state", &state, NULL);
    rtc->last_activity = mono_seconds();
    if (state == GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED) {
        rtc->connected = 1;
        /* The connection is up, so the channel can be opened if the browser has
         * not already made it. */
        if (!rtc->channel) mi_rtc_channel_open(rtc);
        mi_logf(MI_LOG_INFO, "rtc: connected (%s)", rtc->url);
    } else if (state == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED) {
        set_close_reason(rtc, "the WebRTC connection failed");
    } else if (state == GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED) {
        set_close_reason(rtc, "the WebRTC connection closed");
    }
}

static void on_offer_created(GstPromise *promise, gpointer user_data) {
    mi_rtc *rtc = user_data;
    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *offer = NULL;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref(promise);
    if (!offer) {
        set_close_reason(rtc, "the pipeline produced no offer");
        return;
    }
    /* The local description has to be set for ICE to start gathering. */
    g_signal_emit_by_name(rtc->webrtc, "set-local-description", offer, NULL);
    gchar *text = gst_sdp_message_as_text(offer->sdp);
    free(rtc->offer);
    rtc->offer = mi_strdup(text);
    g_free(text);
    gst_webrtc_session_description_free(offer);
    rtc->offer_ready = 1;
}

static void on_negotiation_needed(GstElement *webrtc, gpointer user_data) {
    mi_rtc *rtc = user_data;
    (void)webrtc;
    GstPromise *promise = gst_promise_new_with_change_func(on_offer_created, rtc, NULL);
    g_signal_emit_by_name(rtc->webrtc, "create-offer", NULL, promise);
}

static gboolean on_bus_message(GstBus *bus, GstMessage *msg, gpointer user_data) {
    mi_rtc *rtc = user_data;
    (void)bus;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *debug = NULL;
            gst_message_parse_error(msg, &err, &debug);
            mi_logf(MI_LOG_ERROR, "rtc: pipeline error: %s", err ? err->message : "unknown");
            if (err) g_error_free(err);
            g_free(debug);
            set_close_reason(rtc, "the capture pipeline failed");
            break;
        }
        case GST_MESSAGE_EOS:
            set_close_reason(rtc, "the capture pipeline ended");
            break;
        case GST_MESSAGE_WARNING: {
            GError *err = NULL;
            gchar *debug = NULL;
            gst_message_parse_warning(msg, &err, &debug);
            mi_logf(MI_LOG_INFO, "rtc: pipeline warning: %s", err ? err->message : "unknown");
            if (err) g_error_free(err);
            g_free(debug);
            break;
        }
        default:
            break;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Spawning the display and the browser                                */
/* ------------------------------------------------------------------ */

static void close_extra_fds(void) {
    long max = 1024;
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
        max = (long)rl.rlim_cur;
    if (max > 65536) max = 65536;
    for (long fd = 5; fd < max; fd++) close((int)fd);
}

/*
 * Clear a lock whose display has no owner.
 *
 * A session killed with SIGKILL leaves `/tmp/.X<n>-lock` behind, and Xvfb then
 * refuses that display. Since the display number comes from a counter that
 * restarts at the same value each run, the first session after a restart can
 * land on a stale lock and fail to start at all.
 *
 * X writes its pid into the lock file, so this is exact: the lock is removed
 * only when nothing is listening on the display it guards. A lock held by a
 * live server is left alone.
 */
static void clear_stale_display_lock(const char *display) {
    if (!display || display[0] != ':' || !display[1]) return;
    int n = atoi(display + 1);
    if (n <= 0) return;

    char lock[64];
    snprintf(lock, sizeof lock, "/tmp/.X%d-lock", n);
    FILE *f = fopen(lock, "r");
    if (!f) return; /* no lock, nothing to do */
    long pid = 0;
    if (fscanf(f, "%ld", &pid) != 1) pid = 0;
    fclose(f);

    /*
     * A lock whose owner is still alive is a real display; leave it.
     *
     * `kill(pid, 0)` is not enough on its own: a *zombie* — a process that has
     * exited but not been reaped — accepts the signal, so a killed Xvfb whose
     * parent has not waited on it still looks alive. `/proc/<pid>/stat` carries
     * the state, and 'Z' means gone.
     */
    if (pid > 0 && (kill((pid_t)pid, 0) == 0 || errno == EPERM)) {
        int is_zombie = 0;
        char statpath[64];
        snprintf(statpath, sizeof statpath, "/proc/%ld/stat", pid);
        FILE *sf = fopen(statpath, "r");
        if (sf) {
            char line[512];
            if (fgets(line, sizeof line, sf)) {
                /* The state is the field after the (comm) column. */
                char *close = strrchr(line, ')');
                if (close && close[1] == ' ' && close[2] == 'Z') is_zombie = 1;
            }
            fclose(sf);
        }
        if (!is_zombie) return;
    }

    mi_logf(MI_LOG_INFO, "rtc: clearing a stale display lock for %s (pid %ld is gone)",
            display, pid);
    unlink(lock);
    char sock[64];
    snprintf(sock, sizeof sock, "/tmp/.X11-unix/X%d", n);
    unlink(sock);
}

/* A virtual display for the browser to paint into. */
static pid_t spawn_xvfb(const char *xvfb, const char *display, int w, int h) {
    clear_stale_display_lock(display);

    char screen[32];
    snprintf(screen, sizeof screen, "%dx%dx24", w, h);

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close_extra_fds();
        setpgid(0, 0);
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }
        execlp(xvfb, xvfb, display, "-screen", "0", screen, "-nolisten", "tcp", (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid);
    return pid;
}

/*
 * Launch Chromium on the virtual display, with the CDP pipe for input.
 *
 * `--use-gl=swiftshader` is required because the display has no GPU; without a
 * software GL path Chromium paints nothing, and `ximagesrc` would read a blank
 * screen.
 *
 * `--remote-debugging-pipe` is how input is driven: the browser reads fd 3 and
 * writes fd 4, NUL-terminated JSON. X is for pixels, the pipe for input.
 */
static pid_t spawn_chrome(const mi_rtc_config *cfg, const char *browser, const char *profile,
                          const char *display, const char *url, int *wfd_out, int *rfd_out) {
    int p2c[2], c2p[2];
    if (pipe(p2c) != 0 || pipe(c2p) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]);
        return -1;
    }
    if (pid == 0) {
        /* Relocate to high fds first so the dup2 targets cannot collide with a
         * source fd — fds 3 and 4 are exactly what the pipes were given. */
        int hr = fcntl(p2c[0], F_DUPFD, 100);
        int hw = fcntl(c2p[1], F_DUPFD, 101);
        close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]);
        if (hr < 0 || hw < 0) _exit(120);
        if (dup2(hr, CDP_READ_FD) < 0) _exit(121);
        if (dup2(hw, CDP_WRITE_FD) < 0) _exit(122);
        if (hr != CDP_READ_FD && hr != CDP_WRITE_FD) close(hr);
        if (hw != CDP_READ_FD && hw != CDP_WRITE_FD) close(hw);
        close_extra_fds();
        setpgid(0, 0);
        setenv("DISPLAY", display, 1);

        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }

        char *jsflags = mi_asprintf("--js-flags=--max-old-space-size=%d", cfg->v8_heap_mb);
        char *udd = mi_asprintf("--user-data-dir=%s", profile);
        char *win = mi_asprintf("--window-size=%d,%d", cfg->width, cfg->height);
        char *ua = (cfg->user_agent && *cfg->user_agent)
                       ? mi_asprintf("--user-agent=%s", cfg->user_agent) : NULL;
        const char *av[48];
        int n = 0;
        av[n++] = browser;
        /* No --headless: the pixels have to reach X. */
        av[n++] = "--use-gl=swiftshader";
        av[n++] = "--disable-dev-shm-usage";
        av[n++] = "--no-sandbox";
        av[n++] = "--no-first-run";
        av[n++] = "--no-default-browser-check";
        av[n++] = "--disable-background-networking";
        av[n++] = "--disable-sync";
        av[n++] = "--disable-translate";
        av[n++] = "--disable-extensions";
        av[n++] = "--disable-default-apps";
        av[n++] = "--mute-audio";
        av[n++] = "--hide-scrollbars";
        av[n++] = "--disable-features=Translate,BackForwardCache,AcceptCHFrame";
        av[n++] = "--disable-backgrounding-occluded-windows";
        av[n++] = "--disable-renderer-backgrounding";
        av[n++] = "--disable-background-timer-throttling";
        /* The window is placed at the display origin so the capture rectangle and
         * the page rectangle are the same thing. */
        av[n++] = "--window-position=0,0";
        av[n++] = "--start-maximized";
        av[n++] = "--remote-debugging-pipe";
        if (ua) av[n++] = ua;
        if (jsflags) av[n++] = jsflags;
        if (udd) av[n++] = udd;
        if (win) av[n++] = win;
        av[n++] = (url && *url) ? url : "about:blank";
        av[n++] = NULL;
        execvp(av[0], (char *const *)av);
        _exit(127);
    }

    close(p2c[0]);
    close(c2p[1]);
    setpgid(pid, pid);
    *wfd_out = p2c[1];
    *rfd_out = c2p[0];
    return pid;
}

/* ------------------------------------------------------------------ */
/* The pipeline                                                        */
/* ------------------------------------------------------------------ */

static int build_pipeline(mi_rtc *rtc) {
    const mi_rtc_config *cfg = &rtc->pool->cfg;
    GError *gerr = NULL;
    char *desc = mi_asprintf(
        "ximagesrc display-name=%s use-damage=0 show-pointer=0 ! "
        "video/x-raw,framerate=%d/1 ! "
        "videoconvert ! "
        "video/x-raw,format=I420 ! "
        "vp8enc deadline=1 cpu-used=4 keyframe-max-dist=%d "
        "target-bitrate=%d error-resilient=1 ! "
        "rtpvp8pay pt=96 picture-id-mode=15-bit ! "
        "application/x-rtp,media=video,encoding-name=VP8,payload=96,clock-rate=90000 ! "
        "webrtcbin name=sendrecv bundle-policy=max-bundle",
        rtc->display, cfg->fps, cfg->fps * 2, cfg->bitrate_kbps * 1000);
    if (!desc) return -1;

    rtc->pipeline = gst_parse_launch(desc, &gerr);
    free(desc);
    if (!rtc->pipeline || gerr) {
        mi_logf(MI_LOG_ERROR, "rtc: pipeline could not be built: %s",
                gerr ? gerr->message : "unknown");
        if (gerr) g_error_free(gerr);
        return -1;
    }

    rtc->webrtc = gst_bin_get_by_name(GST_BIN(rtc->pipeline), "sendrecv");
    if (!rtc->webrtc) {
        mi_logf(MI_LOG_ERROR, "rtc: the pipeline has no webrtcbin");
        return -1;
    }

    if (cfg->stun_server && *cfg->stun_server) {
        g_object_set(rtc->webrtc, "stun-server", cfg->stun_server, NULL);
    }

    g_signal_connect(rtc->webrtc, "on-negotiation-needed",
                     G_CALLBACK(on_negotiation_needed), rtc);
    g_signal_connect(rtc->webrtc, "on-ice-candidate", G_CALLBACK(on_ice_candidate), rtc);
    g_signal_connect(rtc->webrtc, "notify::connection-state",
                     G_CALLBACK(on_connection_state), rtc);
    g_signal_connect(rtc->webrtc, "notify::ice-gathering-state",
                     G_CALLBACK(on_gathering_state), rtc);
    /* The input channel arrives here, whichever end opened it. */
    g_signal_connect(rtc->webrtc, "on-data-channel", G_CALLBACK(on_data_channel), rtc);

    GstBus *bus = gst_element_get_bus(rtc->pipeline);
    gst_bus_add_watch_full(bus, G_PRIORITY_DEFAULT, on_bus_message, rtc, NULL);
    gst_object_unref(bus);

    if (gst_element_set_state(rtc->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        mi_logf(MI_LOG_ERROR, "rtc: the pipeline would not start");
        return -1;
    }
    return 0;
}

/* Pump the main context, which is what delivers GStreamer's signals. */
static void pump_context(mi_rtc *rtc, int ms) {
    if (!rtc->context) return;
    double deadline = mono_seconds() + ms / 1000.0;
    for (;;) {
        while (g_main_context_pending(rtc->context)) {
            g_main_context_iteration(rtc->context, FALSE);
        }
        if (mono_seconds() >= deadline) break;
        g_main_context_iteration(rtc->context, FALSE);
        struct timespec t = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000};
        nanosleep(&t, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* Opening a session                                                   */
/* ------------------------------------------------------------------ */

static char *make_profile_dir(const mi_rtc_config *cfg, const char *id) {
    const char *root = (cfg->profile_root && *cfg->profile_root) ? cfg->profile_root : "/tmp";
    char *dir = mi_asprintf("%s/vyzorix-rtc-%s", root, id);
    if (!dir) return NULL;
    mi_rm_rf(dir);
    if (mi_mkdir_p(dir) != 0) { free(dir); return NULL; }
    return dir;
}

mi_rtc_status mi_rtc_open(mi_rtc_pool *pool, const char *url, mi_rtc **out,
                          char *err, size_t errcap) {
    *out = NULL;
    if (err && errcap) err[0] = '\0';
    if (!pool) {
        if (err) snprintf(err, errcap, "no session pool");
        return MI_RTC_FAILED;
    }
    if (!mi_rtc_url_allowed(url)) {
        if (err) snprintf(err, errcap, "only public http(s) pages can be opened");
        return MI_RTC_BAD_URL;
    }
    if (!pool->browser || !pool->xvfb || !pool->gst_ready) {
        if (err) snprintf(err, errcap,
                          "WebRTC needs a browser, Xvfb and the GStreamer webrtc plugins");
        return MI_RTC_NO_PLUGIN;
    }
    if (!pool_take(pool)) {
        if (err) snprintf(err, errcap, "too many live sessions");
        return MI_RTC_BUSY;
    }

    mi_rtc *rtc = mi_calloc(1, sizeof *rtc);
    if (!rtc) {
        pool_release(pool);
        if (err) snprintf(err, errcap, "out of memory");
        return MI_RTC_FAILED;
    }
    rtc->pool = pool;
    rtc->width = pool->cfg.width;
    rtc->height = pool->cfg.height;
    rtc->wfd = -1;
    rtc->rfd = -1;
    rtc->url = mi_strdup(url);
    rtc->last_activity = mono_seconds();

    char id[64];
    snprintf(id, sizeof id, "%d-%ld", (int)getpid(), (long)time(NULL));
    rtc->profile = make_profile_dir(&pool->cfg, id);
    if (!rtc->profile) {
        if (err) snprintf(err, errcap, "could not create a browser profile");
        goto fail;
    }

    /* A display of its own, so two sessions cannot collide. */
    if (pool->cfg.display && *pool->cfg.display) {
        snprintf(rtc->display, sizeof rtc->display, "%s", pool->cfg.display);
    } else {
        pthread_mutex_lock(&pool->lock);
        int num = pool->next_display++;
        pthread_mutex_unlock(&pool->lock);
        snprintf(rtc->display, sizeof rtc->display, ":%d", num);
    }

    rtc->xvfb_pid = spawn_xvfb(pool->xvfb, rtc->display, rtc->width, rtc->height);
    if (rtc->xvfb_pid < 0) {
        if (err) snprintf(err, errcap, "could not start the display");
        goto fail;
    }
    /* The display needs a moment before anything can connect to it. */
    {
        struct timespec t = {.tv_sec = 0, .tv_nsec = 400 * 1000 * 1000};
        nanosleep(&t, NULL);
    }

    rtc->chrome_pid = spawn_chrome(&pool->cfg, pool->browser, rtc->profile, rtc->display, url,
                                   &rtc->wfd, &rtc->rfd);
    if (rtc->chrome_pid < 0) {
        if (err) snprintf(err, errcap, "could not start the browser");
        goto fail;
    }
    /* Non-blocking both ways: a stalled browser must not block Vyzor. */
    int fl = fcntl(rtc->wfd, F_GETFL, 0);
    fcntl(rtc->wfd, F_SETFL, fl | O_NONBLOCK);
    fl = fcntl(rtc->rfd, F_GETFL, 0);
    fcntl(rtc->rfd, F_SETFL, fl | O_NONBLOCK);

    /*
     * A private main context for this session's pipeline.
     *
     * Per session, not global: two sessions must not service each other's
     * signals, and `mi_rtc_pump` runs one context at a time.
     */
    rtc->context = g_main_context_new();
    g_main_context_push_thread_default(rtc->context);

    if (build_pipeline(rtc) != 0) {
        if (err) snprintf(err, errcap, "the capture pipeline could not be started");
        goto fail;
    }

    /* Return at once; the offer is awaited separately, because startup is not
     * instant and the HTTP layer should not hold a request open for all of it. */
    *out = rtc;
    return MI_RTC_OK;

fail:
    mi_rtc_close(rtc);
    return MI_RTC_FAILED;
}

static void refresh_offer(mi_rtc *rtc);

int mi_rtc_wait_offer(mi_rtc *rtc, int timeout_ms) {
    if (!rtc) return -1;
    if (timeout_ms <= 0) timeout_ms = rtc->pool->cfg.startup_timeout_seconds * 1000;
    double deadline = mono_seconds() + timeout_ms / 1000.0;

    g_main_context_push_thread_default(rtc->context);

    /* Attach CDP while the browser is still coming up. Retried, because the
     * startup page does not exist the instant the process is forked. */
    while (!rtc->cdp_attached && mono_seconds() < deadline && !rtc->closed) {
        pump_context(rtc, 100);
        if (rtc_cdp_attach(rtc) == 0) break;
    }

    /* Then wait for the offer, and for gathering so its candidates are in it. */
    double gather_deadline = 0;
    while (mono_seconds() < deadline && !rtc->closed) {
        pump_context(rtc, 100);
        if (!rtc->offer_ready) continue;
        if (rtc->gathering_done) break;
        if (gather_deadline == 0)
            gather_deadline = mono_seconds() + rtc->pool->cfg.gather_timeout_ms / 1000.0;
        if (mono_seconds() >= gather_deadline) break;
    }

    /* The offer text has to be re-read now that gathering has put the
     * candidates into the local description. */
    if (rtc->offer_ready && !rtc->closed) refresh_offer(rtc);

    g_main_context_pop_thread_default(rtc->context);
    return (rtc->offer_ready && !rtc->closed) ? 0 : -1;
}

char *mi_rtc_offer(mi_rtc *rtc) {
    if (!rtc || !rtc->offer) return NULL;
    return mi_strdup(rtc->offer);
}

int mi_rtc_gathering_done(mi_rtc *rtc) { return rtc && rtc->gathering_done; }

/*
 * Re-read the local description and re-serialize it.
 *
 * `on_offer_created` captures the SDP the moment the offer is made, which is
 * *before* ICE has gathered — so the text it stored has no candidates in it.
 * Since there is no signalling socket to trickle them on, the offer has to carry
 * them, and that means taking a fresh copy once gathering has finished rather
 * than reusing the one from the signal.
 */
static void refresh_offer(mi_rtc *rtc) {
    if (!rtc || !rtc->webrtc) return;
    GstWebRTCSessionDescription *local = NULL;
    g_object_get(rtc->webrtc, "local-description", &local, NULL);
    if (!local || !local->sdp) {
        if (local) gst_webrtc_session_description_free(local);
        return;
    }
    gchar *text = gst_sdp_message_as_text(local->sdp);
    if (text) {
        free(rtc->offer);
        rtc->offer = mi_strdup(text);
        g_free(text);
    }
    gst_webrtc_session_description_free(local);
}

int mi_rtc_set_answer(mi_rtc *rtc, const char *sdp) {
    if (!rtc || !rtc->webrtc || !sdp) return -1;
    GstSDPMessage *msg = NULL;
    if (gst_sdp_message_new(&msg) != GST_SDP_OK) return -1;
    if (gst_sdp_message_parse_buffer((const guint8 *)sdp, strlen(sdp), msg) != GST_SDP_OK) {
        gst_sdp_message_free(msg);
        return -1;
    }
    GstWebRTCSessionDescription *answer =
        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, msg);
    if (!answer) return -1;
    g_signal_emit_by_name(rtc->webrtc, "set-remote-description", answer, NULL);
    gst_webrtc_session_description_free(answer);
    rtc->answer_set = 1;
    rtc->last_activity = mono_seconds();
    return 0;
}

int mi_rtc_pump(mi_rtc *rtc, int ms) {
    if (!rtc) return -1;
    if (rtc->closed) return -1;

    g_main_context_push_thread_default(rtc->context);
    pump_context(rtc, ms);
    g_main_context_pop_thread_default(rtc->context);

    /* Drain any CDP the browser has sent, so its responses do not pile up. */
    cdp_read(rtc);
    cdp_flush(rtc);

    /* Age the session: a browser left open is a leak the size of a browser. */
    if (rtc->pool && rtc->pool->cfg.idle_timeout_seconds > 0 &&
        mono_seconds() - rtc->last_activity > rtc->pool->cfg.idle_timeout_seconds) {
        set_close_reason(rtc, "the session was idle for too long");
    }
    return rtc->closed ? -1 : 0;
}

const char *mi_rtc_state(mi_rtc *rtc) {
    if (!rtc || !rtc->webrtc) return "closed";
    GstWebRTCPeerConnectionState st = GST_WEBRTC_PEER_CONNECTION_STATE_NEW;
    g_object_get(rtc->webrtc, "connection-state", &st, NULL);
    switch (st) {
        case GST_WEBRTC_PEER_CONNECTION_STATE_NEW: return "new";
        case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTING: return "connecting";
        case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED: return "connected";
        case GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED: return "disconnected";
        case GST_WEBRTC_PEER_CONNECTION_STATE_FAILED: return "failed";
        case GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED: return "closed";
    }
    return "unknown";
}

int mi_rtc_connected(mi_rtc *rtc) { return rtc && rtc->connected; }

const char *mi_rtc_close_reason(mi_rtc *rtc) {
    if (!rtc) return "no session";
    if (rtc->close_reason[0]) return rtc->close_reason;
    return rtc->closed ? "the session closed" : "";
}

void mi_rtc_viewport(mi_rtc *rtc, int *width, int *height) {
    if (width) *width = rtc ? rtc->width : 0;
    if (height) *height = rtc ? rtc->height : 0;
}

/* ------------------------------------------------------------------ */
/* Closing                                                             */
/* ------------------------------------------------------------------ */

static void kill_group(pid_t pid) {
    if (pid <= 0) return;
    kill(-pid, SIGTERM);
    struct timespec brief = {.tv_sec = 0, .tv_nsec = 300 * 1000 * 1000};
    nanosleep(&brief, NULL);
    kill(-pid, SIGKILL);
    int status = 0;
    for (int i = 0; i < 100; i++) {
        pid_t got = waitpid(pid, &status, WNOHANG);
        if (got == pid || (got < 0 && errno != EINTR)) break;
        nanosleep(&brief, NULL);
    }
}

void mi_rtc_close(mi_rtc *rtc) {
    if (!rtc) return;
    /* Stop the pipeline before the display goes away, or GStreamer reports an
     * error against a dead X connection. */
    if (rtc->pipeline) gst_element_set_state(rtc->pipeline, GST_STATE_NULL);
    if (rtc->channel) gst_object_unref(rtc->channel);
    if (rtc->webrtc) gst_object_unref(rtc->webrtc);
    if (rtc->pipeline) gst_object_unref(rtc->pipeline);
    if (rtc->context) g_main_context_unref(rtc->context);

    kill_group(rtc->chrome_pid);
    kill_group(rtc->xvfb_pid);
    if (rtc->wfd >= 0) close(rtc->wfd);
    if (rtc->rfd >= 0) close(rtc->rfd);
    if (rtc->profile) mi_rm_rf(rtc->profile);
    free(rtc->profile);
    free(rtc->url);
    free(rtc->offer);
    free(rtc->rbuf);
    free(rtc->wbuf);
    free(rtc->cdp_resp_json);
    if (rtc->pool) {
        reap_children(rtc->pool);
        pool_release(rtc->pool);
    }
    free(rtc);
}
