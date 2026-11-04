/* mi_interactive.h - a live, driveable page: Chromium over CDP.
 *
 * WHAT THIS IS FOR
 *
 * The renderer (mi_render.[ch]) draws a page once and hands back a picture. That
 * is the wrong shape for a page you want to *use*: you cannot type in its search
 * box, follow a link, or see it update. This module keeps a browser alive and
 * speaks the Chrome DevTools Protocol to it, so the console can stream the page
 * and forward the user's input back.
 *
 * HOW IT TALKS TO THE BROWSER
 *
 * Over --remote-debugging-pipe, not a TCP port. That matters: a debugging port
 * is an unauthenticated control socket that any local process (or, with a
 * careless bind, anything that can reach the host) could drive, and it is one
 * more thing to firewall. A pipe is private to this process by construction.
 * Chromium reads CDP messages from file descriptor 3 and writes them to 4, each
 * a UTF-8 JSON object terminated by a NUL byte.
 *
 * WHAT BOUNDS IT
 *
 * The same honesty as the renderer applies: an unprivileged container cannot use
 * Chromium's namespace sandbox, so it runs --no-sandbox, and a page that escapes
 * the renderer escapes onto this host. The bounds that are real are: a
 * concurrency gate (each browser costs a few hundred MB), an idle timeout that
 * closes a session nobody is watching, a V8 heap cap, and a hard wall-clock
 * limit on the synchronous startup exchange. The console must only drive pages
 * the operator would be willing to visit.
 */
#ifndef MI_INTERACTIVE_H
#define MI_INTERACTIVE_H

#include <stddef.h>
#include <stdint.h>

#include "mi_json.h"

typedef enum {
    MI_IT_OK = 0,
    MI_IT_BUSY,       /* the gate is full; try again shortly */
    MI_IT_NO_BROWSER, /* no Chromium on this host */
    MI_IT_BAD_URL,    /* not a renderable http(s) URL */
    MI_IT_FAILED      /* the browser could not be started or spoken to */
} mi_it_status;

typedef struct {
    const char *chromium_path; /* NULL searches the usual names */
    int max_concurrent;        /* sessions at once; each is a whole browser */
    int v8_heap_mb;            /* per-session V8 heap cap */
    int viewport_width;
    int viewport_height;
    const char *user_agent;    /* NULL uses the engine's default */
    const char *profile_root;  /* where throwaway profiles are made */
    /*
     * Close a session that has had no input and produced no frame for this many
     * seconds. A live browser left open is a leak the size of a browser; this is
     * what stops a forgotten tab from holding one forever. 0 disables it.
     */
    int idle_timeout_seconds;
    /* How long the startup exchange (attach, enable, navigate) may take. */
    int startup_timeout_seconds;
    /*
     * The keepalive, which catches a client that vanished without a close — a
     * tab closed behind a proxy leaves a connection that never reports EOF, and
     * that session would otherwise hold the gate for as long as its page keeps
     * painting. Ping when the socket has been quiet this long, and drop the
     * client once it has been silent this long. 0 uses the built-in default.
     */
    int keepalive_ping_seconds;
    int keepalive_timeout_seconds;
} mi_interactive_config;

void mi_interactive_config_init(mi_interactive_config *cfg);

typedef struct mi_interactive_pool mi_interactive_pool;

mi_interactive_pool *mi_interactive_pool_new(const mi_interactive_config *cfg);
void mi_interactive_pool_free(mi_interactive_pool *pool);

int mi_interactive_pool_available(const mi_interactive_pool *pool);
int mi_interactive_pool_limit(const mi_interactive_pool *pool);
/* How many sessions are live right now, for reporting. */
int mi_interactive_pool_active(const mi_interactive_pool *pool);

/* ------------------------------------------------------------------ */
/* A session                                                           */
/* ------------------------------------------------------------------ */

typedef struct mi_interactive mi_interactive;

/* What a pump turned up. */
typedef enum {
    MI_IT_EV_NONE = 0, /* nothing within the wait; call again */
    MI_IT_EV_FRAME,    /* a screencast frame is ready */
    MI_IT_EV_SHOT,     /* a full-resolution screenshot is ready */
    MI_IT_EV_STATE,    /* the page's url/title/loading changed */
    MI_IT_EV_MEDIA,    /* the set of downloadable media changed */
    MI_IT_EV_CLOSED    /* the browser went away, or the deadline passed */
} mi_it_event;

/*
 * Start a session on `url`. Blocks until the browser is up and attached, or the
 * startup deadline passes. On failure returns a status other than MI_IT_OK and,
 * when `err` is given, a sentence explaining it. The session takes a gate slot
 * for its whole life; release it with mi_interactive_close.
 *
 * `touch` says the client is a touch device, so the browser emulates touch and
 * the page renders its touch layout. Without it the page believes it is on a
 * desktop and a forwarded finger reads as a mouse, which is not what a phone
 * user sees in any other browser.
 */
mi_it_status mi_interactive_open(mi_interactive_pool *pool, const char *url,
                                 int width, int height, int touch, mi_interactive **out,
                                 char *err, size_t errcap);

/*
 * Read from the browser for up to `ms` and report what arrived.
 *
 * Also ages the session: an idle one is closed here, which is why every caller
 * must keep calling it even when the user is quiet. A closed session keeps
 * returning MI_IT_EV_CLOSED so a caller notices.
 */
mi_it_event mi_interactive_pump(mi_interactive *it, int ms);

/* The last frame from a pump. Valid until the next pump. */
const unsigned char *mi_interactive_frame(mi_interactive *it, size_t *len);
/* "jpeg" or "png", matching the last frame. */
const char *mi_interactive_frame_format(mi_interactive *it);
int mi_interactive_frame_width(mi_interactive *it);
int mi_interactive_frame_height(mi_interactive *it);

/*
 * The keepalive, for a caller that owns the client socket.
 *
 * A browser that closes a tab does not always send a WebSocket close, and a
 * proxy between it and the engine can hold the connection open regardless — so
 * the engine cannot rely on the close to learn the user is gone. It pings, and a
 * client that never answers is dropped, which is what stops a vanished tab from
 * holding the concurrency gate forever.
 */
int mi_interactive_should_ping(mi_interactive *it);
void mi_interactive_note_ping(mi_interactive *it);
/* Record that anything arrived from the client, a pong included. */
void mi_interactive_note_heard(mi_interactive *it);
/* Seconds since the client was last heard from. */
double mi_interactive_silence(mi_interactive *it);
/* How long a silent client is tolerated, for the caller's own check. */
int mi_interactive_silence_limit(const mi_interactive *it);

/* The page's state as a JSON object, or NULL when it has not changed. Valid
 * until the next pump. */
const char *mi_interactive_state(mi_interactive *it);

/*
 * The media the page has actually loaded, as a JSON array.
 *
 * Taken from the browser's own network activity rather than by re-reading the
 * HTML: whatever a player fetched is a real media URL, with a real size and a
 * real content type, and it is what a download would use. A page that streams
 * (an HLS or DASH manifest, a progressive MP4) therefore lists exactly what it
 * played, which is the thing a listing of <video src> cannot know.
 *
 * Owned by the session; valid until the next pump.
 */
const char *mi_interactive_media_json(mi_interactive *it);

/* Ask the browser to re-read what is on the page, so a listing is refreshed
 * after the user has interacted with it. */
void mi_interactive_rescan_media(mi_interactive *it);

/*
 * Whether to collect media at all. Off until asked, because the collection is
 * per-resource bookkeeping on every request the page makes, and a viewer that
 * never opens the HUD should not pay for it.
 */
void mi_interactive_watch_media(mi_interactive *it, int on);

/* Why the session closed, for a farewell message. Never NULL. */
const char *mi_interactive_close_reason(mi_interactive *it);

/* --- Input, forwarded to the browser. All are no-ops once closed. --- */

void mi_interactive_navigate(mi_interactive *it, const char *url);
void mi_interactive_reload(mi_interactive *it);
void mi_interactive_resize(mi_interactive *it, int width, int height);

/* --- Navigation, as a browser's own controls. All are no-ops once closed. --- */

/*
 * Go back or forward in the page's own history.
 *
 * `Page.navigateToHistoryEntry` needs an entry id, so the engine keeps the
 * browser's history in step from `Page.frameNavigated` and the frame tree. The
 * return says whether there was somewhere to go, so a control can be disabled
 * rather than doing nothing.
 */
int mi_interactive_go_back(mi_interactive *it);
int mi_interactive_go_forward(mi_interactive *it);

/* Stop loading the page. */
void mi_interactive_stop(mi_interactive *it);
void mi_interactive_screenshot(mi_interactive *it);

/* A mouse event. `type` is a CDP mouse type (mousePressed/mouseReleased/
 * mouseMoved/mouseWheel). Unknown types are refused by the browser, not here. */
void mi_interactive_mouse(mi_interactive *it, const char *type, double x, double y,
                          const char *button, int click_count, int buttons,
                          int modifiers);
/* A wheel event: a mouse event with deltas, kept separate for clarity. */
void mi_interactive_wheel(mi_interactive *it, double x, double y, double dx, double dy,
                          int modifiers);
/* A key event. `text` may be NULL. `vk` of 0 omits the virtual key code. */
void mi_interactive_key(mi_interactive *it, const char *type, const char *key,
                        const char *code, int vk, const char *text, int modifiers);
/* Committed text (IME, paste), which is not a key sequence. */
void mi_interactive_text(mi_interactive *it, const char *text);
/* A touch event. `points` is a NULL-terminated array of x,y pairs. */
void mi_interactive_touch(mi_interactive *it, const char *type, const double *points,
                          int count, int modifiers);

/*
 * Read touch points out of a client message.
 *
 * Accepts both shapes a client may send, because getting this wrong is silent:
 *
 *   flat    `[x, y, x, y, …]`      — what the console sends, and the natural
 *                                    shape for building a CDP point list
 *   nested  `[[x, y], [x, y], …]`
 *
 * Returns the number of points written to `out`, which must hold `2 * max`.
 * Extracted from the WebSocket handler so it can be tested without a socket: an
 * earlier version accepted only the nested form, so every touch the console sent
 * parsed to zero points and the page received nothing at all.
 */
int mi_interactive_parse_points(const struct mi_json *points, double *out, int max);

/* Whether this URL may be driven. Same rules as the renderer. */
int mi_interactive_url_allowed(const char *url);

/*
 * Close the session and release its slot. Idempotent; safe on NULL.
 *
 * The browser is killed as a whole process group and reaped, because Chromium's
 * helpers outlive a naive kill and a container's PID 1 does not reap them.
 */
void mi_interactive_close(mi_interactive *it);

#endif /* MI_INTERACTIVE_H */
