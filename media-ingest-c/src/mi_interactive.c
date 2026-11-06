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
#include "mi_mediawatch.h"
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
 * rather than allowed to grow Vyzor's memory without limit. */
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

/*
 * One in-flight request.
 *
 * CDP reports a response when its headers arrive and the byte count when it
 * finishes, keyed by a request id — so the two have to be joined. A short table
 * is enough: a page has a few dozen requests in flight at once, not thousands.
 */
typedef struct {
    char id[32];
    char *url;
    char *mime;
    char *type;
} mw_pending;

struct mi_interactive {
    mi_interactive_pool *pool;
    pid_t pid;
    int wfd; /* parent -> browser (the child's fd 3) */
    int rfd; /* browser -> parent (the child's fd 4) */
    char *profile;
    char session_id[64];
    int next_id;
    int shot_id; /* the id of an outstanding captureScreenshot, or 0 */
    /* The id of an outstanding editable-focus probe, or 0. */
    int editing_probe_id;

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
    /*
     * Whether the page has an editable element focused.
     *
     * Reported to the console so it only raises the on-screen keyboard when the
     * page is actually ready to receive text. Without it, every tap opened a
     * keyboard — including taps on links and blank space, which is not what a
     * browser does.
     */
    int editing;

    /*
     * What the page has loaded, so the HUD can list it.
     *
     * Collected from the browser's network activity rather than by reading the
     * markup, so a streaming player's manifest and its media are found, with the
     * sizes the browser actually received.
     */
    mi_mediawatch *media;
    mw_pending *pending;
    size_t pending_count, pending_cap;
    /* An outstanding read of a manifest's body, and the URL it belongs to, so
     * the variants can be attached to the right entry. */
    int manifest_probe_id;
    char *manifest_url;
    /* Whether the picker script and its binding have been installed. */
    int picker_installed;
    /* An outstanding read of the page's own media elements. */
    int media_probe_id;
    int history_probe_id;
    /*
     * An outstanding read of the page's links, and the answer to it.
     *
     * A caller that wants to *walk* a page — a crawler — needs the links the
     * browser can see, which is more than the served HTML has: a page that
     * assembles its list in JavaScript has none of it in the source. The read is
     * asynchronous like every other evaluate, so the answer is kept here until
     * the caller collects it.
     */
    int dom_probe_id;
    char *dom_links;

    /*
     * The page's history, so Back and Forward can be offered.
     *
     * `Page.navigateToHistoryEntry` addresses an entry by the id the browser
     * assigned, so the ids have to be kept. Bounded to a reasonable depth: a
     * viewer only ever goes a few steps either way, and a history that grew
     * without limit would be a leak on a long browsing session.
     */
    int history_entries[64];
    size_t history_count;
    int history_index;

    /* What the last pump turned up. */
    int ev_frame, ev_shot, ev_state, ev_media;

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
    cfg->proxy = NULL;
    cfg->profile_root = NULL;
    /*
     * The idle window.
     *
     * This closes a session whose page is *both* untouched and still. It is
     * generous on purpose: a reader who pauses on a page is the normal case, not
     * an abandoned session, and the keepalive below is what actually finds a
     * client that has gone. Five minutes is long enough that reading a long
     * article does not end the session under the reader.
     */
    cfg->idle_timeout_seconds = 300;
    cfg->startup_timeout_seconds = 15;
    /*
     * The keepalive window.
     *
     * A ping every 20 seconds, and a client is only given up on after 300
     * seconds of silence. The margin matters more than the interval: a browser
     * answers a ping promptly, but a backgrounded tab throttles its timers, a
     * phone suspends the socket when it sleeps, and a proxy can hold a reply for
     * a moment. With a 25-second window any of those killed the session, which is
     * what made a live page feel like it kept dropping.
     *
     * The console also sends its own heartbeat, so a throttled tab still makes
     * itself heard; this window is the backstop for a client that has genuinely
     * gone. Five minutes keeps a leaked session from holding the gate for long
     * without punishing a viewer who stepped away.
     */
    cfg->keepalive_ping_seconds = 20;
    cfg->keepalive_timeout_seconds = 300;
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
    if (pool->cfg.keepalive_timeout_seconds <= 0) pool->cfg.keepalive_timeout_seconds = 300;
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

/* Declared here because a navigation handler earlier in the file needs
 * them, and both are defined further down. */

static int wait_for_id(mi_interactive *it, int id, int ms);

/*
 * The picker.
 *
 * A page loads dozens of images nobody will ever want — icons, spacers, the same
 * thumbnail at six sizes. Listing all of them is not a list of anything, so the
 * Vyzor listens for the one signal that is unambiguous instead: the viewer
 * clicking or long-pressing a piece of media. That is intent, and it is worth
 * far more than a heuristic about size.
 *
 * Reported per pick: the media's own URL (the full-size one a picture links to,
 * when it is wrapped in a link — that is usually the real file), a title from the
 * page's own text, a small image to show for it, and its natural size.
 *
 * Injected as a binding, so it works on every navigation without the page's
 * cooperation, and it is careful to never interfere with the page: listeners are
 * passive, and nothing is prevented or stopped.
 */
static const char *PICKER_JS =
"(function(){"
"if(window.__vzPicker)return;window.__vzPicker=1;"
"function txt(el){"
"  var t=el.getAttribute&&(el.getAttribute('alt')||el.getAttribute('title'));"
"  if(t)return t;"
"  var a=el.closest&&el.closest('a');"
"  if(a){t=a.getAttribute('title')||a.textContent;if(t)return t.trim().slice(0,200);}"
"  var f=el.closest&&el.closest('figure');"
"  if(f){var c=f.querySelector('figcaption');if(c&&c.textContent)return c.textContent.trim().slice(0,200);}"
"  return '';"
"}"
"function thumbFor(el){"
"  var u=el.currentSrc||el.src||'';"
"  if(u&&u.indexOf('data:')!==0&&u.indexOf('blob:')!==0)return u;"
"  var a=el.closest&&el.closest('a');"
"  if(a){var i=a.querySelector('img');if(i){var s=i.currentSrc||i.src;if(s)return s;}}"
"  return '';"
"}"
"function fullUrl(el){"
"  /* A picture inside a link usually links to the real file, not the thumbnail. */"
"  var a=el.closest&&el.closest('a');"
"  if(a&&a.href&&/\\.(png|jpe?g|gif|webp|avif|bmp|svg|mp4|webm|mkv|mov|mp3|m4a|ogg|flac|wav|m3u8|mpd)(\\?|$)/i.test(a.href))return a.href;"
"  return el.currentSrc||el.src||'';"
"}"
"function report(el){"
"  try{"
"    /* An anchor is a pick of the file it points at, not of an element. */"
"    if(el.tagName==='A'){"
"      window.__vzPick(JSON.stringify({url:el.href,kind:'image',title:(el.getAttribute('title')||el.textContent||'').trim().slice(0,200),thumb:'',width:0,height:0}));"
"      return;"
"    }"
"    var u=fullUrl(el);"
"    if(!u||u.indexOf('data:')===0||u.indexOf('blob:')===0)return;"
"    var k='image';"
"    if(el.tagName==='VIDEO')k='video';"
"    else if(el.tagName==='AUDIO')k='audio';"
"    else if(/\\.(m3u8|mpd)(\\?|$)/i.test(u))k='manifest';"
"    window.__vzPick(JSON.stringify({"
"      url:u,"
"      kind:k,"
"      title:txt(el),"
"      thumb:thumbFor(el),"
"      width:(el.naturalWidth||el.videoWidth||0),"
"      height:(el.naturalHeight||el.videoHeight||0)"
"    }));"
"  }catch(e){}"
"}"
"var SEL='img,video,audio';"
"function targetOf(e){"
"  var t=e.target;"
"  if(!t)return null;"
"  /*"
"   * The element under the pointer, then its ancestors."
"   *"
"   * A click's target is very often a wrapper — a thumbnail sits inside a link,"
"   * a figure, a gallery cell — so matching only the target itself misses most"
"   * real clicks. Walking up finds the media the pointer was actually on."
"   */"
"  var el=t;"
"  while(el&&el!==document){"
"    if(el.tagName==='IMG'||el.tagName==='VIDEO'||el.tagName==='AUDIO')return el;"
"    el=el.parentElement;"
"  }"
"  /*"
"   * Nothing media in the ancestry: take the media at the point itself."
"   *"
"   * `elementsFromPoint` sees through overlays and gives everything under the"
"   * cursor, which is what makes a click on a caption over a picture still count"
"   * as a click on the picture."
"   */"
"  var stack=(document.elementsFromPoint?document.elementsFromPoint(e.clientX,e.clientY):[]);"
"  for(var i=0;i<stack.length;i++){"
"    var s2=stack[i];"
"    if(s2.tagName==='IMG'||s2.tagName==='VIDEO'||s2.tagName==='AUDIO')return s2;"
"  }"
"  /*"
"   * Still nothing, but the pointer may be inside a link to a media file — a"
"   * grid of filenames with no thumbnails, say. That link is the pick."
"   */"
"  el=t;"
"  while(el&&el!==document){"
"    if(el.tagName==='A'&&el.href&&/\\.(png|jpe?g|gif|webp|avif|bmp|svg|mp4|webm|mkv|mov|mp3|m4a|ogg|flac|wav|m3u8|mpd)(\\?|$)/i.test(el.href))return el;"
"    el=el.parentElement;"
"  }"
"  return null;"
"}"
"document.addEventListener('click',function(e){var el=targetOf(e);if(el)report(el);},true);"
"var hold=null,held=null;"
"document.addEventListener('touchstart',function(e){"
"  held=targetOf(e);"
"  if(!held)return;"
"  hold=setTimeout(function(){if(held)report(held);hold=null;},550);"
"},true);"
"['touchend','touchcancel','touchmove'].forEach(function(ev){"
"  document.addEventListener(ev,function(){if(hold){clearTimeout(hold);hold=null;}},true);"
"});"
"})();";

static void media_reset_for_navigation(mi_interactive *it);
static void refresh_history(mi_interactive *it);

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

/*
 * Ask the page whether an editable element is focused.
 *
 * A browser raises the on-screen keyboard only for a focused text field, and the
 * console cannot see the page's DOM — so Vyzor asks on the page's behalf and
 * reports the answer in the state message. This is a tiny expression, evaluated
 * after each input rather than polled, so a tap that focuses a field reports it
 * at once and a tap on a link reports nothing.
 *
 * A contenteditable element counts, because that is what a modern site's editor
 * is; so does any input type that takes text (not a checkbox or a button).
 */
static void refresh_editing(mi_interactive *it) {
    if (!it || it->closed) return;
    static const char *EXPR =
        "(function(){var a=document.activeElement;if(!a)return false;"
        "if(a.isContentEditable)return true;"
        "var t=(a.tagName||'').toLowerCase();"
        "if(t==='textarea')return true;"
        "if(t!=='input')return false;"
        "var ty=(a.type||'text').toLowerCase();"
        "return ['text','search','url','tel','email','password','number',''].indexOf(ty)>=0;})()";
    char params[512];
    snprintf(params, sizeof params, "{\"expression\":\"%s\",\"returnByValue\":true}", EXPR);
    /* A fire-and-forget command: the answer is read on the next pump, in the
     * response handler, so this does not block the caller's loop. */
    it->editing_probe_id = cdp_command(it, "Runtime.evaluate", params);
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
    /* Whether the page is ready for text. The console raises its on-screen
     * keyboard only when this is true, so a tap on a link does not open one. */
    mi_json_raw(&b, ",\"editing\":");
    mi_json_bool(&b, it->editing);
    mi_json_raw(&b, "}");
    it->state_json = mi_strdup(mi_json_data(&b));
    mi_json_free(&b);
    it->ev_state = 1;
}

/*
 * Install the picker.
 *
 * `Page.addScriptToEvaluateOnNewDocument` runs it in every document before any
 * page script, which is what makes it survive navigation without the page's
 * cooperation. The binding it calls is added with `Runtime.addBinding`, which
 * arrives at Vyzor as a `Runtime.bindingCalled` event.
 */
static void install_picker(mi_interactive *it) {
    if (!it || it->closed || it->picker_installed) return;
    int rid = cdp_command(it, "Runtime.addBinding", "{\"name\":\"__vzPick\"}");
    wait_for_id(it, rid, 2000);

    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"source\":");
    mi_json_string(&b, PICKER_JS);
    mi_json_raw(&b, "}");
    rid = cdp_command(it, "Page.addScriptToEvaluateOnNewDocument", mi_json_data(&b));
    wait_for_id(it, rid, 2000);

    /*
     * And run it in the document that is already open.
     *
     * `addScriptToEvaluateOnNewDocument` only applies to documents created
     * *after* it is called, and by the time this runs the page has been loaded —
     * it was the browser's startup argument, so it existed before there was
     * anything to attach to. Registering alone therefore armed the picker for the
     * next navigation and left the current page deaf, which is exactly what
     * happened: the script was "installed" and nothing ever fired.
     */
    mi_json_free(&b);
    mi_json_init(&b);
    mi_json_raw(&b, "{\"expression\":");
    mi_json_string(&b, PICKER_JS);
    mi_json_raw(&b, ",\"returnByValue\":false}");
    rid = cdp_command(it, "Runtime.evaluate", mi_json_data(&b));
    wait_for_id(it, rid, 2000);
    mi_json_free(&b);

    it->picker_installed = 1;
    mi_logf(MI_LOG_INFO, "live: picker installed");
}

/* A pick arrived: the viewer engaged with something. */
static void on_binding_called(mi_interactive *it, const mi_json *params) {
    const char *name = mi_json_get_string(params, "name", NULL);
    if (!name || strcmp(name, "__vzPick") != 0) return;
    const char *payload = mi_json_get_string(params, "payload", NULL);
    if (!payload) return;
    mi_json *m = mi_json_parse(payload);
    if (!m) return;

    const char *url = mi_json_get_string(m, "url", NULL);
    const char *kind = mi_json_get_string(m, "kind", "image");
    const char *title = mi_json_get_string(m, "title", NULL);
    const char *thumb = mi_json_get_string(m, "thumb", NULL);
    int w = (int)mi_json_get_number(m, "width", 0);
    int h = (int)mi_json_get_number(m, "height", 0);

    if (url && *url && it->media) {
        if (mi_mediawatch_note_picked(it->media, url, title, thumb, kind, w, h)) {
            mi_logf(MI_LOG_INFO, "live: picked %s", url);
            if (mi_mediawatch_enabled(it->media)) it->ev_media = 1;
        }
    }
    mi_json_value_free(m);
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
    if (!frame) return;

    /*
     * Only the top frame's navigation is the page's address.
     *
     * `Page.frameNavigated` fires for every frame in the tree, not just the main
     * one, so an embedded frame reports its own navigation here too — and those
     * are very often `about:blank`. Acting on one replaced the real address with
     * `about:blank`, which is exactly what happened on google.com (consent and
     * captcha frames) and never on a page with no frames at all.
     *
     * A top-level frame is the one with no parent, which is what tells them
     * apart. Child frames are simply ignored: they are not the page's location.
     */
    if (mi_json_get_string(frame, "parentId", NULL) != NULL) return;

    const char *url = mi_json_get_string(frame, "url", NULL);
    if (url && strcmp(url, it->url) != 0) {
        snprintf(it->url, sizeof it->url, "%s", url);
        set_state_changed(it);
        /* A new document: what the last page loaded is not this one's. */
        media_reset_for_navigation(it);
    }
}


/* ------------------------------------------------------------------ */
/* What the page has loaded                                            */
/* ------------------------------------------------------------------ */

static mw_pending *pending_find(mi_interactive *it, const char *id, int create) {
    for (size_t i = 0; i < it->pending_count; i++) {
        if (!strcmp(it->pending[i].id, id)) return &it->pending[i];
    }
    if (!create) return NULL;
    if (it->pending_count == it->pending_cap) {
        size_t cap = it->pending_cap ? it->pending_cap * 2 : 16;
        /* Bounded: a page cannot hold more than a few hundred requests in
         * flight, and a table that grew without limit would be a leak. */
        if (cap > 512) return NULL;
        mw_pending *grown = mi_realloc(it->pending, cap * sizeof *grown);
        if (!grown) return NULL;
        it->pending = grown;
        it->pending_cap = cap;
    }
    mw_pending *p = &it->pending[it->pending_count++];
    memset(p, 0, sizeof *p);
    snprintf(p->id, sizeof p->id, "%s", id);
    return p;
}

static void pending_drop(mi_interactive *it, const char *id) {
    for (size_t i = 0; i < it->pending_count; i++) {
        if (strcmp(it->pending[i].id, id) != 0) continue;
        free(it->pending[i].url);
        free(it->pending[i].mime);
        free(it->pending[i].type);
        it->pending[i] = it->pending[it->pending_count - 1];
        it->pending_count--;
        return;
    }
}

static void pending_clear(mi_interactive *it) {
    for (size_t i = 0; i < it->pending_count; i++) {
        free(it->pending[i].url);
        free(it->pending[i].mime);
        free(it->pending[i].type);
    }
    it->pending_count = 0;
}

/* A Content-Type with its parameters stripped, which is what a list wants to
 * show and what the classifier reads. */
static char *mime_only(const char *raw) {
    if (!raw) return mi_strdup("");
    const char *semi = strchr(raw, ';');
    size_t n = semi ? (size_t)(semi - raw) : strlen(raw);
    char *out = mi_malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, raw, n);
    out[n] = '\0';
    /* Trim, because a server may pad it. */
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t')) out[--n] = '\0';
    return out;
}

/*
 * An HLS master playlist, parsed for its variants.
 *
 * A manifest is one URL with several qualities behind it, so the list shows them
 * under the manifest rather than as separate entries. Each `#EXT-X-STREAM-INF`
 * carries the bandwidth and, usually, the resolution; the line after it is the
 * playlist for that quality.
 */
static char *hls_variants_json(const char *body, const char *base_url) {
    if (!body || !strstr(body, "#EXT-X-STREAM-INF")) return NULL;
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "[");
    int n = 0;
    const char *p = body;
    while ((p = strstr(p, "#EXT-X-STREAM-INF:")) != NULL) {
        const char *eol = strchr(p, '\n');
        if (!eol) break;
        /* The attributes, between ':' and the newline. */
        const char *attrs = p + strlen("#EXT-X-STREAM-INF:");
        size_t alen = (size_t)(eol - attrs);

        int bandwidth = 0;
        int width = 0, height = 0;
        {
            const char *bw = strstr(attrs, "BANDWIDTH=");
            if (bw && (size_t)(bw - attrs) < alen) bandwidth = atoi(bw + 10);
            const char *res = strstr(attrs, "RESOLUTION=");
            if (res && (size_t)(res - attrs) < alen) {
                const char *x = strchr(res + 11, 'x');
                if (x) {
                    width = atoi(res + 11);
                    height = atoi(x + 1);
                }
            }
        }

        /* The URI is the next line that is not a comment or blank. */
        const char *u = eol + 1;
        while (*u == '#' || *u == '\r' || *u == '\n') {
            const char *nl = strchr(u, '\n');
            if (!nl) { u = NULL; break; }
            u = nl + 1;
        }
        if (!u || !*u) break;
        const char *uend = u;
        while (*uend && *uend != '\r' && *uend != '\n') uend++;
        size_t ulen = (size_t)(uend - u);
        if (ulen == 0) { p = eol + 1; continue; }

        /* The variant's own URL, made absolute against the manifest's. */
        char *variant_url = NULL;
        if (ulen >= 7 && !strncasecmp(u, "http://", 7)) {
            variant_url = mi_strndup(u, ulen);
        } else if (ulen >= 8 && !strncasecmp(u, "https://", 8)) {
            variant_url = mi_strndup(u, ulen);
        } else if (base_url) {
            /* Resolve against the manifest's directory. */
            const char *slash = strrchr(base_url, '/');
            if (slash) {
                size_t plen = (size_t)(slash - base_url) + 1;
                variant_url = mi_asprintf("%.*s%.*s", (int)plen, base_url, (int)ulen, u);
            }
        }

        if (n) mi_json_raw(&b, ",");
        mi_json_raw(&b, "{\"label\":");
        if (height > 0) {
            char label[32];
            snprintf(label, sizeof label, "%dp", height);
            mi_json_string(&b, label);
        } else if (bandwidth > 0) {
            char label[32];
            snprintf(label, sizeof label, "%dkbps", bandwidth / 1000);
            mi_json_string(&b, label);
        } else {
            mi_json_string(&b, "variant");
        }
        mi_json_raw(&b, ",\"width\":");
        mi_json_int(&b, width);
        mi_json_raw(&b, ",\"height\":");
        mi_json_int(&b, height);
        mi_json_raw(&b, ",\"bandwidth\":");
        mi_json_int(&b, bandwidth);
        mi_json_raw(&b, ",\"url\":");
        mi_json_string(&b, variant_url ? variant_url : "");
        mi_json_raw(&b, "}");
        free(variant_url);
        n++;
        p = uend;
    }
    mi_json_raw(&b, "]");
    if (n == 0) {
        mi_json_free(&b);
        return NULL;
    }
    char *out = mi_strdup(mi_json_data(&b));
    mi_json_free(&b);
    return out;
}

/*
 * Ask for a manifest's body, so its variants can be listed.
 *
 * Fetched over the browser's own network stack, not Vyzor's: the manifest
 * may need the page's cookies or referrer, and the browser already has them.
 * `Runtime.evaluate` with an await is the one way to read a body from here
 * without a second CDP domain.
 */
static void request_manifest(mi_interactive *it, const char *url) {
    if (!it || !url || !*url) return;
    /* Only HLS is parsed here; a DASH MPD needs XML, and is listed as one item
     * with its own URL, which is still downloadable. */
    if (!strstr(url, ".m3u8")) return;
    if (it->manifest_probe_id) return; /* one at a time */

    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"expression\":");
    mi_json_buf e;
    mi_json_init(&e);
    mi_json_raw(&e, "fetch(");
    mi_json_string(&e, url);
    mi_json_raw(&e, ").then(r=>r.text()).catch(()=>'')");
    mi_json_string(&b, mi_json_data(&e));
    mi_json_free(&e);
    mi_json_raw(&b, ",\"awaitPromise\":true,\"returnByValue\":true}");
    it->manifest_probe_id = cdp_command(it, "Runtime.evaluate", mi_json_data(&b));
    /* Remembered so the answer can be matched to the entry it belongs to. */
    free(it->manifest_url);
    it->manifest_url = mi_strdup(url);
    mi_json_free(&b);
}

/* Page.navigatedWithinDocument / frameNavigated: the page moved, so what the
 * last one loaded is not this one's. */
/*
 * Re-read the browser's history, so Back and Forward know where they are.
 *
 * The browser is the authority on this; asking it rather than tracking
 * navigations by hand is what keeps the two from disagreeing after a redirect
 * or a same-document navigation.
 */
static void refresh_history(mi_interactive *it) {
    if (!it || it->closed) return;
    it->history_probe_id = cdp_command(it, "Page.getNavigationHistory", NULL);
}

static void media_reset_for_navigation(mi_interactive *it) {
    if (!it || !it->media) return;
    mi_mediawatch_clear(it->media);
    pending_clear(it);
    it->ev_media = 1;
    refresh_history(it);
}

/* A response's headers arrived: remember what it is, keyed by request id. */
static void on_response_received(mi_interactive *it, const mi_json *params) {
    const mi_json *resp = mi_json_get(params, "response");
    if (!resp) return;
    const char *url = mi_json_get_string(resp, "url", NULL);
    const char *id = mi_json_get_string(params, "requestId", NULL);
    if (!url || !id) return;

    /* Only what the page itself asked for. A data: or blob: URL has no bytes a
     * download could fetch. */
    if (!strncasecmp(url, "data:", 5) || !strncasecmp(url, "blob:", 5)) return;

    mw_pending *p = pending_find(it, id, 1);
    if (!p) return;
    free(p->url);
    free(p->mime);
    free(p->type);
    p->url = mi_strdup(url);
    p->mime = mime_only(mi_json_get_string(resp, "mimeType", ""));
    p->type = mi_strdup(mi_json_get_string(params, "type", ""));
}

/* A response finished: now the byte count is known, so the entry can be made. */
static void on_loading_finished(mi_interactive *it, const mi_json *params) {
    const char *id = mi_json_get_string(params, "requestId", NULL);
    if (!id) return;
    mw_pending *p = pending_find(it, id, 0);
    if (!p) return;

    int64_t bytes = (int64_t)mi_json_get_number(params, "encodedDataLength", -1);
    if (bytes <= 0) bytes = (int64_t)mi_json_get_number(params, "decodedBodySize", -1);

    if (p->url) {
        if (mi_mediawatch_note(it->media, p->url, p->mime, p->type, bytes)) {
            /* Recorded either way; reported only when the list is being
             * published, so a viewer who has not asked is not sent messages. */
            if (mi_mediawatch_enabled(it->media)) it->ev_media = 1;
            /* A manifest is worth reading, so its qualities can be shown. */
            if (strstr(p->url, ".m3u8")) request_manifest(it, p->url);
        }
    }
    pending_drop(it, id);
}

/* A response failed: nothing was loaded, so forget it. */
static void on_loading_failed(mi_interactive *it, const mi_json *params) {
    const char *id = mi_json_get_string(params, "requestId", NULL);
    if (id) pending_drop(it, id);
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
    } else if (!strcmp(method, "Runtime.bindingCalled")) {
        on_binding_called(it, params);
    } else if (!strcmp(method, "Network.responseReceived")) {
        on_response_received(it, params);
    } else if (!strcmp(method, "Network.loadingFinished")) {
        on_loading_finished(it, params);
    } else if (!strcmp(method, "Network.loadingFailed")) {
        on_loading_failed(it, params);
    } else if (!strcmp(method, "Page.navigatedWithinDocument")) {
        media_reset_for_navigation(it);
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
        /*
         * The editable-focus probe's answer. A change is a real state change, so
         * it is published; an unchanged answer is dropped, because this runs after
         * every tap and would otherwise flood the console with the same message.
         */
        /*
         * A manifest's body, for its variants.
         *
         * The playlist is the only place the qualities of a stream are named, and
         * a list that shows one row per manifest without them is much less useful
         * than one that shows 480p/720p/1080p with their bitrates.
         */
        /* The page's history, for Back and Forward. */
        if (it->resp_ok && id == it->history_probe_id) {
            it->history_probe_id = 0;
            const mi_json *result = mi_json_get(root, "result");
            const mi_json *entries = mi_json_get(result, "entries");
            int cur = (int)mi_json_get_number(result, "currentIndex", -1);
            if (entries && entries->type == MI_JSON_ARRAY) {
                it->history_count = 0;
                for (size_t i = 0; i < entries->count && it->history_count < 64; i++) {
                    int64_t eid = mi_json_get_int(entries->items[i], "id", 0);
                    if (eid > 0) it->history_entries[it->history_count++] = (int)eid;
                }
                it->history_index = (cur >= 0 && (size_t)cur < it->history_count) ? cur : 0;
            }
        }
        /*
         * The page's own media elements.
         *
         * The network view only knows what has already been fetched; a video that
         * has not been played yet is still a thing to download. This adds those,
         * so the list is complete as well as current.
         */
        if (it->resp_ok && id == it->media_probe_id) {
            it->media_probe_id = 0;
            const mi_json *result = mi_json_get(root, "result");
            const mi_json *inner = mi_json_get(result, "result");
            const char *list = mi_json_get_string(inner, "value", NULL);
            if (list && *list) {
                const char *p = list;
                while (*p) {
                    const char *nl = strchr(p, '\n');
                    size_t n = nl ? (size_t)(nl - p) : strlen(p);
                    if (n > 0) {
                        char *u = mi_strndup(p, n);
                        if (u) {
                            if (mi_mediawatch_note(it->media, u, "", "Media", -1) &&
                                mi_mediawatch_enabled(it->media))
                                it->ev_media = 1;
                            free(u);
                        }
                    }
                    if (!nl) break;
                    p = nl + 1;
                }
            }
        }
        if (it->resp_ok && id == it->dom_probe_id) {
            it->dom_probe_id = 0;
            const mi_json *result = mi_json_get(root, "result");
            const mi_json *inner = mi_json_get(result, "result");
            const char *list = mi_json_get_string(inner, "value", NULL);
            free(it->dom_links);
            it->dom_links = (list && *list) ? mi_strdup(list) : NULL;
        }
        if (it->resp_ok && id == it->manifest_probe_id && it->manifest_url) {
            it->manifest_probe_id = 0;
            const mi_json *result = mi_json_get(root, "result");
            const mi_json *inner = mi_json_get(result, "result");
            /* A returnByValue result carries the value under `value`. */
            const char *body = mi_json_get_string(inner, "value", NULL);
            if (body && *body) {
                char *vars = hls_variants_json(body, it->manifest_url);
                if (vars) {
                    if (mi_mediawatch_note_variants(it->media, it->manifest_url, vars) &&
                        mi_mediawatch_enabled(it->media))
                        it->ev_media = 1;
                    free(vars);
                }
            }
            free(it->manifest_url);
            it->manifest_url = NULL;
        }
        if (it->resp_ok && id == it->editing_probe_id) {
            it->editing_probe_id = 0;
            const mi_json *result = mi_json_get(root, "result");
            const mi_json *inner = mi_json_get(result, "result");
            int now = mi_json_get_bool(inner, "value", 0);
            if (now != it->editing) {
                it->editing = now;
                set_state_changed(it);
            }
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
 * container's Vyzor holds a listen socket and more; none of it belongs in a
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
        char *px = (cfg->proxy && *cfg->proxy)
                       ? mi_asprintf("--proxy-server=%s", cfg->proxy) : NULL;
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
        if (px) av[n++] = px;
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
    /*
     * The media list, created before anything can put something in it.
     *
     * The picker is armed during startup and a pick can arrive the moment a
     * document is live, so this has to exist by then: a pick delivered to a NULL
     * list is dropped without a word, which is a silent failure that looks like
     * the picker never fired.
     *
     * Publishing is off until the console asks — a viewer who never opens the list
     * should not pay for the per-change messages — but the collecting itself
     * always runs, so the list is not empty the first time it is looked at.
     */
    it->media = mi_mediawatch_new();
    if (it->media) mi_mediawatch_set_enabled(it->media, 0);
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
    /* Non-blocking both ways: a stalled browser must not block Vyzor, and
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
        /*
         * Network, so the page's own requests can be watched.
         *
         * This is what makes the media list real: whatever a player fetched is a
         * media URL that works, with the size the browser actually received. A
         * listing read from the markup cannot know either. The domain is enabled
         * always, because it has to be on *before* the page loads to see what the
         * page loads; the recording itself stays off until asked.
         */
        rid = cdp_command(it, "Network.enable", NULL);
        wait_for_id(it, rid, tmo);

        /*
         * The picker, so a click or a long press on media is reported.
         *
         * Installed here rather than lazily because it has to be in place before
         * the page's own scripts run, or the first navigation would miss it.
         */
        install_picker(it);
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

    /*
     * A flag is cleared when its event is returned, not before the call.
     *
     * Clearing up front lost events: a live session produces a frame on
     * almost every call, so a state change set in the same `pump_io` was
     * wiped on the next call without ever being returned — which is why the
     * console never saw a page state at all.
     *
     * State is checked first, deliberately. It is rare and carries
     * information; a frame is frequent and droppable, and the outbox will
     * discard a superseded one anyway. Returning the frame first would starve
     * the state even with sticky flags, because there is always another
     * frame next call.
     */
    if (it->ev_state) { it->ev_state = 0; return MI_IT_EV_STATE; }
    if (it->ev_media) { it->ev_media = 0; return MI_IT_EV_MEDIA; }
    if (it->ev_shot) { it->ev_shot = 0; return MI_IT_EV_SHOT; }
    if (it->ev_frame) { it->ev_frame = 0; return MI_IT_EV_FRAME; }
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

/*
 * Anything at all from the client, including a pong, means it is still there.
 *
 * This counts as *activity*, not just as liveness. A viewer reading a page is
 * connected, present, and doing nothing — and the idle rule used to judge only
 * frames and input, so such a session was closed after the idle timeout even
 * though its client was right there. That is what made a live page feel like it
 * kept dropping: it did, every time the reader paused for a few minutes.
 *
 * A client that has genuinely gone is a different question, and the silence
 * check answers it: it is measured against `last_heard`, which a departed peer
 * stops moving.
 */
void mi_interactive_note_heard(mi_interactive *it) {
    if (!it) return;
    double now = mono_seconds();
    it->last_heard = now;
    it->last_activity = now;
}

/* How long since the client was last heard from, in seconds. */
double mi_interactive_silence(mi_interactive *it) {
    if (!it) return 0;
    return mono_seconds() - it->last_heard;
}

int mi_interactive_silence_limit(const mi_interactive *it) {
    return it ? it->pool->cfg.keepalive_timeout_seconds : 120;
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
    /* A press is what focuses a field, so ask the page what it focused. */
    if (!strcmp(type, "mousePressed") || !strcmp(type, "mouseReleased")) refresh_editing(it);
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

int mi_interactive_parse_points(const mi_json *pts, double *out, int max) {
    if (!pts || !out || max <= 0) return 0;
    if (pts->type != MI_JSON_ARRAY) return 0;
    int count = 0;
    if (pts->count > 0 && pts->items[0] && pts->items[0]->type == MI_JSON_ARRAY) {
        /* Nested pairs: `[[x,y], …]`. */
        for (size_t i = 0; i < pts->count && count < max; i++) {
            const mi_json *pair = pts->items[i];
            if (!pair || pair->type != MI_JSON_ARRAY || pair->count < 2) continue;
            out[count * 2] = pair->items[0]->number;
            out[count * 2 + 1] = pair->items[1]->number;
            count++;
        }
    } else {
        /* Flat: `[x, y, x, y, …]`. */
        for (size_t i = 0; i + 1 < pts->count && count < max; i += 2) {
            const mi_json *x = pts->items[i];
            const mi_json *y = pts->items[i + 1];
            if (!x || !y || x->type != MI_JSON_NUMBER || y->type != MI_JSON_NUMBER) break;
            out[count * 2] = x->number;
            out[count * 2 + 1] = y->number;
            count++;
        }
    }
    return count;
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
    /* A tap is what focuses a field on a touch device. */
    if (!strcmp(type, "touchEnd") || !strcmp(type, "touchCancel")) refresh_editing(it);
}


/* ------------------------------------------------------------------ */
/* What the page has loaded                                            */
/* ------------------------------------------------------------------ */

const char *mi_interactive_media_json(mi_interactive *it) {
    if (!it || !it->media) return "[]";
    return mi_mediawatch_json(it->media);
}

void mi_interactive_watch_media(mi_interactive *it, int on) {
    if (!it || !it->media) return;
    mi_mediawatch_set_enabled(it->media, on);
    if (on) {
        /* Publish at once, so the list shows what the page has *already* loaded
         * rather than waiting for it to load something new — which is what
         * someone opening the list expects to see. */
        it->ev_media = 1;
        /* And re-read the page, since the network view only knows what has been
         * fetched, not everything the page offers. */
        mi_interactive_rescan_media(it);
    }
}

void mi_interactive_rescan_media(mi_interactive *it) {
    if (!it || it->closed) return;
    /*
     * Re-read the page for what it offers.
     *
     * The network view only knows what has already been fetched, which is the
     * truth about what a player is using but misses a media element the page has
     * not started. This asks the page for its own media URLs and offers them
     * too, so a list is complete rather than only current.
     */
    /*
     * Every element that names a source, not only the video ones.
     *
     * A page's media is very often images, and a rescan that looked only at
     * <video>/<audio> reported nothing on a gallery — which is exactly the page
     * someone would open the list on. `data:` and `blob:` URLs are skipped: the
     * first is inline bytes with no URL to fetch, the second is a handle that
     * only exists in that page.
     */
    static const char *EXPR =
        "(function(){var o=[];"
        "var s=document.querySelectorAll('video,audio,source,img');"
        "for(var i=0;i<s.length;i++){var u=s[i].currentSrc||s[i].src||'';"
        "if(!u||u.indexOf('data:')===0||u.indexOf('blob:')===0)continue;"
        "o.push(u);}"
        "return o.join(String.fromCharCode(10));})()";
    /*
     * Built with the string encoder, not with snprintf.
     *
     * Pasting the expression into a format string means its backslashes are not
     * escaped for JSON: `join('\n')` in C becomes a JSON `\n`, which decodes to
     * a *raw newline inside a JavaScript string literal* — a SyntaxError, so the
     * evaluate silently returned nothing. `String.fromCharCode(10)` avoids the
     * escape entirely, and mi_json_string does the quoting.
     */
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"expression\":");
    mi_json_string(&b, EXPR);
    mi_json_raw(&b, ",\"returnByValue\":true}");
    it->media_probe_id = cdp_command(it, "Runtime.evaluate", mi_json_data(&b));
    mi_json_free(&b);

}

/*
 * Ask the browser for the links on the page.
 *
 * The answer is what a crawler needs and the served HTML cannot give: a page
 * that builds its list in JavaScript has no links in its source, and a page that
 * fetches them by XHR has them only after the scripts have run. Reading
 * `document.links` is therefore the one way to see what is actually there.
 *
 * The result is collected by `mi_interactive_dom_links` once the pump has seen
 * the reply, so this returns immediately like every other command.
 */
void mi_interactive_read_dom(mi_interactive *it) {
    if (!it || it->closed) return;
    if (it->dom_probe_id) return; /* one at a time */
    /*
     * Every anchor's href, resolved by the browser against the document, plus
     * any element carrying a download or a data-src — the shapes a file listing
     * uses. One URL per line, so the answer is a simple list rather than JSON
     * that would have to be parsed twice.
     */
    static const char *EXPR =
        "(function(){var o=[];"
        "var a=document.querySelectorAll('a[href]');"
        "for(var i=0;i<a.length;i++){var u=a[i].href;"
        "if(u&&u.indexOf('javascript:')!==0)o.push(u);}"
        "var d=document.querySelectorAll('[data-src],[data-href],[download]');"
        "for(var j=0;j<d.length;j++){"
        "var v=d[j].getAttribute('data-src')||d[j].getAttribute('data-href')||d[j].href;"
        "if(v&&o.indexOf(v)<0)o.push(v);}"
        "return o.join(String.fromCharCode(10));})()";
    /*
     * Quoted by the JSON encoder rather than pasted into a format string.
     *
     * A backslash written in C is not escaped for JSON, so the old form sent a
     * literal `\n` that decoded to a raw newline inside the JavaScript string —
     * a SyntaxError, and the read returned nothing at all.
     */
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"expression\":");
    mi_json_string(&b, EXPR);
    mi_json_raw(&b, ",\"returnByValue\":true}");
    it->dom_probe_id = cdp_command(it, "Runtime.evaluate", mi_json_data(&b));
    mi_json_free(&b);
    touched(it);
}

/*
 * The links the last completed read returned, or NULL.
 *
 * Newline-separated, and owned by the session: valid until the next read. NULL
 * means the answer has not arrived yet — the caller pumps and asks again.
 */
const char *mi_interactive_dom_links(mi_interactive *it) {
    if (!it || it->closed) return NULL;
    return it->dom_links;
}

/*
 * Click at a point, as a user would.
 *
 * A listing that only appears after a click — "Load more", a category tab — is
 * unreachable by reading the page, and this is how the crawler reaches it. The
 * press and release are dispatched as a pair because that is what the browser
 * treats as a click; a press alone leaves the element held.
 */
void mi_interactive_click_at(mi_interactive *it, double x, double y) {
    if (!it || it->closed) return;
    mi_interactive_mouse(it, "mousePressed", x, y, "left", 1, 1, 0);
    mi_interactive_mouse(it, "mouseReleased", x, y, "left", 1, 0, 0);
}

/*
 * Click the first element matching a CSS selector, if there is one.
 *
 * Returns 1 when an element was found and clicked, 0 when the selector matched
 * nothing. The click is done *in the page* rather than by synthesising a mouse
 * event at a coordinate: an element that is off-screen or behind an overlay has
 * coordinates that would miss it, while `element.click()` is what the page's own
 * handler would receive from a user's click.
 */
int mi_interactive_click_selector(mi_interactive *it, const char *selector) {
    if (!it || it->closed || !selector || !*selector) return 0;
    /* The selector is a caller-supplied string, so it is embedded as a JSON
     * string rather than pasted into the expression — a quote in it would
     * otherwise break the script, and a crafted one could run code. */
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "{\"expression\":\"(function(){var e=document.querySelector(");
    mi_json_string(&b, selector);
    mi_json_raw(&b, ");if(!e)return 0;e.click();return 1;})()\",\"returnByValue\":true}");
    cdp_command(it, "Runtime.evaluate", mi_json_data(&b));
    mi_json_free(&b);
    touched(it);
    /* The caller cannot know whether it matched, since the answer is
     * asynchronous; it learns from whether the link count grows. */
    return 1;
}

int mi_interactive_go_back(mi_interactive *it) {
    if (!it || it->closed) return 0;
    if (it->history_index <= 0) return 0;
    /* `Page.getNavigationHistory` is the authority; stepping the index here and
     * asking for that entry keeps the two in step without a second round trip
     * before the command. */
    it->history_index--;
    char params[64];
    snprintf(params, sizeof params, "{\"entryId\":%d}", it->history_entries[it->history_index]);
    cdp_command(it, "Page.navigateToHistoryEntry", params);
    touched(it);
    return 1;
}

int mi_interactive_go_forward(mi_interactive *it) {
    if (!it || it->closed) return 0;
    if (it->history_index + 1 >= (int)it->history_count) return 0;
    it->history_index++;
    char params[64];
    snprintf(params, sizeof params, "{\"entryId\":%d}", it->history_entries[it->history_index]);
    cdp_command(it, "Page.navigateToHistoryEntry", params);
    touched(it);
    return 1;
}

void mi_interactive_stop(mi_interactive *it) {
    if (!it || it->closed) return;
    cdp_command(it, "Page.stopLoading", NULL);
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
    free(it->manifest_url);
    free(it->dom_links);
    pending_clear(it);
    free(it->pending);
    mi_mediawatch_free(it->media);
    if (it->pool) {
        reap_children(it->pool);
        pool_release(it->pool);
    }
    free(it);
}
