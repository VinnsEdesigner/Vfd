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
#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "mi_json.h"
#include "mi_util.h"

/* A resolve prints one JSON object; this bounds it so a pathological page cannot
 * exhaust memory. A large playlist is the case that would: hundreds of formats
 * each with a long URL. */
#define EXTRACT_MAX_OUTPUT (8 * 1024 * 1024)

/* The fixed arguments before any per-site ones. Used to size the argv array, so
 * it must cover the longest branch below. */
#define EXTRACT_BASE_ARGS 32

void mi_extract_config_init(mi_extract_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->binary = NULL;
    cfg->gallery_binary = NULL;
    cfg->kind = MI_EXTRACT_AUTO;
    cfg->timeout_seconds = 60.0;
    cfg->cookies_file = NULL;
    cfg->cookies_text = NULL;
    cfg->proxy = NULL;
    cfg->impersonate = NULL;
    cfg->enumerate = 0;
    cfg->search_term = NULL;
    cfg->search_limit = 0;
    cfg->date_after = NULL;
    cfg->date_before = NULL;
    cfg->playlist_items = NULL;
    cfg->match_filter = NULL;
    cfg->extractor_args = NULL;
    cfg->extractor_arg_count = 0;
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
        free(res->formats[i].protocol);
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

static const char *gallery_binary_for(const mi_extract_config *cfg) {
    return (cfg && cfg->gallery_binary && *cfg->gallery_binary) ? cfg->gallery_binary
                                                                : "gallery-dl";
}

/*
 * Netscape cookie text as ffmpeg's `-cookies` string.
 *
 * The two tools want different things and neither will take the other's:
 *
 *   yt-dlp   a Netscape cookie FILE (what a browser export gives)
 *   ffmpeg   newline-delimited `Set-Cookie` field values
 *
 * A user exports once, in Netscape format, so that is the input and this converts
 * it for the tool that needs the other shape. A Netscape line is
 *
 *   domain  include-subdomains  path  secure  expiry  name  value
 *
 * tab-separated, with `#` comments and blank lines to skip. ffmpeg's form is
 * `name=value; domain=...; path=...`, one per line — the domain and path matter
 * or the cookie is not sent.
 *
 * Returns an owned string, or NULL when there is nothing usable.
 */
char *mi_extract_cookies_for_ffmpeg(const char *text) {
    if (!text || !*text) return NULL;
    mi_json_buf out;
    mi_json_init(&out);

    const char *p = text;
    int written = 0;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);

        /* Skip comments and blanks; a Netscape export is full of both. */
        if (n > 0 && p[0] != '#') {
            char line[2048];
            if (n < sizeof line) {
                memcpy(line, p, n);
                line[n] = '\0';
                char *f[7] = {0};
                int nf = 0;
                char *save = NULL;
                for (char *t = strtok_r(line, "\t", &save); t && nf < 7;
                     t = strtok_r(NULL, "\t", &save)) {
                    f[nf++] = t;
                }
                /* A cookie needs a name and a value; the rest is placement. */
                if (nf >= 7 && *f[5]) {
                    if (written) mi_json_raw(&out, "\n");
                    mi_json_raw(&out, f[5]); /* name */
                    mi_json_raw(&out, "=");
                    mi_json_raw(&out, f[6]); /* value */
                    mi_json_raw(&out, "; domain=");
                    /* A leading dot means "any subdomain" in Netscape; ffmpeg
                     * matches the domain itself, so the dot is dropped. */
                    mi_json_raw(&out, f[0][0] == '.' ? f[0] + 1 : f[0]);
                    mi_json_raw(&out, "; path=");
                    mi_json_raw(&out, *f[2] ? f[2] : "/");
                    written++;
                }
            }
        }
        if (!nl) break;
        p = nl + 1;
    }

    if (!written) {
        mi_json_free(&out);
        return NULL;
    }
    char *result = mi_strdup(mi_json_data(&out));
    mi_json_free(&out);
    return result;
}

/*
 * Write inline cookies to a temporary file for yt-dlp, which takes a path.
 *
 * Created 0600 and removed by the caller as soon as the resolve finishes: it
 * holds a credential, and a credential on disk is one more place it can outlive
 * its use. The path is returned in `out_path`, owned by the caller.
 *
 * Returns 0 on success.
 */
static int write_temp_cookies(const char *text, char **out_path) {
    *out_path = NULL;
    if (!text || !*text) return -1;
    char tmpl[] = "/tmp/vyzor-cookies-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    /* mkstemp already creates 0600, but say so: this file must never be
     * group- or world-readable. */
    fchmod(fd, 0600);
    size_t len = strlen(text);
    ssize_t wrote = write(fd, text, len);
    close(fd);
    if (wrote < 0 || (size_t)wrote != len) {
        unlink(tmpl);
        return -1;
    }
    *out_path = mi_strdup(tmpl);
    return *out_path ? 0 : -1;
}

/* The public spelling of the above: the download path needs it too, to give
 * yt-dlp the same cookies the resolve used. */
int mi_extract_write_cookies_file(const char *text, char **out_path) {
    return write_temp_cookies(text, out_path);
}

/*
 * Hosts gallery-dl handles that yt-dlp does not, as host *fragments*.
 *
 * A fragment so a subdomain or a country domain still matches ("flickr" catches
 * both flickr.com and live.staticflickr.com). Deliberately short: this decides
 * which tool is asked first, and every entry should be a site where yt-dlp
 * genuinely returns nothing. A site that is merely image-heavy but also has
 * video (Reddit, X) is left out, because yt-dlp handles those.
 */
const char *const MI_GALLERY_HOSTS[] = {
    "flickr", "imgur", "deviantart", "artstation", "pixiv", "danbooru",
    "gelbooru", "safebooru", "e-hentai", "tumblr", "pinterest", "500px",
    "unsplash", "pexels", "photobucket", "smugmug", "weasyl", "furaffinity",
    "newgrounds", "baraag", "mastodon", "bluesky", "vk.com", "weibo",
    "naver", "bcy.net", "lofter", "wikiart", "manga", "comic", NULL
};

/** Does any fragment appear in the host of `url`? */
static int host_matches(const char *url, const char *const *fragments) {
    /* The host, between the scheme and the first path separator. */
    const char *p = strstr(url, "://");
    if (!p) return 0;
    p += 3;
    const char *end = p;
    while (*end && *end != '/' && *end != '?' && *end != '#') end++;
    size_t n = (size_t)(end - p);
    if (n == 0 || n > 255) return 0;
    char host[256];
    memcpy(host, p, n);
    host[n] = '\0';
    for (size_t i = 0; fragments[i]; i++) {
        if (strcasestr(host, fragments[i])) return 1;
    }
    return 0;
}

mi_extract_kind mi_extract_pick(const mi_extract_config *cfg, const char *url) {
    if (cfg && cfg->kind != MI_EXTRACT_AUTO) return cfg->kind;
    /*
     * AUTO: gallery-dl for a host it is known for, yt-dlp otherwise.
     *
     * yt-dlp is the default because it is the one that handles video, which is
     * the commoner case; a host on the gallery list is the exception where
     * yt-dlp would return nothing.
     */
    if (url && host_matches(url, MI_GALLERY_HOSTS)) return MI_EXTRACT_GALLERY;
    return MI_EXTRACT_VIDEO;
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
 * The impersonation target, spelled the way the tools accept it.
 *
 * Both yt-dlp and curl_cffi reject a target that carries a browser's display
 * name or a separator: curl_cffi wants `chrome131`, and answers
 * `Impersonating Chrome131 is not supported` for `Chrome131` or `chrome-131`.
 * A human typing into a settings box writes exactly those, and the failure is
 * silent — the fetch falls back to plain libcurl — so the value is normalised
 * here rather than left to the caller: lowercased, and with `-`, `_` and spaces
 * removed.
 *
 * A bare family ("chrome") is returned unchanged; both tools expand it to their
 * newest supported version themselves, and that is the form the docs use.
 *
 * Returns an owned string, or NULL when there is nothing to normalise.
 */
char *mi_extract_normalize_impersonate(const char *target) {
    if (!target || !*target) return NULL;
    char *out = mi_strdup(target);
    if (!out) return NULL;
    size_t w = 0;
    for (size_t r = 0; out[r]; r++) {
        char c = out[r];
        if (c == '-' || c == '_' || c == ' ') continue;
        out[w++] = (char)tolower((unsigned char)c);
    }
    out[w] = '\0';
    return out;
}

/*
 * Run an extractor and return its stdout, or NULL.
 *
 * `probe_only` adds the flags that make it resolve without downloading, and is
 * what `mi_extract_available` uses so the check cannot fetch anything.
 *
 * The two tools take different flags for the same intent: yt-dlp dumps one JSON
 * object with `-J`, gallery-dl dumps a list of tuples with `-j`, and both need to
 * be told not to download.
 */
static char *run_extractor_kind(const mi_extract_config *cfg, mi_extract_kind kind,
                                const char *url, int probe_only, int *exit_code) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return NULL;

    const char *binary = (kind == MI_EXTRACT_GALLERY) ? gallery_binary_for(cfg)
                                                      : binary_for(cfg);
    /*
     * Sized for the fixed options plus every per-site argument.
     *
     * A fixed 24 was enough until extractor arguments were added, and those are
     * a caller-supplied list — so the bound is derived rather than guessed. The
     * `+ 8` covers the base command and the trailing URL.
     */
    const char **argv = mi_calloc(EXTRACT_BASE_ARGS + (cfg ? cfg->extractor_arg_count : 0) + 8,
                                  sizeof *argv);
    if (!argv) return NULL;
    /* Inline cookies are written to a temp file for yt-dlp, which takes a path;
     * unlinked on every exit path below. */
    char *cookie_tmp = NULL;
    /* The normalised impersonation target, when one was given. Owned here and
     * freed on every exit path, like the cookie temp. */
    char *imp_tmp = NULL;
    int a = 0;
    argv[a++] = binary;
    if (probe_only) {
        argv[a++] = "--version";
    } else if (kind == MI_EXTRACT_GALLERY) {
        /*
         * gallery-dl: dump the metadata, download nothing.
         *
         * Its flags differ from yt-dlp's for the same intent. `-j` is the JSON
         * dump and `--no-download` is the resolve-only switch; there is no
         * `--skip-download`, and no JS runtime or format selection because it is
         * not resolving streams.
         */
        argv[a++] = "-j";
        argv[a++] = "--no-download";
        argv[a++] = "--no-colors";
        /*
         * Cookies, from a file or from memory.
         *
         * yt-dlp takes a path, so inline cookies are written to a 0600 temp file
         * for the length of the resolve and removed straight after. The file is
         * created here and unlinked by the caller below.
         */
        if (cfg && cfg->cookies_file && *cfg->cookies_file) {
            argv[a++] = "--cookies";
            argv[a++] = cfg->cookies_file;
        } else if (cfg && cfg->cookies_text && *cfg->cookies_text) {
            if (write_temp_cookies(cfg->cookies_text, &cookie_tmp) == 0) {
                argv[a++] = "--cookies";
                argv[a++] = cookie_tmp;
            }
        }
        if (cfg && cfg->proxy && *cfg->proxy) {
            argv[a++] = "--proxy";
            argv[a++] = cfg->proxy;
        }
        argv[a++] = "--";
        argv[a++] = url;
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
        } else if (cfg && cfg->cookies_text && *cfg->cookies_text) {
            /* The same from-memory cookies the gallery path takes: yt-dlp wants
             * a path, so they are written to a 0600 temp file for the resolve
             * and removed by the caller below. Without this the video extractor
             * resolved anonymously and a site behind a sign-in answered with a
             * wall. */
            if (write_temp_cookies(cfg->cookies_text, &cookie_tmp) == 0) {
                argv[a++] = "--cookies";
                argv[a++] = cookie_tmp;
            }
        }
        /*
         * The proxy, when the host's own address is refused. yt-dlp resolves
         * *and* the media fetch that follows both go through it.
         */
        if (cfg && cfg->proxy && *cfg->proxy) {
            argv[a++] = "--proxy";
            argv[a++] = cfg->proxy;
        }
        /*
         * Impersonation, when a site fingerprints the client. yt-dlp wears the
         * named browser's TLS/JA3 fingerprint; without this the request is
         * plainly libcurl and a fingerprinting site refuses it.
         *
         * Normalised, because yt-dlp rejects a target with a separator or a
         * capital — "Chrome131" is an error, "chrome131" is not — and that error
         * aborts the whole resolve rather than being ignored.
         */
        if (cfg && cfg->impersonate && *cfg->impersonate) {
            imp_tmp = mi_extract_normalize_impersonate(cfg->impersonate);
            argv[a++] = "--impersonate";
            argv[a++] = imp_tmp ? imp_tmp : cfg->impersonate;
        }
        /*
         * The discovery filters.
         *
         * All four are applied by the extractor *while listing*, which is the
         * point: a filtered-out item is never resolved, so bounding a crawl costs
         * nothing where filtering afterwards would mean transferring everything
         * first. They are only meaningful for a list, but passing one for a
         * single item is harmless — the extractor ignores a filter it cannot
         * apply.
         */
        if (cfg && cfg->date_after && *cfg->date_after) {
            argv[a++] = "--dateafter";
            argv[a++] = cfg->date_after;
        }
        if (cfg && cfg->date_before && *cfg->date_before) {
            argv[a++] = "--datebefore";
            argv[a++] = cfg->date_before;
        }
        if (cfg && cfg->playlist_items && *cfg->playlist_items) {
            argv[a++] = "--playlist-items";
            argv[a++] = cfg->playlist_items;
        }
        if (cfg && cfg->match_filter && *cfg->match_filter) {
            argv[a++] = "--match-filters";
            argv[a++] = cfg->match_filter;
        }
        /*
         * Per-site arguments, verbatim and repeatable.
         *
         * This is the escape hatch when a site changes and the extractor needs a
         * nudge before its next release. The syntax is the extractor's own, so it
         * is passed through rather than wrapped.
         */
        for (size_t i = 0; cfg && i < cfg->extractor_arg_count; i++) {
            if (!cfg->extractor_args[i] || !*cfg->extractor_args[i]) continue;
            argv[a++] = "--extractor-args";
            argv[a++] = cfg->extractor_args[i];
        }
        argv[a++] = "--";
        argv[a++] = url;
    }
    argv[a] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        free(imp_tmp);
        free(argv);
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
    /* The cookie file held a credential; remove it the moment the child is done
     * with it rather than leaving it for a later sweep. */
    if (cookie_tmp) {
        unlink(cookie_tmp);
        free(cookie_tmp);
    }
    free(imp_tmp);
    free(argv);
    if (timed_out) {
        free(buf);
        return NULL;
    }
    return buf;
}

/*
 * A cached answer per extractor: which binaries exist cannot change at runtime.
 *
 * Two entries rather than one, because the tools are independent — a host may
 * have yt-dlp and not gallery-dl, and the console should then offer video search
 * and not the gallery list.
 */
static int g_available[2] = {-1, -1};

int mi_extract_available_kind(const mi_extract_config *cfg, mi_extract_kind kind) {
    int slot = (kind == MI_EXTRACT_GALLERY) ? 1 : 0;
    if (g_available[slot] >= 0) return g_available[slot];
    int code = -1;
    char *out = run_extractor_kind(cfg, kind, NULL, 1, &code);
    g_available[slot] = (out != NULL && code == 0) ? 1 : 0;
    free(out);
    return g_available[slot];
}

int mi_extract_available(const mi_extract_config *cfg) {
    return mi_extract_available_kind(cfg, MI_EXTRACT_VIDEO);
}

/* A string field, or NULL. */
static char *dup_string(const mi_json *o, const char *key) {
    const char *v = mi_json_get_string(o, key, NULL);
    return (v && *v) ? mi_strdup(v) : NULL;
}

/*
 * The best thumbnail URL for an object, or NULL.
 *
 * yt-dlp reports thumbnails two ways and only one of them is usually populated:
 * `thumbnail` is a single URL and is frequently null, while `thumbnails` is a
 * list of `{url, width, height}`. Measured on a search: `thumbnail` was null for
 * every entry and `thumbnails` held two each — so reading only the singular field
 * meant every result card lost its poster, which is exactly the complaint.
 *
 * The list is preferred, and the largest by width within it: a poster is shown in
 * a card, and the biggest of a set of sizes looks right at any card size where
 * the smallest does not.
 */
static char *best_thumbnail(const mi_json *o) {
    const mi_json *list = mi_json_get(o, "thumbnails");
    if (list && list->type == MI_JSON_ARRAY && list->count > 0) {
        const mi_json *best = NULL;
        double best_w = -1;
        for (size_t i = 0; i < list->count; i++) {
            const mi_json *t = list->items[i];
            if (!t || t->type != MI_JSON_OBJECT) continue;
            const char *u = mi_json_get_string(t, "url", NULL);
            if (!u || !*u) continue;
            /* A thumbnail with no stated size still beats none, so it competes
             * with a width of 0 rather than being skipped. */
            double w = mi_json_get_number(t, "width", 0);
            if (!best || w > best_w) {
                best = t;
                best_w = w;
            }
        }
        if (best) {
            const char *u = mi_json_get_string(best, "url", NULL);
            if (u && *u) return mi_strdup(u);
        }
    }
    /* Then the single field, for an extractor that only sets that one. */
    return dup_string(o, "thumbnail");
}

/*
 * gallery-dl's output, as the same result shape.
 *
 * It answers a different question and returns a different shape: a flat list of
 * `[kind, ...]` tuples rather than one object with a formats array.
 *
 *   2  directory   [2, {metadata}]           sets the metadata for what follows
 *   3  file        [3, "url", {metadata}]    one file, with its own URL
 *   6  queue       [6, "url", {metadata}]    an external URL another extractor
 *                                            should handle — a linked post
 *
 * The length is what identifies the kind, not the constant, because the JSON
 * form does not carry the documented 3-element tuple for a directory. Measured:
 * a directory arrives as a 2-element array and a file as a 3-element one.
 *
 * The metadata is carried forward: a file's dict often omits fields the
 * directory set (the category, the uploader), and a card that showed a bare
 * filename would be less useful than one that showed where it came from.
 */
static int parse_gallery(const mi_json *doc, mi_extract_result *out) {
    if (!doc || doc->type != MI_JSON_ARRAY) return MI_ERR_ARG;

    /*
     * The entries array is allocated up front, sized to the whole document.
     *
     * Only some tuples become entries — a directory and a queue are not files —
     * so this over-allocates, which is the right trade: a gallery-dl document is
     * a flat list, and counting the files first would mean walking it twice for
     * a few hundred bytes.
     */
    if (doc->count > 0) {
        out->entries = mi_calloc(doc->count, sizeof *out->entries);
        if (!out->entries) return MI_ERR_NOMEM;
    }

    /* The most recent directory metadata, merged into each file's. */
    const mi_json *dir_meta = NULL;

    for (size_t i = 0; i < doc->count; i++) {
        const mi_json *e = doc->items[i];
        if (!e || e->type != MI_JSON_ARRAY || e->count < 2) continue;
        const mi_json *kind = e->items[0];
        if (!kind || kind->type != MI_JSON_NUMBER) continue;
        int k = (int)kind->number;

        if (k == 2) {
            /* A directory: remember its metadata for the files that follow. */
            const mi_json *meta = e->items[1];
            if (meta && meta->type == MI_JSON_OBJECT) dir_meta = meta;
            continue;
        }
        if (k != 3 && k != 6) continue;

        /* A file (3) or an external URL to queue (6): both carry a URL. */
        const mi_json *url = e->items[1];
        if (!url || url->type != MI_JSON_STRING || !url->string || !*url->string) continue;
        const mi_json *meta = e->count > 2 ? e->items[2] : NULL;
        if (!meta || meta->type != MI_JSON_OBJECT) meta = dir_meta;
        if (!meta || meta->type != MI_JSON_OBJECT) continue;

        const char *fn = mi_json_get_string(meta, "filename", NULL);
        const char *title = mi_json_get_string(meta, "title", NULL);
        const char *cat = mi_json_get_string(meta, "category", NULL);
        const char *ext = mi_json_get_string(meta, "extension", NULL);

        mi_extract_entry *dst = &out->entries[out->entry_count];
        memset(dst, 0, sizeof *dst);
        dst->duration_seconds = -1;
        dst->id = dup_string(meta, "id");
        /*
         * A title, then a filename, then the URL's last segment. A gallery file
         * often has no title at all, and a blank row is worse than a filename.
         */
        if (title && *title) {
            dst->title = mi_strdup(title);
        } else if (fn && *fn) {
            dst->title = mi_strdup(fn);
        } else {
            const char *slash = strrchr(url->string, '/');
            dst->title = mi_strdup(slash && slash[1] ? slash + 1 : url->string);
        }
        dst->url = mi_strdup(url->string);
        dst->uploader = dup_string(meta, "username");
        if (!dst->uploader) dst->uploader = dup_string(meta, "owner");
        if (!dst->uploader) dst->uploader = dup_string(meta, "artist");
        dst->thumbnail = best_thumbnail(meta);
        /* An image has no duration; a queued post might. */
        dst->duration_seconds = mi_json_get_number(meta, "duration", -1);
        /* A queued external URL is not itself a file — it is somewhere to look. */
        dst->is_playlist = (k == 6);
        (void)cat;
        (void)ext;
        out->entry_count++;
    }

    /* A gallery has no single title of its own; the first file's category is the
     * closest thing, and the console shows the site from the URL anyway. */
    if (dir_meta) {
        const char *cat = mi_json_get_string(dir_meta, "category", NULL);
        if (cat && *cat) out->extractor = mi_strdup(cat);
    }
    return MI_OK;
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

    /*
     * Which tool to ask.
     *
     * A photo site and a video site are different questions, and the two tools
     * answer one each: yt-dlp returns streams and formats, gallery-dl returns the
     * images on a page. The choice is by host (or an explicit setting), and it
     * decides both the command and the parser.
     */
    mi_extract_kind kind = mi_extract_pick(cfg, url);

    int code = -1;
    char *json = run_extractor_kind(cfg, kind, query, 0, &code);
    free(query);
    if (!json) {
        mi_logf(MI_LOG_ERROR, "extract: %s produced no output (exit %d)",
                kind == MI_EXTRACT_GALLERY ? "gallery-dl" : "yt-dlp", code);
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

    /*
     * gallery-dl answers in its own shape — a flat list of tuples — so it gets
     * its own parser and returns from here. Everything below is the yt-dlp shape.
     */
    if (kind == MI_EXTRACT_GALLERY) {
        int rc = parse_gallery(doc, out);
        mi_json_value_free(doc);
        return rc;
    }

    out->title = dup_string(doc, "title");
    out->extractor = dup_string(doc, "extractor");
    out->thumbnail = best_thumbnail(doc);
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
                /*
                 * Whether the URL is a file or a manifest.
                 *
                 * yt-dlp reports `protocol`; a direct one ("https", "http") is a
                 * file a downloader can range, while "m3u8_native"/"m3u8" is an
                 * HLS variant and "mhtml" a storyboard. Only the direct ones can
                 * be handed to a plain fetcher; the rest need a reader, and
                 * choosing one by height alone picked exactly the ones a plain
                 * fetch cannot use.
                 *
                 * Absent protocol means "assume direct": most extractors do not
                 * report it, and the ones that do report it for every format.
                 */
                dst->protocol = dup_string(f, "protocol");
                dst->direct = 1;
                if (dst->protocol && *dst->protocol) {
                    if (!strncasecmp(dst->protocol, "m3u8", 4) ||
                        !strcasecmp(dst->protocol, "dash") ||
                        !strcasecmp(dst->protocol, "mhtml") ||
                        !strcasecmp(dst->protocol, "rtmp")) {
                        dst->direct = 0;
                    }
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
                const char *vext = mi_json_get_string(f, "video_ext", NULL);
                const char *aext = mi_json_get_string(f, "audio_ext", NULL);
                int v_known = vc != NULL;
                int a_known = ac != NULL;
                /*
                 * Absent means unknown, so the other fields decide. `video_ext`
                 * counts here for the same reason `audio_ext` does below: a
                 * format that names a video container carries a picture even
                 * when the extractor never reported a codec or a height —
                 * measured on archive.org's AVI entries, which resolve to a
                 * single format with `video_ext: "avi"` and every other video
                 * field null. Without it those formats were dropped and the page
                 * reported "no video stream".
                 */
                dst->has_video = v_known ? (vc[0] && strcmp(vc, "none") != 0)
                                         : (dst->height > 0 || (res && *res) ||
                                            (vext && *vext && strcmp(vext, "none") != 0));
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
                dst->thumbnail = best_thumbnail(e);
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
