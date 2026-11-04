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
    MI_IT_EV_CLOSED    /* the browser went away, or the deadline passed */
} mi_it_event;

/*
 * Start a session on `url`. Blocks until the browser is up and attached, or the
 * startup deadline passes. On failure returns a status other than MI_IT_OK and,
 * when `err` is given, a sentence explaining it. The session takes a gate slot
 * for its whole life; release it with mi_interactive_close.
 */
mi_it_status mi_interactive_open(mi_interactive_pool *pool, const char *url,
                                 int width, int height, mi_interactive **out,
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

/* The page's state as a JSON object, or NULL when it has not changed. Valid
 * until the next pump. */
const char *mi_interactive_state(mi_interactive *it);

/* Why the session closed, for a farewell message. Never NULL. */
const char *mi_interactive_close_reason(mi_interactive *it);

/* --- Input, forwarded to the browser. All are no-ops once closed. --- */

void mi_interactive_navigate(mi_interactive *it, const char *url);
void mi_interactive_reload(mi_interactive *it);
void mi_interactive_resize(mi_interactive *it, int width, int height);
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
