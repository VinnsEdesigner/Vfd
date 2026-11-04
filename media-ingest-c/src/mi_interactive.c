/* mi_interactive.c - a live, driveable page: Chromium over the CDP pipe.
 *
 * The shape of this file is dictated by one choice: the browser is driven over
 * --remote-debugging-pipe, so the transport is two file descriptors and every
 * message is a NUL-terminated JSON object. There is no WebSocket client here and
 * no TCP port opened — a debugging port would be an unauthenticated control
 * socket for anything on the host.
 *
 * A session is a small state machine fed by a poll loop. `pump` reads whatever
 * the browser has said, turns screencast frames into the latest picture, folds
 * navigation events into a page state, and acknowledges each frame so the next
 * one is sent. Input from the console goes the other way as CDP commands.
 *
 * The concurrency gate lives on the pool, because the server itself spawns a
 * thread per connection with no cap: an expensive endpoint has to bring its own.
 */
#define _GNU_SOURCE
#include "mi_interactive.h"

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

#include "mi_json.h"
#include "mi_render.h" /* one set of URL rules for both render and live sessions */
#include "mi_util.h"

/* Chromium's own names, in the order they are usually found. */
static const char *const BROWSERS[] = {
    "chromium", "chromium-browser", "google-chrome", "google-chrome-stable",
    "chrome", NULL,
};

/* Chromium reads CDP from fd 3 and writes to fd 4 under --remote-debugging-pipe.
 * This is not configurable; the probe that settled it is in the memory log. */
#define CDP_READ_FD 3
#define CDP_WRITE_FD 4

/* The outgoing command queue is bounded, so a flooding console is disconnected
 * rather than allowed to grow the engine's memory without limit. */
#define CDP_WRITE_MAX (1024u * 1024u)
/* Incoming: a screenshot of a large page is the biggest expected message. */
#define CDP_READ_MAX (32u * 1024u * 1024u)

/* A screencast frame's quality. High enough to read text, low enough that a
 * frame is tens of kilobytes rather than hundreds. */
#define SCREENCAST_QUALITY 70

struct mi_interactive_pool {
    mi_interactive_config cfg;
    char *browser;
    pthread_mutex_t lock;
    int in_use;
    pthread_mutex_t reap_lock;
};

struct mi_interactive {
    mi_interactive_pool *pool;
    pid_t pid;
    int wfd; /* parent -> browser (the child's fd 3) */
    int rfd; /* browser -> parent (the child's fd 4) */
    char *profile;
    char session_id[64];
    int next_id;
    int shot_id; /* the id of an outstanding captureScreenshot, or 0 */

    /* The most recent response, kept as text so the synchronous startup
     * exchange can read a result (a target list, a session id) rather than only
     * being told that a response arrived. */
    int resp_id;
    int resp_ok;
    char *resp_json;

    /* Incoming bytes, split on NUL. */
    unsigned char *rbuf;
    size_t rbuf_len, rbuf_cap;
    /* Outgoing commands waiting for the pipe to accept them. */
    unsigned char *wbuf;
    size_t wbuf_len, wbuf_cap;

    unsigned char *frame;
    size_t frame_len;
    char frame_format[8];
    int frame_w, frame_h;
    /*
     * Whether this session is emulating a mobile device.
     *
     * It has to be remembered, not inferred: `Emulation.setDeviceMetricsOverride`
     * carries the flag, and a resize that sent `mobile: false` would silently
     * switch a phone layout back to a desktop one the first time the console
     * re-fit the stage — which is exactly what happened.
     */
    int mobile;

    /* Page state, emitted as JSON only when it changes. */
    char *state_json;
    char url[1024];
    char title[512];
    int loading;

    /* What the last pump turned up. */
    int ev_frame, ev_shot, ev_state;

    /*
     * Two different clocks, and the difference matters.
     *
     * `last_activity` moves whenever anything happens, including a frame the
     * page painted on its own. The idle timeout reads it, so a session is closed
     * only when the page is *both* untouched and still — a user reading a page
     * that animates is not idle, and cutting them off would be wrong.
     *
     * That rule cannot catch a client that has vanished, because such a page
     * keeps painting and keeps the clock moving. The keepalive does that instead:
     * see `mi_interactive_should_ping` and the silence check in the server loop.
     */
    double last_activity;
    /* When a heartbeat was last sent, and when anything at all was last heard
     * from the client. A client that disappears without a close (a tab closed
     * behind a proxy that keeps the connection) is found this way. */
    double last_ping;
    double last_heard;
    int closed;
    char close_reason[200];
};

void mi_interactive_config_init(mi_interactive_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->chromium_path = NULL;
    cfg->max_concurrent = 1;
    cfg->v8_heap_mb = 256;
    cfg->viewport_width = 1280;
    cfg->viewport_height = 800;
    cfg->user_agent = NULL;
    cfg->profile_root = NULL;
    cfg->idle_timeout_seconds = 180;
    cfg->startup_timeout_seconds = 15;
    /* A ping every 20 s, and a client silent for 45 s is gone. Both well above
     * any plausible round trip, and both short enough that a leaked session does
     * not hold the gate for long. */
    cfg->keepalive_ping_seconds = 20;
    cfg->keepalive_timeout_seconds = 45;
}

int mi_interactive_url_allowed(const char *url) {
    return mi_render_url_allowed(url);
}

/* ------------------------------------------------------------------ */
/* Finding a browser                                                   */
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

/* ------------------------------------------------------------------ */
/* The pool                                                            */
/* ------------------------------------------------------------------ */

mi_interactive_pool *mi_interactive_pool_new(const mi_interactive_config *cfg) {
    /* Chromium's helpers reparent when the browser dies; as a subreaper they
     * come here and one waitpid loop clears them, instead of piling up as
     * zombies behind a container's PID 1 that never reaps. */
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0) {
        mi_logf(MI_LOG_ERROR, "interactive: could not become a subreaper: %s", strerror(errno));
    }
    mi_interactive_pool *pool = mi_calloc(1, sizeof *pool);
    if (!pool) return NULL;
    pool->cfg = *cfg;
    if (pool->cfg.max_concurrent <= 0) pool->cfg.max_concurrent = 1;
    if (pool->cfg.viewport_width <= 0) pool->cfg.viewport_width = 1280;
    if (pool->cfg.viewport_height <= 0) pool->cfg.viewport_height = 800;
    if (pool->cfg.v8_heap_mb <= 0) pool->cfg.v8_heap_mb = 256;
    if (pool->cfg.startup_timeout_seconds <= 0) pool->cfg.startup_timeout_seconds = 15;
    if (pool->cfg.keepalive_ping_seconds <= 0) pool->cfg.keepalive_ping_seconds = 20;
    if (pool->cfg.keepalive_timeout_seconds <= 0) pool->cfg.keepalive_timeout_seconds = 45;
    pool->browser = find_browser(cfg->chromium_path);
    if (!pool->browser) {
        mi_logf(MI_LOG_INFO, "interactive: no browser found; live sessions are unavailable");
    } else {
        mi_logf(MI_LOG_INFO, "interactive: using %s (limit %d, idle %ds)",
                pool->browser, pool->cfg.max_concurrent, pool->cfg.idle_timeout_seconds);
    }
    pthread_mutex_init(&pool->lock, NULL);
    pthread_mutex_init(&pool->reap_lock, NULL);
    return pool;
}

void mi_interactive_pool_free(mi_interactive_pool *pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->lock);
    while (pool->in_use > 0) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    pthread_mutex_unlock(&pool->lock);
    free(pool->browser);
    pthread_mutex_destroy(&pool->lock);
    pthread_mutex_destroy(&pool->reap_lock);
    free(pool);
}

int mi_interactive_pool_available(const mi_interactive_pool *pool) {
    return pool && pool->browser != NULL;
}

int mi_interactive_pool_limit(const mi_interactive_pool *pool) {
    return pool ? pool->cfg.max_concurrent : 0;
}

int mi_interactive_pool_active(const mi_interactive_pool *pool) {
    if (!pool) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&pool->lock);
    int n = pool->in_use;
    pthread_mutex_unlock((pthread_mutex_t *)&pool->lock);
    return n;
}

/* Take a slot, or report busy. A queue behind a live browser would grow without
 * bound, so a full gate is refused and the console retries. */
static int pool_take(mi_interactive_pool *pool) {
    pthread_mutex_lock(&pool->lock);
    int ok = pool->in_use < pool->cfg.max_concurrent;
    if (ok) pool->in_use++;
    pthread_mutex_unlock(&pool->lock);
    return ok;
}

static void pool_release(mi_interactive_pool *pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->lock);
    if (pool->in_use > 0) pool->in_use--;
    pthread_mutex_unlock(&pool->lock);
}

static void reap_children(mi_interactive_pool *pool) {
    pthread_mutex_lock(&pool->reap_lock);
    for (;;) {
        int status = 0;
        pid_t got = waitpid(-1, &status, WNOHANG);
        if (got <= 0) break;
    }
    pthread_mutex_unlock(&pool->reap_lock);
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static double mono_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void set_close_reason(mi_interactive *it, const char *why) {
    snprintf(it->close_reason, sizeof it->close_reason, "%s", why);
    it->closed = 1;
}

/* Append to the outgoing queue. */
static void cdp_queue(mi_interactive *it, const char *json, size_t len) {
    if (it->closed) return;
    if (it->wbuf_len + len + 1 > CDP_WRITE_MAX) {
        set_close_reason(it, "the console sent more commands than the browser could take");
        return;
    }
    if (it->wbuf_len + len + 1 > it->wbuf_cap) {
        size_t cap = (it->wbuf_len + len + 1) * 2;
        unsigned char *grown = mi_realloc(it->wbuf, cap);
        if (!grown) return;
        it->wbuf = grown;
        it->wbuf_cap = cap;
    }
    memcpy(it->wbuf + it->wbuf_len, json, len);
    it->wbuf_len += len;
    it->wbuf[it->wbuf_len++] = '\0'; /* CDP frames messages with a NUL */
}

static void cdp_flush(mi_interactive *it) {
    while (!it->closed && it->wbuf_len > 0) {
        ssize_t n = write(it->wfd, it->wbuf, it->wbuf_len);
        if (n > 0) {
            memmove(it->wbuf, it->wbuf + n, it->wbuf_len - (size_t)n);
            it->wbuf_len -= (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return; /* pipe full */
        if (n < 0 && errno == EINTR) continue;
        set_close_reason(it, "the browser stopped reading");
        return;
    }
}

/* Send one command. `params` is a raw JSON object or NULL. */
static int cdp_command(mi_interactive *it, const char *method, const char *params) {
    if (it->closed) return 0;
    int id = ++it->next_id;
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"id\":");
    mi_json_int(&b, id);
    mi_json_raw(&b, ",\"method\":");
    mi_json_string(&b, method);
    if (it->session_id[0]) {
        mi_json_raw(&b, ",\"sessionId\":");
        mi_json_string(&b, it->session_id);
    }
    if (params) {
        mi_json_raw(&b, ",\"params\":");
        mi_json_raw(&b, params);
    }
    mi_json_raw(&b, "}");
    cdp_queue(it, mi_json_data(&b), b.len);
    mi_json_free(&b);
    cdp_flush(it);
    return id;
}

static void set_state_changed(mi_interactive *it) {
    free(it->state_json);
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"url\":");
    mi_json_string(&b, it->url);
    mi_json_raw(&b, ",\"title\":");
    mi_json_string(&b, it->title);
    mi_json_raw(&b, ",\"loading\":");
    mi_json_bool(&b, it->loading);
    mi_json_raw(&b, "}");
    it->state_json = mi_strdup(mi_json_data(&b));
    mi_json_free(&b);
    it->ev_state = 1;
}

/* Read a string out of the last response's result object. Allocated. */
static char *resp_result_string(const mi_interactive *it, const char *key) {
    if (!it->resp_json) return NULL;
    mi_json *root = mi_json_parse(it->resp_json);
    if (!root) return NULL;
    const mi_json *result = mi_json_get(root, "result");
    const char *v = mi_json_get_string(result, key, NULL);
    char *out = v ? mi_strdup(v) : NULL;
    mi_json_value_free(root);
    return out;
}

/* ------------------------------------------------------------------ */
/* Reading and dispatching                                             */
/* ------------------------------------------------------------------ */

/* Decode a base64 image into the frame buffer and remember its format. */
static void store_frame(mi_interactive *it, const char *b64, size_t b64len,
                        const char *format, int w, int h) {
    if (!b64) return;
    size_t cap = (b64len / 4 + 2) * 3;
    unsigned char *buf = mi_malloc(cap ? cap : 1);
    if (!buf) return;
    ssize_t got = mi_base64_decode(b64, b64len, buf, cap);
    if (got < 0) {
        free(buf);
        return;
    }
    free(it->frame);
    it->frame = buf;
    it->frame_len = (size_t)got;
    /* `format` may point at it->frame_format itself, so guard against an
     * overlapping snprintf, which is undefined and showed up as an empty
     * format on alternating frames. */
    if (format && format != it->frame_format)
        snprintf(it->frame_format, sizeof it->frame_format, "%s", format);
    if (w > 0) it->frame_w = w;
    if (h > 0) it->frame_h = h;
    it->last_activity = mono_seconds();
}

/* A screencast frame arrived: take the picture and acknowledge it, or no more
 * will come. The ack carries the frame's own numeric session id, not our CDP
 * session string — they are different things. */
static void on_screencast_frame(mi_interactive *it, const mi_json *params) {
    const char *data = mi_json_get_string(params, "data", NULL);
    const mi_json *meta = mi_json_get(params, "metadata");
    int w = (int)mi_json_get_number(meta, "deviceWidth", 0);
    int h = (int)mi_json_get_number(meta, "deviceHeight", 0);
    const char *fmt = it->frame_format[0] ? it->frame_format : "jpeg";
    store_frame(it, data, data ? strlen(data) : 0, fmt, w, h);
    it->ev_frame = 1;

    long num = (long)mi_json_get_number(params, "sessionId", 0);
    char cmd[160];
    snprintf(cmd, sizeof cmd, "{\"sessionId\":%ld}", num);
    cdp_command(it, "Page.screencastFrameAck", cmd);
}

static void on_frame_navigated(mi_interactive *it, const mi_json *params) {
    const mi_json *frame = mi_json_get(params, "frame");
    const char *url = mi_json_get_string(frame, "url", NULL);
    if (url && strcmp(url, it->url) != 0) {
        snprintf(it->url, sizeof it->url, "%s", url);
        set_state_changed(it);
    }
}

static void handle_event(mi_interactive *it, const char *method, const mi_json *params) {
    if (!strcmp(method, "Page.screencastFrame")) {
        on_screencast_frame(it, params);
    } else if (!strcmp(method, "Page.frameNavigated")) {
        on_frame_navigated(it, params);
    } else if (!strcmp(method, "Page.frameStartedLoading")) {
        if (!it->loading) { it->loading = 1; set_state_changed(it); }
    } else if (!strcmp(method, "Page.loadEventFired") ||
               !strcmp(method, "Page.frameStoppedLoading")) {
        if (it->loading) { it->loading = 0; set_state_changed(it); }
    } else if (!strcmp(method, "Page.javascriptDialogOpening")) {
        /* An alert()/confirm() blocks the page until answered. A browsing
         * surface must not freeze, so dialogs are dismissed automatically. */
        cdp_command(it, "Page.handleJavaScriptDialog", "{\"accept\":true}");
    } else if (!strcmp(method, "Inspector.targetCrashed") ||
               !strcmp(method, "Target.detachedFromTarget")) {
        set_close_reason(it, "the page crashed");
    }
}

/* Handle one complete CDP message. */
static void handle_message(mi_interactive *it, const char *json) {
    mi_json *root = mi_json_parse(json);
    if (!root) return;

    const mi_json *idv = mi_json_get(root, "id");
    const char *method = mi_json_get_string(root, "method", NULL);

    if (idv && idv->type == MI_JSON_NUMBER) {
        int id = (int)idv->number;
        it->resp_id = id;
        it->resp_ok = mi_json_get(root, "error") == NULL;
        free(it->resp_json);
        it->resp_json = mi_strdup(json);
        /* A screenshot's bytes come back on its response, not as an event. */
        if (it->resp_ok && id == it->shot_id) {
            const mi_json *result = mi_json_get(root, "result");
            const char *data = mi_json_get_string(result, "data", NULL);
            store_frame(it, data, data ? strlen(data) : 0, "png", it->frame_w, it->frame_h);
            it->ev_shot = 1;
            it->shot_id = 0;
        }
    } else if (method) {
        handle_event(it, method, mi_json_get(root, "params"));
    }
    mi_json_value_free(root);
}

/* Read and dispatch whatever the browser has said, up to `ms`. Returns 0 while
 * the session is alive, -1 once it is closed. */
static int pump_io(mi_interactive *it, int ms) {
    if (it->closed) return -1;
    cdp_flush(it);

    struct pollfd p = {.fd = it->rfd, .events = POLLIN};
    int pr = poll(&p, 1, ms);
    if (pr < 0 && errno != EINTR) {
        set_close_reason(it, "the browser connection broke");
        return -1;
    }
    if (pr > 0 && (p.revents & (POLLIN | POLLHUP))) {
        for (;;) {
            if (it->rbuf_len + 65536 > it->rbuf_cap) {
                size_t cap = it->rbuf_cap ? it->rbuf_cap * 2 : 131072;
                if (cap > CDP_READ_MAX + 65536) cap = CDP_READ_MAX + 65536;
                unsigned char *grown = mi_realloc(it->rbuf, cap);
                if (!grown) return -1;
                it->rbuf = grown;
                it->rbuf_cap = cap;
            }
            ssize_t n = read(it->rfd, it->rbuf + it->rbuf_len, 65536);
            if (n > 0) {
                it->rbuf_len += (size_t)n;
                if (it->rbuf_len > CDP_READ_MAX) {
                    set_close_reason(it, "the browser sent more data than expected");
                    return -1;
                }
                continue;
            }
            if (n == 0) {
                set_close_reason(it, "the browser exited");
                return -1;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            set_close_reason(it, "the browser connection broke");
            return -1;
        }

        /* Split on NUL and dispatch each complete message. */
        size_t start = 0;
        for (size_t i = 0; i < it->rbuf_len; i++) {
            if (it->rbuf[i] != '\0') continue;
            it->rbuf[i] = '\0';
            if (i > start) handle_message(it, (const char *)it->rbuf + start);
            start = i + 1;
        }
        if (start > 0) {
            memmove(it->rbuf, it->rbuf + start, it->rbuf_len - start);
            it->rbuf_len -= start;
        }
    }
    return it->closed ? -1 : 0;
}

/* Pump until the response with `id` arrives, or the deadline passes. Returns 1
 * on a successful response, 0 on an error response, -1 on timeout or close. */
static int wait_for_id(mi_interactive *it, int id, int ms) {
    double deadline = mono_seconds() + ms / 1000.0;
    for (;;) {
        if (pump_io(it, 50) < 0) return -1;
        if (it->resp_id == id) return it->resp_ok ? 1 : 0;
        if (mono_seconds() >= deadline) return -1;
    }
}

/* ------------------------------------------------------------------ */
/* Starting a browser                                                  */
/* ------------------------------------------------------------------ */

/* Close every inherited descriptor except the three the browser is handed. A
 * container's engine holds a listen socket and more; none of it belongs in a
 * browser, and leaking it keeps those objects alive for the session's life. */
static void close_extra_fds(void) {
    struct rlimit rl;
    long max = 1024;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
        max = (long)rl.rlim_cur;
    if (max > 65536) max = 65536;
    for (long fd = 5; fd < max; fd++) close((int)fd);
}

static pid_t spawn_browser(const mi_interactive_config *cfg, const char *browser,
                           const char *profile, const char *start_url,
                           int *wfd_out, int *rfd_out) {
    int p2c[2], c2p[2];
    if (pipe(p2c) != 0 || pipe(c2p) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]);
        return -1;
    }
    if (pid == 0) {
        /* Relocate to high fds first so the dup2 targets cannot collide with a
         * source fd — the bug that silently closed the read end in the probe. */
        int hr = fcntl(p2c[0], F_DUPFD, 100);
        int hw = fcntl(c2p[1], F_DUPFD, 101);
        close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]);
        if (hr < 0 || hw < 0) _exit(120);
        if (dup2(hr, CDP_READ_FD) < 0) _exit(121);
        if (dup2(hw, CDP_WRITE_FD) < 0) _exit(122);
        if (hr != CDP_READ_FD && hr != CDP_WRITE_FD) close(hr);
        if (hw != CDP_READ_FD && hw != CDP_WRITE_FD) close(hw);
        close_extra_fds();

        /* Its own group, so a kill takes the whole browser tree. */
        setpgid(0, 0);

        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }

        char *jsflags = mi_asprintf("--js-flags=--max-old-space-size=%d", cfg->v8_heap_mb);
        char *udd = mi_asprintf("--user-data-dir=%s", profile);
        char *win = mi_asprintf("--window-size=%d,%d", cfg->viewport_width,
                                cfg->viewport_height);
        char *ua = (cfg->user_agent && *cfg->user_agent)
                       ? mi_asprintf("--user-agent=%s", cfg->user_agent) : NULL;
        const char *av[40];
        int n = 0;
        av[n++] = browser;
        av[n++] = "--headless=new";
        av[n++] = "--no-sandbox";
        av[n++] = "--disable-dev-shm-usage";
        av[n++] = "--disable-gpu";
        av[n++] = "--hide-scrollbars";
        av[n++] = "--no-first-run";
        av[n++] = "--no-default-browser-check";
        av[n++] = "--disable-background-networking";
        av[n++] = "--disable-sync";
        av[n++] = "--disable-translate";
        av[n++] = "--disable-extensions";
        av[n++] = "--disable-default-apps";
        av[n++] = "--mute-audio";
        av[n++] = "--disable-features=Translate,BackForwardCache,AcceptCHFrame";
        /* Keep the page painting while it is backgrounded, or a screencast of a
         * window nobody has focus on would stop updating. */
        av[n++] = "--disable-backgrounding-occluded-windows";
        av[n++] = "--disable-renderer-backgrounding";
        av[n++] = "--disable-background-timer-throttling";
        av[n++] = "--remote-debugging-pipe";
        if (ua) av[n++] = ua;
        if (jsflags) av[n++] = jsflags;
        if (udd) av[n++] = udd;
        if (win) av[n++] = win;
        /* The page is the browser's own startup argument, not a Page.navigate
         * after attaching. That is the difference between a screencast that
         * starts and one the browser refuses as "not attached to an active
         * page": the startup target is the active page. */
        av[n++] = (start_url && *start_url) ? start_url : "about:blank";
        av[n++] = NULL;
        execvp(av[0], (char *const *)av);
        _exit(127);
    }

    close(p2c[0]);
    close(c2p[1]);
    setpgid(pid, pid); /* also in the parent, to close the race */
    *wfd_out = p2c[1];
    *rfd_out = c2p[0];
    return pid;
}

/* ------------------------------------------------------------------ */
/* Opening a session                                                   */
/* ------------------------------------------------------------------ */

static char *make_profile_dir(const mi_interactive_config *cfg, const char *id) {
    const char *root = (cfg->profile_root && *cfg->profile_root) ? cfg->profile_root : "/tmp";
    char *dir = mi_asprintf("%s/vyzorix-live-%s", root, id);
    if (!dir) return NULL;
    mi_rm_rf(dir);
    if (mi_mkdir_p(dir) != 0) { free(dir); return NULL; }
    return dir;
}

/* Find the page target's id in the last response (a Target.getTargets result). */
static int parse_page_target(const char *json, char *out, size_t outcap) {
    mi_json *root = mi_json_parse(json);
    if (!root) return -1;
    const mi_json *result = mi_json_get(root, "result");
    const mi_json *infos = mi_json_get(result, "targetInfos");
    int found = -1;
    if (infos && infos->type == MI_JSON_ARRAY) {
        for (size_t i = 0; i < infos->count; i++) {
            const mi_json *t = infos->items[i];
            const char *type = mi_json_get_string(t, "type", NULL);
            if (!type || strcmp(type, "page") != 0) continue;
            const char *tid = mi_json_get_string(t, "targetId", NULL);
            if (!tid) continue;
            snprintf(out, outcap, "%s", tid);
            found = 0;
            break;
        }
    }
    mi_json_value_free(root);
    return found;
}

mi_it_status mi_interactive_open(mi_interactive_pool *pool, const char *url,
                                 int width, int height, int touch, mi_interactive **out,
                                 char *err, size_t errcap) {
    *out = NULL;
    if (err && errcap) err[0] = '\0';
    if (!pool) {
        if (err) snprintf(err, errcap, "no session pool");
        return MI_IT_FAILED;
    }
    if (!mi_interactive_url_allowed(url)) {
        if (err) snprintf(err, errcap, "only public http(s) pages can be opened");
        return MI_IT_BAD_URL;
    }
    if (!pool->browser) {
        if (err) snprintf(err, errcap, "no browser is installed on this host");
        return MI_IT_NO_BROWSER;
    }
    if (!pool_take(pool)) {
        if (err) snprintf(err, errcap, "too many live sessions");
        return MI_IT_BUSY;
    }

    mi_interactive *it = mi_calloc(1, sizeof *it);
    if (!it) {
        pool_release(pool);
        if (err) snprintf(err, errcap, "out of memory");
        return MI_IT_FAILED;
    }
    it->pool = pool;
    it->wfd = it->rfd = -1;
    snprintf(it->frame_format, sizeof it->frame_format, "jpeg");
    {
        double now = mono_seconds();
        it->last_activity = now;
        it->last_ping = now;
        it->last_heard = now;
    }

    int tmo = pool->cfg.startup_timeout_seconds * 1000;

    char id[64];
    snprintf(id, sizeof id, "%d-%ld", (int)getpid(), (long)time(NULL));
    it->profile = make_profile_dir(&pool->cfg, id);
    if (!it->profile) {
        if (err) snprintf(err, errcap, "could not create a browser profile");
        goto fail;
    }

    it->pid = spawn_browser(&pool->cfg, pool->browser, it->profile, url,
                            &it->wfd, &it->rfd);
    if (it->pid < 0) {
        if (err) snprintf(err, errcap, "could not start the browser");
        goto fail;
    }
    /* Non-blocking both ways: a stalled browser must not block the engine, and
     * the read loop drains until EAGAIN rather than blocking on the next byte. */
    int fl = fcntl(it->wfd, F_GETFL, 0);
    fcntl(it->wfd, F_SETFL, fl | O_NONBLOCK);
    fl = fcntl(it->rfd, F_GETFL, 0);
    fcntl(it->rfd, F_SETFL, fl | O_NONBLOCK);

    /* 1. Find the page target. The startup page may not be ready at once, so
     * this is retried until the deadline rather than asked once. */
    char target[64] = {0};
    {
        double deadline = mono_seconds() + tmo / 1000.0;
        while (!target[0] && mono_seconds() < deadline && !it->closed) {
            int rid = cdp_command(it, "Target.getTargets", NULL);
            if (wait_for_id(it, rid, 2000) < 0) continue;
            if (it->resp_json) parse_page_target(it->resp_json, target, sizeof target);
            if (!target[0]) {
                struct timespec t = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
                nanosleep(&t, NULL);
            }
        }
    }
    if (!target[0]) {
        if (err) snprintf(err, errcap, "the browser did not offer a page to drive");
        goto fail;
    }

    /* 2. Attach, flattened, so commands carry a sessionId and events for this
     * page arrive on the same pipe without a target-routing dance. */
    {
        char params[256];
        snprintf(params, sizeof params,
                 "{\"targetId\":\"%s\",\"flatten\":true}", target);
        int rid = cdp_command(it, "Target.attachToTarget", params);
        if (wait_for_id(it, rid, tmo) != 1) {
            if (err) snprintf(err, errcap, "could not attach to the page");
            goto fail;
        }
        char *sid = resp_result_string(it, "sessionId");
        if (!sid || !*sid) {
            free(sid);
            if (err) snprintf(err, errcap, "the browser attached without a session");
            goto fail;
        }
        snprintf(it->session_id, sizeof it->session_id, "%s", sid);
        free(sid);
    }

    /* 3. Turn on the domains the session uses. */
    {
        int rid = cdp_command(it, "Page.enable", NULL);
        wait_for_id(it, rid, tmo);
        rid = cdp_command(it, "Runtime.enable", NULL);
        wait_for_id(it, rid, tmo);
    }

    /* 4. The viewport the console asked for. Without this the browser paints at
     * its own window size and the frames do not match the canvas. */
    {
        char params[160];
        snprintf(params, sizeof params,
                 "{\"width\":%d,\"height\":%d,\"deviceScaleFactor\":1,\"mobile\":%s}",
                 width, height, touch ? "true" : "false");
        int rid = cdp_command(it, "Emulation.setDeviceMetricsOverride", params);
        wait_for_id(it, rid, tmo);
        it->frame_w = width;
        it->frame_h = height;
        it->mobile = touch;
    }

    /* 4b. Touch, when the console is on a touch device. A phone sends a finger,
     * not a pointer, and a page lays itself out differently for touch — so the
     * emulation has to match the input that will arrive. */
    if (touch) {
        int rid = cdp_command(it, "Emulation.setTouchEmulationEnabled",
                              "{\"enabled\":true,\"maxTouchPoints\":5}");
        wait_for_id(it, rid, tmo);
    }

    /*
     * Focus emulation, always.
     *
     * A headless browser has no window, so the page is treated as unfocused and
     * a clicked field does not take the caret — typing then goes nowhere. This
     * tells the page it is focused, which is what a real window would do, and is
     * the difference between a page you can type in and one you cannot.
     */
    {
        int rid = cdp_command(it, "Emulation.setFocusEmulationEnabled", "{\"enabled\":true}");
        wait_for_id(it, rid, tmo);
    }

    /* 5. Start the stream. The page itself was the browser's startup argument,
     * so it is already the active, attached page — which is what a screencast
     * requires. */
    {
        char params[160];
        snprintf(params, sizeof params,
                 "{\"format\":\"jpeg\",\"quality\":%d,\"everyNthFrame\":1}",
                 SCREENCAST_QUALITY);
        int rid = cdp_command(it, "Page.startScreencast", params);
        if (wait_for_id(it, rid, tmo) != 1) {
            if (err) snprintf(err, errcap, "the browser would not start streaming the page");
            goto fail;
        }
    }

    /* The page's title, best-effort: a page may not have one yet, and that is
     * not a reason to fail the session. */
    {
        int rid = cdp_command(it, "Runtime.evaluate",
                              "{\"expression\":\"document.title\",\"returnByValue\":true}");
        if (wait_for_id(it, rid, 2000) == 1 && it->resp_json) {
            mi_json *root = mi_json_parse(it->resp_json);
            const mi_json *result = mi_json_get(root, "result");
            const mi_json *r2 = mi_json_get(result, "result");
            const char *t = mi_json_get_string(r2, "value", NULL);
            if (t) snprintf(it->title, sizeof it->title, "%s", t);
            mi_json_value_free(root);
        }
    }
    snprintf(it->url, sizeof it->url, "%s", url);

    /* Prime the first state message and give the browser a moment to paint so
     * the first frame is not a blank canvas. */
    it->ev_state = 0;
    set_state_changed(it);
    pump_io(it, 400);

    /*
     * Start the clocks here, not at allocation. The startup exchange takes
     * seconds, during which the client has nothing to say, so counting that time
     * as silence would drop a perfectly healthy session the moment it opened.
     */
    {
        double now = mono_seconds();
        it->last_activity = now;
        it->last_ping = now;
        it->last_heard = now;
    }
    *out = it;
    return MI_IT_OK;

fail:
    mi_interactive_close(it);
    return MI_IT_FAILED;
}

/* ------------------------------------------------------------------ */
/* Pumping                                                             */
/* ------------------------------------------------------------------ */

mi_it_event mi_interactive_pump(mi_interactive *it, int ms) {
    if (!it) return MI_IT_EV_CLOSED;
    it->ev_frame = it->ev_shot = it->ev_state = 0;
    if (!it->closed) pump_io(it, ms);

    if (it->closed) return MI_IT_EV_CLOSED;

    double now = mono_seconds();

    /*
     * A live browser left open is a leak the size of a browser, so a session
     * nobody is using is closed here rather than left to the OS. "Not in use"
     * means both untouched and still: the clock is the last frame or command, so
     * a user reading a page that animates is not cut off.
     *
     * A client that has vanished is a different problem — its page may keep
     * painting — and is found by the keepalive, not here.
     */
    if (it->pool && it->pool->cfg.idle_timeout_seconds > 0 &&
        now - it->last_activity > it->pool->cfg.idle_timeout_seconds) {
        set_close_reason(it, "the session was idle for too long");
        return MI_IT_EV_CLOSED;
    }

    if (it->ev_frame) return MI_IT_EV_FRAME;
    if (it->ev_shot) return MI_IT_EV_SHOT;
    if (it->ev_state) return MI_IT_EV_STATE;
    return MI_IT_EV_NONE;
}

/*
 * Whether the client has gone quiet long enough that the session should be
 * probed. The server sends a ping, so a peer that has vanished without a close
 * is found and its session released rather than holding the gate.
 */
int mi_interactive_should_ping(mi_interactive *it) {
    if (!it || it->closed) return 0;
    return mono_seconds() - it->last_ping >= (double)it->pool->cfg.keepalive_ping_seconds;
}

void mi_interactive_note_ping(mi_interactive *it) {
    if (it) it->last_ping = mono_seconds();
}

/* Anything at all from the client, including a pong, means it is still there. */
void mi_interactive_note_heard(mi_interactive *it) {
    if (it) it->last_heard = mono_seconds();
}

/* How long since the client was last heard from, in seconds. */
double mi_interactive_silence(mi_interactive *it) {
    if (!it) return 0;
    return mono_seconds() - it->last_heard;
}

int mi_interactive_silence_limit(const mi_interactive *it) {
    return it ? it->pool->cfg.keepalive_timeout_seconds : 45;
}

const unsigned char *mi_interactive_frame(mi_interactive *it, size_t *len) {
    if (len) *len = it ? it->frame_len : 0;
    return it ? it->frame : NULL;
}

const char *mi_interactive_frame_format(mi_interactive *it) {
    return it ? it->frame_format : "jpeg";
}

int mi_interactive_frame_width(mi_interactive *it) { return it ? it->frame_w : 0; }
int mi_interactive_frame_height(mi_interactive *it) { return it ? it->frame_h : 0; }

const char *mi_interactive_state(mi_interactive *it) {
    return it ? it->state_json : NULL;
}

const char *mi_interactive_close_reason(mi_interactive *it) {
    if (!it) return "no session";
    if (it->close_reason[0]) return it->close_reason;
    return it->closed ? "the session closed" : "";
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

/* A command from the user's console, which is activity. */
static void touched(mi_interactive *it) {
    if (it) it->last_activity = mono_seconds();
}

void mi_interactive_navigate(mi_interactive *it, const char *url) {
    if (!it || it->closed || !url) return;
    if (!mi_interactive_url_allowed(url)) return;
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"url\":");
    mi_json_string(&b, url);
    mi_json_raw(&b, "}");
    cdp_command(it, "Page.navigate", mi_json_data(&b));
    mi_json_free(&b);
    snprintf(it->url, sizeof it->url, "%s", url);
    set_state_changed(it);
    touched(it);
}

void mi_interactive_reload(mi_interactive *it) {
    if (!it || it->closed) return;
    cdp_command(it, "Page.reload", "{\"ignoreCache\":false}");
    touched(it);
}

void mi_interactive_resize(mi_interactive *it, int width, int height) {
    if (!it || it->closed) return;
    if (width < 64) width = 64;
    if (height < 64) height = 64;
    if (width > 4096) width = 4096;
    if (height > 4096) height = 4096;
    char params[160];
    /* The `mobile` flag is carried over, not reset: a resize must not change how
     * the page is laid out, only how big it is. Sending `false` here is what
     * turned a phone layout back into a desktop one on the first re-fit. */
    snprintf(params, sizeof params,
             "{\"width\":%d,\"height\":%d,\"deviceScaleFactor\":1,\"mobile\":%s}",
             width, height, it->mobile ? "true" : "false");
    cdp_command(it, "Emulation.setDeviceMetricsOverride", params);
    it->frame_w = width;
    it->frame_h = height;
    touched(it);
}

void mi_interactive_screenshot(mi_interactive *it) {
    if (!it || it->closed) return;
    it->shot_id = cdp_command(it, "Page.captureScreenshot", "{\"format\":\"png\"}");
    touched(it);
}

void mi_interactive_mouse(mi_interactive *it, const char *type, double x, double y,
                          const char *button, int click_count, int buttons,
                          int modifiers) {
    if (!it || it->closed || !type) return;
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"type\":");
    mi_json_string(&b, type);
    mi_json_raw(&b, ",\"x\":");
    mi_json_double(&b, x);
    mi_json_raw(&b, ",\"y\":");
    mi_json_double(&b, y);
    if (button && *button) {
        mi_json_raw(&b, ",\"button\":");
        mi_json_string(&b, button);
    }
    if (click_count > 0) {
        mi_json_raw(&b, ",\"clickCount\":");
        mi_json_int(&b, click_count);
    }
    mi_json_raw(&b, ",\"buttons\":");
    mi_json_int(&b, buttons);
    mi_json_raw(&b, ",\"modifiers\":");
    mi_json_int(&b, modifiers);
    mi_json_raw(&b, "}");
    cdp_command(it, "Input.dispatchMouseEvent", mi_json_data(&b));
    mi_json_free(&b);
    touched(it);
}

void mi_interactive_wheel(mi_interactive *it, double x, double y, double dx, double dy,
                          int modifiers) {
    if (!it || it->closed) return;
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"type\":\"mouseWheel\",\"x\":");
    mi_json_double(&b, x);
    mi_json_raw(&b, ",\"y\":");
    mi_json_double(&b, y);
    mi_json_raw(&b, ",\"deltaX\":");
    mi_json_double(&b, dx);
    mi_json_raw(&b, ",\"deltaY\":");
    mi_json_double(&b, dy);
    mi_json_raw(&b, ",\"modifiers\":");
    mi_json_int(&b, modifiers);
    mi_json_raw(&b, "}");
    cdp_command(it, "Input.dispatchMouseEvent", mi_json_data(&b));
    mi_json_free(&b);
    touched(it);
}

void mi_interactive_key(mi_interactive *it, const char *type, const char *key,
                        const char *code, int vk, const char *text, int modifiers) {
    if (!it || it->closed || !type) return;
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"type\":");
    mi_json_string(&b, type);
    if (key) { mi_json_raw(&b, ",\"key\":"); mi_json_string(&b, key); }
    if (code) { mi_json_raw(&b, ",\"code\":"); mi_json_string(&b, code); }
    if (vk > 0) { mi_json_raw(&b, ",\"windowsVirtualKeyCode\":"); mi_json_int(&b, vk); }
    if (text) { mi_json_raw(&b, ",\"text\":"); mi_json_string(&b, text); }
    mi_json_raw(&b, ",\"modifiers\":");
    mi_json_int(&b, modifiers);
    mi_json_raw(&b, "}");
    cdp_command(it, "Input.dispatchKeyEvent", mi_json_data(&b));
    mi_json_free(&b);
    touched(it);
}

void mi_interactive_text(mi_interactive *it, const char *text) {
    if (!it || it->closed || !text) return;
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"text\":");
    mi_json_string(&b, text);
    mi_json_raw(&b, "}");
    cdp_command(it, "Input.insertText", mi_json_data(&b));
    mi_json_free(&b);
    touched(it);
}

void mi_interactive_touch(mi_interactive *it, const char *type, const double *points,
                          int count, int modifiers) {
    if (!it || it->closed || !type || !points || count <= 0) return;
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"type\":");
    mi_json_string(&b, type);
    mi_json_raw(&b, ",\"touchPoints\":[");
    for (int i = 0; i < count; i++) {
        if (i) mi_json_raw(&b, ",");
        mi_json_raw(&b, "{\"x\":");
        mi_json_double(&b, points[i * 2]);
        mi_json_raw(&b, ",\"y\":");
        mi_json_double(&b, points[i * 2 + 1]);
        mi_json_raw(&b, ",\"id\":");
        mi_json_int(&b, i);
        mi_json_raw(&b, "}");
    }
    mi_json_raw(&b, "],\"modifiers\":");
    mi_json_int(&b, modifiers);
    mi_json_raw(&b, "}");
    cdp_command(it, "Input.dispatchTouchEvent", mi_json_data(&b));
    mi_json_free(&b);
    touched(it);
}

/* ------------------------------------------------------------------ */
/* Closing                                                             */
/* ------------------------------------------------------------------ */

void mi_interactive_close(mi_interactive *it) {
    if (!it) return;
    if (it->pid > 0) {
        /* Kill the whole group: politely, then firmly, then reap. Chromium's
         * helpers would otherwise reparent and, in a container, never be
         * reaped. */
        kill(-it->pid, SIGTERM);
        struct timespec brief = {.tv_sec = 0, .tv_nsec = 300 * 1000 * 1000};
        nanosleep(&brief, NULL);
        kill(-it->pid, SIGKILL);
        int status = 0;
        for (int i = 0; i < 100; i++) {
            pid_t got = waitpid(it->pid, &status, WNOHANG);
            if (got == it->pid || (got < 0 && errno != EINTR)) break;
            nanosleep(&brief, NULL);
        }
    }
    if (it->wfd >= 0) close(it->wfd);
    if (it->rfd >= 0) close(it->rfd);
    if (it->profile) mi_rm_rf(it->profile);
    free(it->profile);
    free(it->rbuf);
    free(it->wbuf);
    free(it->frame);
    free(it->state_json);
    free(it->resp_json);
    if (it->pool) {
        reap_children(it->pool);
        pool_release(it->pool);
    }
    free(it);
}
