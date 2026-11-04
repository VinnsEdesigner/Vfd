/* mi_rtc.c - a live page as raw video over WebRTC.
 *
 * The pipeline is built here and driven by a GLib main loop that `mi_rtc_pump`
 * runs. That is the shape GStreamer wants: `webrtcbin` signals (negotiation
 * needed, ICE candidate) are delivered on the main context, so a session that is
 * not pumped does not negotiate and does not send.
 *
 * Chromium runs on a real X display rather than headless, because headless
 * composites internally and never touches X — so there would be nothing for
 * `ximagesrc` to read. It is launched with `--use-gl=swiftshader` because the
 * virtual display has no GPU, and with `--remote-debugging-pipe` because the CDP
 * pipe is still how input is driven. Two channels, two jobs: X for pixels, the
 * pipe for input.
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

#include "mi_render.h" /* the shared URL rules */
#include "mi_util.h"

#define GST_USE_UNSTABLE_API_WAS_SET 1

/* Chromium's names, in the order they are usually found. */
static const char *const BROWSERS[] = {
    "chromium", "chromium-browser", "google-chrome", "google-chrome-stable",
    "chrome", NULL,
};

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

    /* The GStreamer side. */
    GstElement *pipeline;
    GstElement *webrtc;
    GMainContext *context;
    GMainLoop *loop;
    guint bus_watch;

    /* Negotiation. */
    char *offer;
    int offer_ready;
    int answer_set;
    int connected;
    int failed;
    /* Local candidates, kept so they can be reported to the console. */
    char *local_ice;
    size_t local_ice_len;
    size_t local_ice_cap;

    /* Counters, so the stream can be reported rather than guessed at. */
    uint64_t frames;
    uint64_t bytes;

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
    cfg->chromium_path = NULL;
    cfg->xvfb_path = NULL;
    cfg->display = NULL;
    cfg->max_concurrent = 1;
    cfg->v8_heap_mb = 256;
    cfg->width = 1280;
    cfg->height = 800;
    /* 25 is what a viewer perceives as live, and every extra frame costs an
     * encode. ximagesrc will happily produce more than this. */
    cfg->fps = 25;
    /* VP8 at 1280x800 needs a few hundred kbit/s to look right. */
    cfg->bitrate_kbps = 2500;
    cfg->user_agent = NULL;
    cfg->profile_root = NULL;
    cfg->idle_timeout_seconds = 180;
    cfg->startup_timeout_seconds = 25;
    cfg->stun_server = NULL;
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
    pool->next_display = 90;

    pool->browser = find_browser(cfg->chromium_path);
    pool->xvfb = (cfg->xvfb_path && *cfg->xvfb_path)
                     ? (file_is_executable(cfg->xvfb_path) ? mi_strdup(cfg->xvfb_path) : NULL)
                     : (mi_which("Xvfb") ? mi_strdup("Xvfb") : NULL);

    /* GStreamer is initialised once for the process. */
    if (!gst_is_initialized()) {
        gst_init(NULL, NULL);
    }
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
        pid_t got = waitpid(-1, &status, WNOHANG);
        if (got <= 0) break;
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
/* GStreamer callbacks                                                 */
/* ------------------------------------------------------------------ */

/* A local ICE candidate: keep it for the console, which needs it to connect. */
static void on_ice_candidate(GstElement *webrtc, guint mline_index, gchar *candidate,
                             gpointer user_data) {
    mi_rtc *rtc = user_data;
    (void)webrtc;
    if (!candidate || !*candidate) return;
    char entry[512];
    int n = snprintf(entry, sizeof entry, "%s{\"candidate\":\"%.400s\",\"mline\":%u}",
                     rtc->local_ice_len ? "," : "", candidate, mline_index);
    if (n <= 0) return;
    size_t need = rtc->local_ice_len + (size_t)n + 2;
    if (need > rtc->local_ice_cap) {
        size_t cap = need * 2;
        char *grown = mi_realloc(rtc->local_ice, cap);
        if (!grown) return;
        rtc->local_ice = grown;
        rtc->local_ice_cap = cap;
    }
    memcpy(rtc->local_ice + rtc->local_ice_len, entry, (size_t)n);
    rtc->local_ice_len += (size_t)n;
    rtc->local_ice[rtc->local_ice_len] = '\0';
    rtc->last_activity = mono_seconds();
}

/*
 * The connection state, which is how "media is flowing" is known.
 *
 * This is a *property*, not a signal — webrtcbin has no
 * `on-connection-state-changed` signal, and connecting to one logs a GLib
 * critical and silently never fires. GObject's `notify::` is how a property
 * change is watched.
 */
static void on_connection_state(GObject *object, GParamSpec *pspec, gpointer user_data) {
    mi_rtc *rtc = user_data;
    (void)pspec;
    GstWebRTCPeerConnectionState state = GST_WEBRTC_PEER_CONNECTION_STATE_NEW;
    g_object_get(object, "connection-state", &state, NULL);
    rtc->last_activity = mono_seconds();
    if (state == GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED) {
        rtc->connected = 1;
        mi_logf(MI_LOG_INFO, "rtc: connected (%s)", rtc->url);
    } else if (state == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED) {
        set_close_reason(rtc, "the WebRTC connection failed");
    } else if (state == GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED) {
        set_close_reason(rtc, "the WebRTC connection closed");
    }
}

static void on_negotiation_needed(GstElement *webrtc, gpointer user_data);

/* The offer is ready: keep the SDP so the caller can hand it to a browser. */
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

/* A bus message: an error, an EOS, or a warning worth logging. */
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

/* Close every inherited descriptor above the ones the child is handed. */
static void close_extra_fds(void) {
    struct rlimit rl;
    long max = 1024;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
        max = (long)rl.rlim_cur;
    if (max > 65536) max = 65536;
    for (long fd = 5; fd < max; fd++) close((int)fd);
}

/* An X server on a display of its own, so two sessions cannot collide. */
static pid_t spawn_xvfb(const char *xvfb, const char *display, int w, int h) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setpgid(0, 0);
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }
        close_extra_fds();
        char screen[64];
        snprintf(screen, sizeof screen, "%dx%dx24", w, h);
        char *argv[] = {
            (char *)xvfb, (char *)display, "-screen", "0", screen,
            "-nolisten", "tcp", "-noreset", NULL,
        };
        execvp(argv[0], argv);
        _exit(127);
    }
    setpgid(pid, pid);
    return pid;
}

/*
 * Chromium on that display, NOT headless.
 *
 * This is the change that makes raw capture possible: headless Chromium
 * composites internally and never touches X, so `ximagesrc` would have nothing to
 * read. `--use-gl=swiftshader` is required because the virtual display has no GPU
 * — without a software GL path Chromium paints nothing.
 *
 * The CDP pipe is kept, because input is still driven over it. X is for pixels,
 * the pipe is for input; two channels, two jobs.
 */
static pid_t spawn_chrome(const mi_rtc_config *cfg, const char *browser, const char *profile,
                          const char *display, const char *url) {
    int p2c[2], c2p[2];
    if (pipe(p2c) != 0 || pipe(c2p) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]);
        return -1;
    }
    if (pid == 0) {
        /* Relocate first so the dup2 targets cannot collide with a source fd. */
        int hr = fcntl(p2c[0], F_DUPFD, 100);
        int hw = fcntl(c2p[1], F_DUPFD, 101);
        close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]);
        if (hr < 0 || hw < 0) _exit(120);
        if (dup2(hr, 3) < 0) _exit(121);
        if (dup2(hw, 4) < 0) _exit(122);
        if (hr != 3 && hr != 4) close(hr);
        if (hw != 3 && hw != 4) close(hw);
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
    /* The pipe ends are closed here: this module drives input elsewhere, and
     * leaking them would keep the browser alive after the session ends. */
    close(p2c[1]);
    close(c2p[0]);
    return pid;
}

/* ------------------------------------------------------------------ */
/* The pipeline                                                        */
/* ------------------------------------------------------------------ */

/*
 * Build and start the capture pipeline.
 *
 * The order matters and each element is here for a reason:
 *
 *   ximagesrc   reads the X display. Its caps are deliberately NOT fixed — it
 *               advertises `video/x-raw` with no format and negotiates, and
 *               demanding one at the source fails with "can't handle caps".
 *   videoconvert converts whatever X gave us into I420, which VP8 wants.
 *   vp8enc      encodes. `deadline` makes it keep up with real time rather than
 *               trying to be pretty, and `cpu-used` trades a little quality for
 *               speed, because this shares a host with the browser.
 *   rtpvp8pay   packetises for RTP.
 *   webrtcbin   the transport: ICE, DTLS, SRTP.
 *
 * `appsrc` is not used: the frames come from X, not from us.
 */
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

    /* The bus carries errors and EOS; without draining it a failure is silent. */
    GstBus *bus = gst_element_get_bus(rtc->pipeline);
    gst_bus_add_watch_full(bus, G_PRIORITY_DEFAULT, on_bus_message, rtc, NULL);
    gst_object_unref(bus);

    GstStateChangeReturn ret = gst_element_set_state(rtc->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
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
        /* Wait for something to happen, but not past the deadline. */
        int left = (int)((deadline - mono_seconds()) * 1000.0);
        if (left <= 0) break;
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
                          char **offer_out, char *err, size_t errcap) {
    *out = NULL;
    if (offer_out) *offer_out = NULL;
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
    rtc->url = mi_strdup(url);
    rtc->last_activity = mono_seconds();
    rtc->local_ice_cap = 1024;
    rtc->local_ice = mi_malloc(rtc->local_ice_cap);
    if (rtc->local_ice) rtc->local_ice[0] = '\0';

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

    rtc->chrome_pid = spawn_chrome(&pool->cfg, pool->browser, rtc->profile, rtc->display, url);
    if (rtc->chrome_pid < 0) {
        if (err) snprintf(err, errcap, "could not start the browser");
        goto fail;
    }

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

    /*
     * Wait for the browser to paint and the offer to appear.
     *
     * Both take time: chromium has to start, create its window and paint before
     * `ximagesrc` has anything to read, and the offer is produced by a signal that
     * only arrives while the context is pumped.
     */
    int tmo = pool->cfg.startup_timeout_seconds * 1000;
    double deadline = mono_seconds() + tmo / 1000.0;
    while (mono_seconds() < deadline) {
        pump_context(rtc, 100);
        if (rtc->offer_ready) break;
        if (rtc->closed) break;
    }
    if (!rtc->offer_ready || !rtc->offer) {
        if (err) snprintf(err, errcap, "the pipeline produced no offer");
        goto fail;
    }

    if (offer_out) {
        *offer_out = mi_strdup(rtc->offer);
    }
    *out = rtc;
    return MI_RTC_OK;

fail:
    mi_rtc_close(rtc);
    return MI_RTC_FAILED;
}

/* ------------------------------------------------------------------ */
/* Driving a session                                                   */
/* ------------------------------------------------------------------ */

int mi_rtc_set_answer(mi_rtc *rtc, const char *sdp) {
    if (!rtc || !rtc->webrtc || !sdp) return -1;
    GstSDPMessage *msg = NULL;
    if (gst_sdp_message_new(&msg) != GST_SDP_OK) return -1;
    if (gst_sdp_message_parse_buffer((const guint8 *)sdp, (guint)strlen(sdp), msg) != GST_SDP_OK) {
        gst_sdp_message_free(msg);
        return -1;
    }
    GstWebRTCSessionDescription *answer =
        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, msg);
    if (!answer) {
        gst_sdp_message_free(msg);
        return -1;
    }
    g_signal_emit_by_name(rtc->webrtc, "set-remote-description", answer, NULL);
    gst_webrtc_session_description_free(answer);
    rtc->answer_set = 1;
    rtc->last_activity = mono_seconds();
    return 0;
}

int mi_rtc_add_ice(mi_rtc *rtc, const char *candidate, int mline) {
    if (!rtc || !rtc->webrtc || !candidate || !*candidate) return -1;
    g_signal_emit_by_name(rtc->webrtc, "add-ice-candidate", mline < 0 ? 0 : mline, candidate);
    rtc->last_activity = mono_seconds();
    return 0;
}

int mi_rtc_pump(mi_rtc *rtc, int ms) {
    if (!rtc) return -1;
    if (rtc->closed) return -1;

    g_main_context_push_thread_default(rtc->context);
    pump_context(rtc, ms);
    g_main_context_pop_thread_default(rtc->context);

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

char *mi_rtc_local_ice_json(mi_rtc *rtc) {
    if (!rtc) return mi_strdup("[]");
    return mi_asprintf("[%s]", rtc->local_ice ? rtc->local_ice : "");
}

void mi_rtc_stats(mi_rtc *rtc, uint64_t *frames, uint64_t *bytes) {
    if (frames) *frames = rtc ? rtc->frames : 0;
    if (bytes) *bytes = rtc ? rtc->bytes : 0;
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
    if (rtc->pipeline) {
        gst_element_set_state(rtc->pipeline, GST_STATE_NULL);
    }
    if (rtc->webrtc) gst_object_unref(rtc->webrtc);
    if (rtc->pipeline) gst_object_unref(rtc->pipeline);
    if (rtc->context) g_main_context_unref(rtc->context);
    if (rtc->loop) g_main_loop_unref(rtc->loop);

    kill_group(rtc->chrome_pid);
    kill_group(rtc->xvfb_pid);
    if (rtc->profile) mi_rm_rf(rtc->profile);
    free(rtc->profile);
    free(rtc->url);
    free(rtc->offer);
    free(rtc->local_ice);
    if (rtc->pool) {
        reap_children(rtc->pool);
        pool_release(rtc->pool);
    }
    free(rtc);
}

