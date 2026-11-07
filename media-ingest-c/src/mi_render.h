/* mi_render.h - render a page with headless Chromium.
 *
 * This exists because a page is not its bytes. A site that builds itself with
 * JavaScript — a search box, an app shell — arrives as a document whose script
 * must run before there is anything to show, and a site that forbids framing
 * still has to be *rendered*, not embedded. So Vyzor runs a real browser
 * and returns either a picture of the page or its rendered DOM.
 *
 * WHAT THIS IS NOT
 *
 * A render is a resource-bounded subprocess, not a security sandbox. An
 * unprivileged container cannot use Chromium's namespace sandbox, so it runs
 * with --no-sandbox: a page that escapes the renderer escapes onto this host.
 * The bounds that are real are resource bounds — a hard wall-clock kill, a V8
 * heap cap, a concurrency gate, an output size cap, and a throwaway profile per
 * render. Run Vyzor where nothing sensitive lives.
 */
#ifndef MI_RENDER_H
#define MI_RENDER_H

#include <stddef.h>
#include <stdint.h>

/* What to produce. */
typedef enum {
    MI_RENDER_IMAGE = 0, /* a PNG of the page */
    MI_RENDER_DOM        /* the rendered HTML, after scripts have run */
} mi_render_mode;

typedef enum {
    MI_RENDER_OK = 0,
    MI_RENDER_BUSY,      /* the concurrency gate refused; try again shortly */
    MI_RENDER_TIMEOUT,   /* killed at the deadline */
    MI_RENDER_FAILED,    /* Chromium exited non-zero, or wrote nothing */
    MI_RENDER_TOO_LARGE, /* the output exceeded the cap */
    MI_RENDER_BAD_URL,   /* not http(s), or a refused host */
    MI_RENDER_NO_BROWSER /* no Chromium on this host */
} mi_render_status;

typedef struct {
    /* Chromium to run. NULL searches the usual names. */
    const char *chromium_path;
    /* Hard wall-clock limit for one render, in seconds.
     *
     * This is the limit that matters. Chromium's own --virtual-time-budget
     * bounds *page* time and does nothing about a page that never settles: a
     * request to a black-holed address ran until this killed it. */
    int timeout_seconds;
    /* Cap on the bytes returned. A page's DOM can be enormous. */
    size_t max_output_bytes;
    int viewport_width;
    int viewport_height;
    /* Per-render V8 heap cap, in MB. This is how memory is bounded: `ulimit -v`
     * cannot be used, because Chromium reserves a huge virtual address space and
     * dies at once under it. */
    int v8_heap_mb;
    /* How many renders may run at the same time. Each costs roughly 450-550 MB
     * of proportional set size, so this is the knob that decides the host's
     * memory requirement, and the default is deliberately 1. */
    int max_concurrent;
    /* Sent to the site. NULL uses Vyzor's default. */
    const char *user_agent;
    /*
     * A proxy the browser sends its requests through, e.g.
     * "http://host:port" or "socks5://host:port". Passed to Chromium as
     * --proxy-server, so a rendered page reaches a host that refuses this
     * address. NULL is direct.
     */
    const char *proxy;
    /* Where throwaway browser profiles are made. NULL uses the system temp dir. */
    const char *profile_root;
    /*
     * Where rendered pages are cached, and how much may be kept.
     *
     * A render costs about half a gigabyte and a second or two, and the same
     * page is often looked at twice. `cache_dir` NULL turns caching off;
     * otherwise the budget bounds it, because an unbounded cache is a slow disk
     * leak. `cache_ttl_seconds` stops a page that has changed being served from
     * the cache.
     */
    const char *cache_dir;
    size_t cache_max_bytes;
    size_t cache_max_entries;
    int cache_ttl_seconds;
} mi_render_config;

void mi_render_config_init(mi_render_config *cfg);

typedef struct {
    mi_render_status status;
    /* The PNG or the HTML. Owned; free with mi_render_result_free. */
    unsigned char *data;
    size_t len;
    /* "image/png" or "text/html; charset=utf-8". */
    const char *content_type;
    /* The page's <title>, when the output had one. May be NULL. */
    char *title;
    /* Wall-clock time the render took, whether it succeeded or not. */
    double elapsed_seconds;
    /* 1 when the output was cut off at max_output_bytes. */
    int truncated;
    /* 1 when this came from the cache rather than a fresh render. */
    int from_cache;
    /* Why it failed, for a log or a message. May be NULL. */
    char *error;
} mi_render_result;

void mi_render_result_free(mi_render_result *res);

/* A human-readable name for a status, for logs and messages. */
const char *mi_render_status_name(mi_render_status s);

/*
 * The renderer, and the gate that bounds it.
 *
 * A pool owns the configuration and the concurrency limit, so the limit is a
 * property of the process rather than of a request — which is what it has to be,
 * since the server itself spawns a thread per connection with no cap of its own.
 *
 * Creating a pool also makes this process a child subreaper, so Chromium's
 * orphaned helpers reparent here and can be reaped. Without that they reparent
 * to PID 1, which in a container commonly does not reap them, and the process
 * table fills with zombies.
 */
typedef struct mi_render_pool mi_render_pool;

mi_render_pool *mi_render_pool_new(const mi_render_config *cfg);
void mi_render_pool_free(mi_render_pool *pool);

/* Whether a browser was found. A pool exists either way; a render reports
 * MI_RENDER_NO_BROWSER when this is 0. */
int mi_render_pool_available(const mi_render_pool *pool);

/* How many renders may run at once, for reporting. */
int mi_render_pool_limit(const mi_render_pool *pool);

/* Cache figures for reporting: entries, bytes, hits and misses. Any may be NULL.
 * `hits` and `misses` are 0 when caching is off. */
void mi_render_pool_cache_stats(const mi_render_pool *pool, size_t *entries,
                                size_t *bytes, uint64_t *hits, uint64_t *misses);

/* Whether this pool caches at all. */
int mi_render_pool_caching(const mi_render_pool *pool);

/* Whether a slot is free right now, without taking one. */
int mi_render_pool_idle(const mi_render_pool *pool);

/*
 * Change the proxy the browser uses, at runtime.
 *
 * The browser is forked per render, so the next render uses the new value; a
 * render already running is not disturbed. NULL or "" is a direct connection.
 * Safe to call while renders run.
 */
void mi_render_pool_set_proxy(mi_render_pool *pool, const char *proxy);

/*
 * Render `url` into `out`. Takes a slot for the duration; returns MI_RENDER_BUSY
 * immediately when none is free rather than queueing, because a queue would
 * grow without bound behind a slow page.
 */
mi_render_status mi_render_run(mi_render_pool *pool, const char *url,
                               mi_render_mode mode, mi_render_result *out);

/* The same, but never blocks and never takes a slot — for a health check. */
int mi_render_url_allowed(const char *url);

#endif /* MI_RENDER_H */
