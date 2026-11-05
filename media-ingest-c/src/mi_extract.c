/* mi_extract.c - the URLs a streaming site will not put in its HTML.
 *
 * The extractor is run as a subprocess and asked for JSON. Its output is parsed
 * here into the small shape the rest of Vyzor needs: a title, and a list of
 * formats each carrying a URL and what it contains.
 *
 * WHY A SUBPROCESS AND WHY JSON
 *
 * The alternative — reimplementing the signature and `n` transforms — is a
 * constant-maintenance job against a moving target, and it is the reason yt-dlp
 * exists. Running the maintained implementation is the honest choice.
 *
 * The output is read from a pipe, not a file, and the arguments are an argv
 * array, never a shell string: the URL comes from a caller, and a URL pasted
 * into a command line is a command injection.
 */
#define _GNU_SOURCE
#include "mi_extract.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "mi_json.h"
#include "mi_util.h"

/* A resolve prints one JSON object; this bounds it so a pathological page cannot
 * exhaust memory. A large playlist is the case that would: hundreds of formats
 * each with a long URL. */
#define EXTRACT_MAX_OUTPUT (8 * 1024 * 1024)

void mi_extract_config_init(mi_extract_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->binary = NULL;
    cfg->timeout_seconds = 60.0;
    cfg->cookies_file = NULL;
    cfg->enumerate = 0;
    cfg->search_term = NULL;
    cfg->search_limit = 0;
}

void mi_extract_result_free(mi_extract_result *res) {
    if (!res) return;
    free(res->title);
    free(res->extractor);
    free(res->thumbnail);
    for (size_t i = 0; i < res->format_count; i++) {
        free(res->formats[i].url);
        free(res->formats[i].format_id);
        free(res->formats[i].ext);
        free(res->formats[i].resolution);
        free(res->formats[i].vcodec);
        free(res->formats[i].acodec);
        free(res->formats[i].mime);
    }
    free(res->formats);
    for (size_t i = 0; i < res->entry_count; i++) {
        free(res->entries[i].id);
        free(res->entries[i].title);
        free(res->entries[i].url);
        free(res->entries[i].uploader);
        free(res->entries[i].thumbnail);
    }
    free(res->entries);
    memset(res, 0, sizeof *res);
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static const char *binary_for(const mi_extract_config *cfg) {
    return (cfg && cfg->binary && *cfg->binary) ? cfg->binary : "yt-dlp";
}

/*
 * The URL or query to hand the extractor.
 *
 * A search is expressed as a pseudo-URL: `ytsearch5:term`. Building it here
 * keeps the rest of the argv the same for both cases — the difference between
 * "resolve this address" and "find something matching this" is the argument,
 * not the invocation.
 *
 * Returns an owned string, or NULL on a bad argument.
 */
static char *query_for(const mi_extract_config *cfg, const char *url) {
    if (cfg && cfg->search_term && *cfg->search_term) {
        int limit = cfg->search_limit > 0 ? cfg->search_limit : 10;
        return mi_asprintf("ytsearch%d:%s", limit, cfg->search_term);
    }
    return url ? mi_strdup(url) : NULL;
}

/*
 * A JavaScript runtime to hand yt-dlp, or NULL.
 *
 * Probed with `which` rather than assumed, because naming a runtime the host
 * does not have makes yt-dlp fail where naming none would have worked. The
 * order is preference: deno is yt-dlp's own default and the best supported,
 * node is the one a host is most likely to already have (this one does), and
 * bun and quickjs are the remaining supported options.
 *
 * The answer is cached: which runtimes exist does not change while Vyzor runs,
 * and this is called for every resolve.
 */
static const char *find_js_runtime(void) {
    static const char *cached = NULL;
    static int probed = 0;
    if (probed) return cached;
    probed = 1;
    static const char *CANDIDATES[] = {"deno", "node", "bun", "quickjs", NULL};
    for (size_t i = 0; CANDIDATES[i]; i++) {
        if (mi_which(CANDIDATES[i])) {
            cached = CANDIDATES[i];
            break;
        }
    }
    return cached;
}

/*
 * Run the extractor on `url` and return its stdout, or NULL.
 *
 * `probe_only` adds the flags that make it resolve without downloading, and is
 * what `mi_extract_available` uses so the check cannot fetch anything.
 */
static char *run_extractor(const mi_extract_config *cfg, const char *url, int probe_only,
                           int *exit_code) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return NULL;

    const char *binary = binary_for(cfg);
    const char *argv[24];
    int a = 0;
    argv[a++] = binary;
    if (probe_only) {
        argv[a++] = "--version";
    } else {
        /* Resolve only: never fetch the media. The URLs are handed to Vyzor's own
         * download path, so the extractor has no business downloading. */
        argv[a++] = "-J"; /* dump the info as JSON */
        argv[a++] = "--no-warnings";
        argv[a++] = "--skip-download";
        /*
         * A JS runtime, when there is one.
         *
         * yt-dlp 2026 moved the signature and challenge solving into a separate
         * JavaScript runtime and warns, without one, that "YouTube extraction
         * has been deprecated and some formats may be missing" — which is
         * literally formats the caller never sees. Only deno is enabled by
         * default, so a host with node but no deno runs with *no* runtime at all
         * unless it is named here. Measured: the debug line reads
         * "JS Challenge Providers: bun (unavailable), deno (unavailable),
         * node (unavailable), quickjs (unavailable)" on a host that has node.
         *
         * The runtime is passed when one exists rather than assumed: naming a
         * runtime the host does not have is worse than naming none.
         */
        const char *runtime = find_js_runtime();
        if (runtime) {
            argv[a++] = "--js-runtimes";
            argv[a++] = runtime;
        }
        /*
         * One item, unless the caller wants a list.
         *
         * A channel or a playlist URL describes many items; `--flat-playlist`
         * enumerates them without resolving each one, which is what a discovery
         * pass needs and is far cheaper. Without it, a playlist URL resolves to
         * its *first* item only, which is not what "list this channel" means.
         */
        /* A search or a list URL is always enumerated: there is no single item
         * to resolve, and the caller picks from what comes back. */
        int list = (cfg && (cfg->enumerate || (cfg->search_term && *cfg->search_term)));
        if (list) argv[a++] = "--flat-playlist";
        else argv[a++] = "--no-playlist";
        if (cfg && cfg->cookies_file && *cfg->cookies_file) {
            argv[a++] = "--cookies";
            argv[a++] = cfg->cookies_file;
        }
        argv[a++] = "--";
        argv[a++] = url;
    }
    argv[a] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }
    if (pid == 0) {
        /* The child writes the JSON and nothing else, so its stdout is the pipe
         * and its stderr is dropped — a warning is not a result. */
        dup2(pipefd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    /* The parent must close its copy of the write end, or the read below never
     * sees EOF: the child's exit leaves the pipe open through this fd and the
     * loop waits for a writer that will never write again. */
    close(pipefd[1]);

    /*
     * Read with a deadline.
     *
     * The extractor is a network client against a site that may not answer, so
     * a read that simply blocked could hold a Vyzor worker forever. The deadline
     * is what makes "the site hung" a reported failure rather than a stall.
     */
    char *buf = NULL;
    size_t len = 0, cap = 0;
    double deadline = now_seconds() + (cfg && cfg->timeout_seconds > 0
                                           ? cfg->timeout_seconds
                                           : 60.0);
    int timed_out = 0;
    for (;;) {
        double left = deadline - now_seconds();
        if (left <= 0) {
            timed_out = 1;
            break;
        }
        struct pollfd p = {.fd = pipefd[0], .events = POLLIN};
        int pr = poll(&p, 1, (int)(left * 1000));
        if (pr == 0) {
            timed_out = 1;
            break;
        }
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        char chunk[8192];
        ssize_t n = read(pipefd[0], chunk, sizeof chunk);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        if (n == 0) break; /* the child closed its end */
        if (len + (size_t)n > EXTRACT_MAX_OUTPUT) break;
        if (len + (size_t)n + 1 > cap) {
            size_t want = (len + (size_t)n + 1) * 2;
            char *grown = mi_realloc(buf, want);
            if (!grown) break;
            buf = grown;
            cap = want;
        }
        memcpy(buf + len, chunk, (size_t)n);
        len += (size_t)n;
    }
    close(pipefd[0]);
    if (buf) buf[len] = '\0';

    if (timed_out) {
        /* A resolver that will not finish is killed rather than left running: it
         * is a Python process, and a leaked one is a leak. */
        kill(pid, SIGKILL);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (exit_code) *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (timed_out) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* A cached answer for `mi_extract_available`: it cannot change at runtime. */
static int g_available = -1;

int mi_extract_available(const mi_extract_config *cfg) {
    if (g_available >= 0) return g_available;
    int code = -1;
    char *out = run_extractor(cfg, NULL, 1, &code);
    g_available = (out != NULL && code == 0) ? 1 : 0;
    free(out);
    return g_available;
}

/* A string field, or NULL. */
static char *dup_string(const mi_json *o, const char *key) {
    const char *v = mi_json_get_string(o, key, NULL);
    return (v && *v) ? mi_strdup(v) : NULL;
}

int mi_extract(const mi_extract_config *cfg, const char *url, mi_extract_result *out) {
    if (!cfg || !out) return MI_ERR_ARG;
    memset(out, 0, sizeof *out);
    out->duration_seconds = -1;

    /*
     * The argument is a URL or a search query.
     *
     * The two are different questions — "resolve this address" and "find
     * something matching this" — and yt-dlp expresses both as one argument
     * (`ytsearch5:term` for the second), so the choice is made here and the rest
     * of the invocation is identical.
     */
    char *query = query_for(cfg, url);
    if (!query) return MI_ERR_ARG;

    int code = -1;
    char *json = run_extractor(cfg, query, 0, &code);
    free(query);
    if (!json) {
        mi_logf(MI_LOG_ERROR, "extract: the extractor produced no output (exit %d)", code);
        return MI_ERR_NET;
    }
    /*
     * A non-zero exit is a failure, and saying so matters.
     *
     * The extractor prints an error to stderr and exits 1 when it cannot read a
     * page — a site that needs cookies, a missing impersonation dependency, a
     * video that was taken down. Its stdout is then empty or partial, and
     * treating that as "resolved, no formats" made every such page look like a
     * page with no media: the endpoint answered `ok: true` with `title: null`
     * and zero formats, which reads as success. Measured on three sites at once.
     */
    if (code != 0) {
        mi_logf(MI_LOG_ERROR, "extract: the extractor exited %d (%zu bytes of output)",
                code, strlen(json));
        free(json);
        return MI_ERR_NET;
    }

    mi_json *doc = mi_json_parse(json);
    free(json);
    if (!doc) return MI_ERR_ARG;

    out->title = dup_string(doc, "title");
    out->extractor = dup_string(doc, "extractor");
    out->thumbnail = dup_string(doc, "thumbnail");
    double dur = mi_json_get_number(doc, "duration", -1);
    out->duration_seconds = dur;

    const mi_json *formats = mi_json_get(doc, "formats");
    if (formats && formats->type == MI_JSON_ARRAY && formats->count > 0) {
        out->formats = mi_calloc(formats->count, sizeof *out->formats);
        if (out->formats) {
            for (size_t i = 0; i < formats->count; i++) {
                const mi_json *f = formats->items[i];
                if (!f || f->type != MI_JSON_OBJECT) continue;
                const char *u = mi_json_get_string(f, "url", NULL);
                /* A format with no URL is one the extractor could not resolve
                 * (a storyboard, a live fragment); it is not something to offer. */
                if (!u || !*u) continue;

                mi_extract_format *dst = &out->formats[out->format_count];
                memset(dst, 0, sizeof *dst);
                dst->url = mi_strdup(u);
                dst->format_id = dup_string(f, "format_id");
                dst->ext = dup_string(f, "ext");
                dst->resolution = dup_string(f, "resolution");
                dst->vcodec = dup_string(f, "vcodec");
                dst->acodec = dup_string(f, "acodec");
                dst->mime = dup_string(f, "mime_type");
                dst->filesize = (int64_t)mi_json_get_number(f, "filesize", -1);
                if (dst->filesize < 0) {
                    dst->filesize = (int64_t)mi_json_get_number(f, "filesize_approx", -1);
                }
                dst->bitrate = mi_json_get_number(f, "tbr", -1);
                dst->height = (int)mi_json_get_number(f, "height", 0);
                /*
                 * `vcodec` and `acodec` are three-valued, and the difference
                 * between two of them is the whole bug this fixes:
                 *
                 *   "avc1.640028"  a codec name — this stream carries it
                 *   "none"         explicitly absent — this stream does not
                 *   absent/null    *unknown* — the extractor could not tell
                 *
                 * Reading "unknown" as "absent" dropped every format on
                 * archive.org, whose entries carry a URL and a resolution but no
                 * codec names at all: three formats reported, none offered. When
                 * the codec is unknown, the other fields are what say whether
                 * there is a picture and a sound.
                 */
                const char *vc = mi_json_get_string(f, "vcodec", NULL);
                const char *ac = mi_json_get_string(f, "acodec", NULL);
                const char *res = mi_json_get_string(f, "resolution", NULL);
                const char *aext = mi_json_get_string(f, "audio_ext", NULL);
                int v_known = vc != NULL;
                int a_known = ac != NULL;
                /* Absent means unknown, so the other fields decide. */
                dst->has_video = v_known ? (vc[0] && strcmp(vc, "none") != 0)
                                         : (dst->height > 0 || (res && *res));
                dst->has_audio = a_known ? (ac[0] && strcmp(ac, "none") != 0)
                                         : (mi_json_get_number(f, "abr", -1) > 0 ||
                                            (aext && *aext && strcmp(aext, "none") != 0));
                out->format_count++;
            }
        }
    }

    /*
     * The entries of a playlist, channel or search.
     *
     * `--flat-playlist` returns these instead of formats, and they are what a
     * discovery pass wants: a title, an id and a page URL per item, with no
     * stream resolution done. A chosen one is resolved in a second call.
     */
    const mi_json *entries = mi_json_get(doc, "entries");
    if (entries && entries->type == MI_JSON_ARRAY && entries->count > 0) {
        out->entries = mi_calloc(entries->count, sizeof *out->entries);
        if (out->entries) {
            for (size_t i = 0; i < entries->count; i++) {
                const mi_json *e = entries->items[i];
                if (!e || e->type != MI_JSON_OBJECT) continue;
                mi_extract_entry *dst = &out->entries[out->entry_count];
                memset(dst, 0, sizeof *dst);
                dst->duration_seconds = -1;
                dst->id = dup_string(e, "id");
                dst->title = dup_string(e, "title");
                dst->uploader = dup_string(e, "uploader");
                dst->thumbnail = dup_string(e, "thumbnail");
                dst->duration_seconds = mi_json_get_number(e, "duration", -1);
                const char *eu = mi_json_get_string(e, "url", NULL);
                if (eu && *eu) dst->url = mi_strdup(eu);
                const char *etype = mi_json_get_string(e, "_type", NULL);
                dst->is_playlist = etype && strcmp(etype, "url") != 0;
                /* An entry with no title and no id is not something to offer. */
                if (!dst->title && !dst->id) {
                    free(dst->id);
                    free(dst->url);
                    free(dst->uploader);
                    free(dst->thumbnail);
                    continue;
                }
                out->entry_count++;
            }
        }
    }

    mi_json_value_free(doc);
    return MI_OK;
}
