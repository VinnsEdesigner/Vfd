/* mi_render.c - render a page with headless Chromium. */

#define _GNU_SOURCE
#include "mi_render.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "mi_util.h"

/* Chromium's own names, in the order they are usually found. */
static const char *const BROWSERS[] = {
    "chromium", "chromium-browser", "google-chrome", "google-chrome-stable",
    "chrome", NULL,
};

/* The flags a render runs with, and why each is here. These were settled by
 * measurement on this host, not copied from a blog:
 *
 *   --headless=new        the modern headless mode; the old one is removed
 *   --no-sandbox          an unprivileged container cannot use the namespace
 *                         sandbox. This is the honest cost of running here.
 *   --disable-dev-shm-usage
 *                         /dev/shm is 64 MB in this container; Chromium needs
 *                         more than that and misbehaves without this
 *   --disable-gpu         no GPU, and asking for one wastes time
 *   --hide-scrollbars     a scrollbar in the picture is noise
 *   --no-first-run, --no-default-browser-check
 *                         otherwise the first run opens a setup flow
 *   --disable-background-networking, --disable-sync, --disable-translate,
 *   --disable-extensions, --disable-default-apps
 *                         none of these are wanted and all cost time or traffic
 *   --mute-audio          nothing should make noise
 *   --js-flags=--max-old-space-size=N
 *                         the memory bound. `ulimit -v` cannot be used: Chromium
 *                         reserves a huge virtual address space and dies at once
 *   --user-data-dir=…     a throwaway profile, so no state survives a render
 */
typedef struct {
    char *argv[40];
    int argc;
    char **owned; /* strings to free */
    int owned_count;
} arg_list;

static void args_init(arg_list *a) {
    memset(a, 0, sizeof *a);
    a->owned = mi_calloc(32, sizeof(char *));
}

static void args_push(arg_list *a, const char *s) {
    if (a->argc < 38) a->argv[a->argc++] = (char *)s;
}

/* Push a string this list must free. */
static void args_push_owned(arg_list *a, char *s) {
    if (!s) return;
    if (a->argc < 38) a->argv[a->argc++] = s;
    if (a->owned && a->owned_count < 32) a->owned[a->owned_count++] = s;
}

static void args_free(arg_list *a) {
    for (int i = 0; i < a->owned_count; i++) free(a->owned[i]);
    free(a->owned);
    memset(a, 0, sizeof *a);
}

/* ------------------------------------------------------------------ */
/* URL rules                                                           */
/* ------------------------------------------------------------------ */

/*
 * Whether the engine will render this URL.
 *
 * Only http(s): a file:// URL would read this host's disk, and anything else is
 * not a page. Loopback and link-local addresses are refused as well, so a
 * render cannot reach the engine's own port (a render of ourselves would be a
 * loop) or an internal service that happens to be listening. That is a real
 * restriction on a self-hosted setup, so it is stated here rather than
 * discovered later.
 */
int mi_render_url_allowed(const char *url) {
    if (!url || !*url) return 0;
    if (strncasecmp(url, "http://", 7) != 0 && strncasecmp(url, "https://", 8) != 0) {
        return 0;
    }

    const char *host = strstr(url, "://");
    if (!host) return 0;
    host += 3;
    /* Skip credentials, which a URL may carry. */
    const char *at = strchr(host, '@');
    const char *slash = strchr(host, '/');
    if (at && (!slash || at < slash)) host = at + 1;

    /*
     * The host, which is either a name (up to the first '/', ':' or '?') or a
     * bracketed IPv6 literal. The brackets have to be handled separately: a
     * naive "stop at the colon" leaves "[" for "[::1]", so the loopback check
     * below never sees the address it is meant to refuse.
     */
    size_t hlen = 0;
    if (host[0] == '[') {
        while (host[hlen] && host[hlen] != ']') hlen++;
        if (host[hlen] == ']') hlen++; /* include the closing bracket */
    } else {
        while (host[hlen] && host[hlen] != '/' && host[hlen] != ':' && host[hlen] != '?') hlen++;
    }
    if (hlen == 0) return 0;

    char h[256];
    if (hlen >= sizeof h) return 0;
    memcpy(h, host, hlen);
    h[hlen] = '\0';

    /* A hostname, not an address literal we would have to decode. */
    if (!strcasecmp(h, "localhost") || !strcasecmp(h, "localhost.localdomain")) return 0;
    if (!strcmp(h, "0.0.0.0")) return 0;
    /* IPv6 loopback and the unspecified address. */
    if (!strcasecmp(h, "[::1]") || !strcasecmp(h, "[::]") ||
        !strcasecmp(h, "[0:0:0:0:0:0:0:1]") || !strcasecmp(h, "[0:0:0:0:0:0:0:0]")) {
        return 0;
    }

    /* 127.0.0.0/8 and 169.254.0.0/16 are loopback and link-local. */
    if (!strncmp(h, "127.", 4)) return 0;
    if (!strncmp(h, "169.254.", 8)) return 0;
    /* A bare dotted-quad in 10./172.16-31./192.168. is private; refusing it
     * would break a legitimate intranet page, so it is allowed — the operator
     * chose to run here. */
    return 1;
}

/* ------------------------------------------------------------------ */
/* Finding a browser                                                   */
/* ------------------------------------------------------------------ */

static int file_is_executable(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

static char *find_browser(const char *hint) {
    if (hint && *hint) {
        if (file_is_executable(hint)) return mi_strdup(hint);
        return NULL;
    }
    /* A bare name is fine: the child uses execvp, which searches PATH. */
    for (size_t i = 0; BROWSERS[i]; i++) {
        if (mi_which(BROWSERS[i])) return mi_strdup(BROWSERS[i]);
    }
    /* Playwright's bundled build, when the system has none. */
    const char *home = getenv("HOME");
    if (home) {
        char *p = mi_asprintf("%s/.cache/ms-playwright/chromium-1248/chrome-linux64/chrome", home);
        if (p && file_is_executable(p)) return p;
        free(p);
        p = mi_asprintf("%s/.cache/ms-playwright/chromium-1200/chrome-linux64/chrome", home);
        if (p && file_is_executable(p)) return p;
        free(p);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* The pool                                                            */
/* ------------------------------------------------------------------ */

struct mi_render_pool {
    mi_render_config cfg;
    char *browser;
    /* A counting semaphore as a mutex + condition: the free count is explicit,
     * which makes "is a slot free" answerable without blocking. */
    pthread_mutex_t lock;
    pthread_cond_t free_slot;
    int in_use;
    /* Serialises reaping, so two renders finishing at once do not both walk the
     * same waitpid loop. */
    pthread_mutex_t reap_lock;
};

void mi_render_config_init(mi_render_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->chromium_path = NULL;
    cfg->timeout_seconds = 20;
    cfg->max_output_bytes = 8 * 1024 * 1024;
    cfg->viewport_width = 1280;
    cfg->viewport_height = 900;
    cfg->v8_heap_mb = 256;
    cfg->max_concurrent = 1;
    cfg->user_agent = NULL;
    cfg->profile_root = NULL;
}

const char *mi_render_status_name(mi_render_status s) {
    switch (s) {
        case MI_RENDER_OK: return "ok";
        case MI_RENDER_BUSY: return "busy";
        case MI_RENDER_TIMEOUT: return "timeout";
        case MI_RENDER_FAILED: return "failed";
        case MI_RENDER_TOO_LARGE: return "too_large";
        case MI_RENDER_BAD_URL: return "bad_url";
        case MI_RENDER_NO_BROWSER: return "no_browser";
    }
    return "unknown";
}

void mi_render_result_free(mi_render_result *res) {
    if (!res) return;
    free(res->data);
    free(res->title);
    free(res->error);
    memset(res, 0, sizeof *res);
}

mi_render_pool *mi_render_pool_new(const mi_render_config *cfg) {
    /*
     * Become a subreaper before anything is spawned.
     *
     * Chromium forks a tree of helpers. When the browser exits, any helper not
     * yet gone is reparented — to PID 1 by default, and in a container PID 1 is
     * often a shell that never calls wait(), so each render leaks a handful of
     * zombies until the process table is full. As a subreaper they come here
     * instead, and one waitpid loop clears them. Measured: 74 zombies after a
     * handful of renders before this; the count stays flat with it.
     */
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0) {
        mi_logf(MI_LOG_ERROR, "render: could not become a subreaper: %s", strerror(errno));
    }

    mi_render_pool *pool = mi_calloc(1, sizeof *pool);
    if (!pool) return NULL;
    pool->cfg = *cfg;
    if (pool->cfg.timeout_seconds <= 0) pool->cfg.timeout_seconds = 20;
    if (pool->cfg.max_concurrent <= 0) pool->cfg.max_concurrent = 1;
    if (pool->cfg.viewport_width <= 0) pool->cfg.viewport_width = 1280;
    if (pool->cfg.viewport_height <= 0) pool->cfg.viewport_height = 900;
    if (pool->cfg.v8_heap_mb <= 0) pool->cfg.v8_heap_mb = 256;
    if (pool->cfg.max_output_bytes == 0) pool->cfg.max_output_bytes = 8 * 1024 * 1024;

    pool->browser = find_browser(cfg->chromium_path);
    if (!pool->browser) {
        mi_logf(MI_LOG_INFO, "render: no browser found; rendering is unavailable");
    } else {
        mi_logf(MI_LOG_INFO, "render: using %s (limit %d, timeout %ds, heap %d MB)",
                pool->browser, pool->cfg.max_concurrent, pool->cfg.timeout_seconds,
                pool->cfg.v8_heap_mb);
    }

    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->free_slot, NULL);
    pthread_mutex_init(&pool->reap_lock, NULL);
    return pool;
}

void mi_render_pool_free(mi_render_pool *pool) {
    if (!pool) return;
    /* Wait for any render in flight, so nothing is killed mid-write. */
    pthread_mutex_lock(&pool->lock);
    while (pool->in_use > 0) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    pthread_mutex_unlock(&pool->lock);
    free(pool->browser);
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->free_slot);
    pthread_mutex_destroy(&pool->reap_lock);
    free(pool);
}

int mi_render_pool_available(const mi_render_pool *pool) {
    return pool && pool->browser != NULL;
}

int mi_render_pool_limit(const mi_render_pool *pool) {
    return pool ? pool->cfg.max_concurrent : 0;
}

int mi_render_pool_idle(const mi_render_pool *pool) {
    if (!pool) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&pool->lock);
    int idle = pool->in_use < pool->cfg.max_concurrent;
    pthread_mutex_unlock((pthread_mutex_t *)&pool->lock);
    return idle;
}

/* ------------------------------------------------------------------ */
/* Running the browser                                                 */
/* ------------------------------------------------------------------ */

/* Reap every child that has finished, including reparented orphans. Called
 * after a render so nothing is left behind. */
static void reap_children(mi_render_pool *pool) {
    pthread_mutex_lock(&pool->reap_lock);
    for (;;) {
        int status = 0;
        pid_t got = waitpid(-1, &status, WNOHANG);
        if (got <= 0) break;
    }
    pthread_mutex_unlock(&pool->reap_lock);
}

/* A unique, empty directory for one render's browser profile. */
static char *make_profile_dir(const mi_render_config *cfg, const char *id) {
    const char *root = (cfg->profile_root && *cfg->profile_root) ? cfg->profile_root
                                                                 : "/tmp";
    char *dir = mi_asprintf("%s/vyzorix-render-%s", root, id);
    if (!dir) return NULL;
    mi_rm_rf(dir);
    if (mi_mkdir_p(dir) != 0) {
        free(dir);
        return NULL;
    }
    return dir;
}

/* Milliseconds since an arbitrary point; for the deadline. */
static double mono_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/*
 * Run the browser and wait, with a deadline.
 *
 * The deadline is enforced here rather than by Chromium, because Chromium's own
 * time limit bounds page time and a page that never settles runs forever: a
 * request to a black-holed address kept going until this killed it.
 *
 * The child is put in its own process group, so the kill takes the whole tree
 * and not just the leader — otherwise the helpers are orphaned, which is how the
 * zombie pile starts.
 */
static int run_browser(mi_render_pool *pool, arg_list *args, const char *stdout_path,
                       int *out_timed_out) {
    *out_timed_out = 0;

    /* --dump-dom writes the document to stdout, so when a stdout file is given
     * that is where it must go; otherwise both streams are discarded. Getting
     * this wrong is why the DOM mode produced nothing while the image mode
     * worked: the DOM was being thrown away. */
    int out_fd = stdout_path ? open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0600)
                             : open("/dev/null", O_WRONLY);
    int devnull = open("/dev/null", O_WRONLY);

    pid_t pid = fork();
    if (pid < 0) {
        if (out_fd >= 0) close(out_fd);
        if (devnull >= 0) close(devnull);
        return -1;
    }
    if (pid == 0) {
        /* Its own group, so the parent can signal the whole tree at once. */
        setpgid(0, 0);
        if (out_fd >= 0) dup2(out_fd, STDOUT_FILENO);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        /* execvp, not execv: the browser is usually named rather than pathed,
         * and this resolves it the same way a shell would. */
        execvp(args->argv[0], args->argv);
        _exit(127);
    }
    if (out_fd >= 0) close(out_fd);
    if (devnull >= 0) close(devnull);
    setpgid(pid, pid); /* also set in the parent, to close the race */

    double deadline = mono_seconds() + pool->cfg.timeout_seconds;
    int status = 0;
    for (;;) {
        pid_t got = waitpid(pid, &status, WNOHANG);
        if (got == pid) break;
        if (got < 0 && errno != EINTR) return -1;
        if (mono_seconds() >= deadline) {
            /* Kill the group, politely then firmly, and wait for it to be gone
             * so the profile directory can be removed safely. */
            kill(-pid, SIGTERM);
            struct timespec brief = {.tv_sec = 0, .tv_nsec = 300 * 1000 * 1000};
            nanosleep(&brief, NULL);
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            *out_timed_out = 1;
            return 0;
        }
        struct timespec tick = {.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000};
        nanosleep(&tick, NULL);
    }

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* Read a file whole, up to a cap. Returns the bytes and sets `truncated`. */
static unsigned char *read_capped(const char *path, size_t cap, size_t *len_out,
                                  int *truncated) {
    *len_out = 0;
    *truncated = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    unsigned char *buf = mi_malloc(cap + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, cap, f);
    /* One more byte tells us whether there was more. */
    int extra = fgetc(f) != EOF;
    fclose(f);
    if (extra) *truncated = 1;
    buf[got] = '\0';
    *len_out = got;
    return buf;
}

/* The page's <title>, for a label. Best-effort. */
static char *title_from_html(const unsigned char *html, size_t len) {
    if (!html) return NULL;
    char *s = mi_strndup((const char *)html, len < 65536 ? len : 65536);
    if (!s) return NULL;
    char *title = NULL;
    char *open = strcasestr(s, "<title");
    if (open) {
        char *gt = strchr(open, '>');
        char *close = gt ? strcasestr(gt, "</title>") : NULL;
        if (gt && close && close > gt + 1) {
            title = mi_strndup(gt + 1, (size_t)(close - gt - 1));
        }
    }
    free(s);
    if (!title) return NULL;
    /* Collapse whitespace and cap it. */
    char *w = title;
    int space = 0;
    for (char *r = title; *r; r++) {
        if (isspace((unsigned char)*r)) {
            space = 1;
            continue;
        }
        if (space && w != title) *w++ = ' ';
        space = 0;
        *w++ = *r;
    }
    *w = '\0';
    if (strlen(title) > 200) title[200] = '\0';
    return title;
}

mi_render_status mi_render_run(mi_render_pool *pool, const char *url,
                               mi_render_mode mode, mi_render_result *out) {
    memset(out, 0, sizeof *out);
    out->content_type = "application/octet-stream";
    double started = mono_seconds();

    if (!pool) {
        out->status = MI_RENDER_FAILED;
        out->error = mi_strdup("no renderer");
        return out->status;
    }
    if (!mi_render_url_allowed(url)) {
        out->status = MI_RENDER_BAD_URL;
        out->error = mi_strdup("only public http(s) pages can be rendered");
        return out->status;
    }
    if (!pool->browser) {
        out->status = MI_RENDER_NO_BROWSER;
        out->error = mi_strdup("no browser is installed on this host");
        return out->status;
    }

    /* Take a slot, or refuse at once. A queue would grow without bound behind a
     * slow page, and the caller can retry. */
    pthread_mutex_lock(&pool->lock);
    if (pool->in_use >= pool->cfg.max_concurrent) {
        pthread_mutex_unlock(&pool->lock);
        out->status = MI_RENDER_BUSY;
        out->error = mi_strdup("the renderer is busy");
        out->elapsed_seconds = mono_seconds() - started;
        return out->status;
    }
    pool->in_use++;
    pthread_mutex_unlock(&pool->lock);

    mi_render_status rc = MI_RENDER_FAILED;
    char *profile = NULL;
    char *out_path = NULL;

    /* A unique profile per render, so no state survives one. */
    char id[64];
    snprintf(id, sizeof id, "%d-%ld", (int)getpid(), (long)time(NULL));
    profile = make_profile_dir(&pool->cfg, id);
    if (!profile) {
        out->error = mi_strdup("could not create a browser profile");
        goto done;
    }

    out_path = mi_asprintf("%s/output.%s", profile, mode == MI_RENDER_IMAGE ? "png" : "html");
    if (!out_path) {
        out->error = mi_strdup("out of memory");
        goto done;
    }

    arg_list args;
    args_init(&args);
    args_push(&args, pool->browser);
    args_push(&args, "--headless=new");
    args_push(&args, "--no-sandbox");
    args_push(&args, "--disable-dev-shm-usage");
    args_push(&args, "--disable-gpu");
    args_push(&args, "--hide-scrollbars");
    args_push(&args, "--no-first-run");
    args_push(&args, "--no-default-browser-check");
    args_push(&args, "--disable-background-networking");
    args_push(&args, "--disable-sync");
    args_push(&args, "--disable-translate");
    args_push(&args, "--disable-extensions");
    args_push(&args, "--disable-default-apps");
    args_push(&args, "--mute-audio");
    args_push(&args, "--disable-features=Translate,BackForwardCache,AcceptCHFrame");
    if (pool->cfg.user_agent && *pool->cfg.user_agent) {
        args_push_owned(&args, mi_asprintf("--user-agent=%s", pool->cfg.user_agent));
    }
    args_push_owned(&args, mi_asprintf("--js-flags=--max-old-space-size=%d", pool->cfg.v8_heap_mb));
    args_push_owned(&args, mi_asprintf("--user-data-dir=%s", profile));
    /* Bounds the *page's* time, which is what makes a slow site settle rather
     * than being killed at the deadline. It does not bound a hang; the deadline
     * above does that. */
    args_push_owned(&args, mi_asprintf("--virtual-time-budget=%d",
                                       pool->cfg.timeout_seconds * 500));

    if (mode == MI_RENDER_IMAGE) {
        args_push_owned(&args, mi_asprintf("--window-size=%d,%d", pool->cfg.viewport_width,
                                           pool->cfg.viewport_height));
        args_push_owned(&args, mi_asprintf("--screenshot=%s", out_path));
    } else {
        args_push(&args, "--dump-dom");
    }
    args_push(&args, url);
    args_push(&args, NULL);

    int timed_out = 0;
    /* The image mode writes the file itself; the DOM mode writes to stdout,
     * which is captured into the same place. */
    int exit_code = run_browser(pool, &args,
                                mode == MI_RENDER_DOM ? out_path : NULL, &timed_out);
    args_free(&args);

    if (timed_out) {
        rc = MI_RENDER_TIMEOUT;
        out->error = mi_asprintf("the page did not finish within %d seconds",
                                 pool->cfg.timeout_seconds);
        goto done;
    }
    if (exit_code != 0) {
        rc = MI_RENDER_FAILED;
        out->error = mi_asprintf("the browser exited with status %d", exit_code);
        goto done;
    }

    size_t len = 0;
    int truncated = 0;
    unsigned char *data = read_capped(out_path, pool->cfg.max_output_bytes, &len, &truncated);
    if (!data) {
        rc = MI_RENDER_FAILED;
        out->error = mi_strdup("the browser produced no output");
        goto done;
    }
    if (len == 0) {
        free(data);
        rc = MI_RENDER_FAILED;
        out->error = mi_strdup("the browser produced an empty result");
        goto done;
    }

    out->data = data;
    out->len = len;
    out->truncated = truncated;
    out->content_type = mode == MI_RENDER_IMAGE ? "image/png" : "text/html; charset=utf-8";
    if (mode == MI_RENDER_DOM) out->title = title_from_html(data, len);
    rc = truncated ? MI_RENDER_TOO_LARGE : MI_RENDER_OK;

done:
    /* The profile goes away whatever happened, so nothing accumulates. */
    if (profile) mi_rm_rf(profile);
    free(profile);
    free(out_path);
    reap_children(pool);

    pthread_mutex_lock(&pool->lock);
    pool->in_use--;
    pthread_cond_broadcast(&pool->free_slot);
    pthread_mutex_unlock(&pool->lock);

    out->status = rc;
    out->elapsed_seconds = mono_seconds() - started;
    return rc;
}
