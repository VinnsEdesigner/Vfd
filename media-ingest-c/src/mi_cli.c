/* mi_cli.c - command line front end.
 *
 *   vyzor detect  <url>
 *   vyzor fetch   <url> -o <dir> [-c N] [-H "K: V"] ...
 *   vyzor detect-file <path>
 *   vyzor hls     <input> -o <dir> [--renditions 1080,720,480]
 *   vyzor ingest  <url> -o <dir> --hls <hlsdir>
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <getopt.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>

#include "mi_detect.h"
#include "mi_download.h"
#include "mi_hls.h"
#include "mi_server.h"
#include "mi_storage.h"
#include "mi_util.h"

#define MI_VERSION MI_VYZOR_VERSION

/* ------------------------------------------------------------------ */
/* storage option bundle                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int enabled;
    const char *endpoint;
    const char *region;
    const char *bucket;
    const char *access_key;
    const char *secret_key;
    const char *session_token;
    const char *content_type;
    const char *cdn_base;
    const char *cf_token;
    const char *cf_zone;
    const char *prefix;
    int path_style;
    int purge;
} storage_opts;

static void storage_opts_init(storage_opts *o) {
    memset(o, 0, sizeof *o);
    o->endpoint = "https://s3.amazonaws.com";
    o->region = "us-east-1";
    o->path_style = 1;
}

static void storage_opts_to_config(const storage_opts *o, mi_s3_config *cfg) {
    mi_s3_config_init(cfg);
    cfg->endpoint = o->endpoint;
    cfg->region = o->region;
    cfg->bucket = o->bucket;
    cfg->access_key = o->access_key;
    cfg->secret_key = o->secret_key;
    cfg->session_token = o->session_token;
    cfg->content_type = o->content_type;
    cfg->path_style = o->path_style;
}

/* Build "<prefix>/<relative>" with the leading dir stripped from the local path. */
static char *object_key_for(const storage_opts *o, const char *base_dir,
                            const char *local_path) {
    const char *rel = local_path;
    if (base_dir) {
        size_t n = strlen(base_dir);
        if (!strncmp(local_path, base_dir, n) && local_path[n] == '/') rel = local_path + n + 1;
    }
    /* Strip a leading "./" for tidiness. */
    while (!strncmp(rel, "./", 2)) rel += 2;
    if (o->prefix && *o->prefix) {
        size_t n = strlen(o->prefix);
        int slash = o->prefix[n - 1] == '/';
        return mi_asprintf("%s%s%s", o->prefix, slash ? "" : "/", rel);
    }
    return mi_strdup(rel);
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void json_escape_print(const char *s) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        switch (*p) {
            case '"': fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            default:
                if (*p < 0x20) printf("\\u%04x", *p);
                else putchar(*p);
        }
    }
    putchar('"');
}

static void print_media_json(const mi_media_info *info) {
    printf("{\n");
    printf("    \"kind\": \"%s\",\n", mi_kind_name(info->kind));
    printf("    \"confidence\": %.3f,\n", info->confidence);
    printf("    \"container\": ");
    json_escape_print(info->container);
    printf(",\n    \"is_video\": %s", info->is_video ? "true" : "false");
    if (info->manifest_count > 0) {
        printf(",\n    \"manifest_urls\": [");
        for (size_t i = 0; i < info->manifest_count; i++) {
            if (i) printf(", ");
            json_escape_print(info->manifest_urls[i]);
        }
        printf("]");
    }
    printf("\n  }");
}

static void progress_cb(const mi_progress *p, void *user) {
    (void)user;
    if (mi_log_get_level() < MI_LOG_INFO) return;
    if (p->total_bytes > 0) {
        double frac = (double)p->downloaded_bytes / (double)p->total_bytes * 100.0;
        fprintf(stderr, "\r  %5.1f%%  %8.2f / %.2f MB", frac,
                p->downloaded_bytes / 1e6, p->total_bytes / 1e6);
    } else {
        fprintf(stderr, "\r  %8.2f MB", p->downloaded_bytes / 1e6);
    }
    if (p->finished) fputc('\n', stderr);
    fflush(stderr);
}

static char **append_header(char **headers, size_t *count, const char *h) {
    char **grown = mi_realloc(headers, (*count + 1) * sizeof(char *));
    if (!grown) return headers;
    grown[*count] = mi_strdup(h);
    (*count)++;
    return grown;
}

/* ------------------------------------------------------------------ */
/* directory walking (for HLS uploads)                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    char **items;
    size_t count;
} path_list;

static void path_list_add(path_list *l, const char *p) {
    char **grown = mi_realloc(l->items, (l->count + 1) * sizeof(char *));
    if (!grown) return;
    l->items = grown;
    l->items[l->count++] = mi_strdup(p);
}

static void path_list_free(path_list *l) {
    for (size_t i = 0; i < l->count; i++) free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->count = 0;
}

static void walk_dir(const char *dir, path_list *out) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char *child = mi_path_join(dir, ent->d_name);
        if (mi_path_is_dir(child)) {
            walk_dir(child, out);
        } else {
            path_list_add(out, child);
        }
        free(child);
    }
    closedir(d);
}

/* ------------------------------------------------------------------ */
/* storage + CDN                                                       */
/* ------------------------------------------------------------------ */

static const char *content_type_for(const char *path, const char *override_ct) {
    if (override_ct) return override_ct;
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".m3u8")) return "application/vnd.apple.mpegurl";
    if (!strcmp(dot, ".ts")) return "video/mp2t";
    if (!strcmp(dot, ".mp4") || !strcmp(dot, ".m4v")) return "video/mp4";
    if (!strcmp(dot, ".webm")) return "video/webm";
    if (!strcmp(dot, ".mkv")) return "video/x-matroska";
    if (!strcmp(dot, ".mpd")) return "application/dash+xml";
    if (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcmp(dot, ".png")) return "image/png";
    return "application/octet-stream";
}

/* Upload one local file; prints a JSON line per object when `verbose_json`. */
static int upload_one(const storage_opts *o, const char *base_dir,
                      const char *local_path, char ***urls_out, size_t *n_out) {
    mi_s3_config cfg;
    storage_opts_to_config(o, &cfg);
    cfg.content_type = content_type_for(local_path, o->content_type);

    char *key = object_key_for(o, base_dir, local_path);
    if (!key) return MI_ERR_NOMEM;

    mi_upload_result up;
    int rc = mi_s3_upload_file(&cfg, local_path, key, &up);
    if (rc != MI_OK) {
        mi_logf(MI_LOG_ERROR, "upload failed for %s", local_path);
        free(key);
        return rc;
    }
    char *public_url = mi_storage_public_url(&cfg, o->cdn_base, key);
    mi_logf(MI_LOG_INFO, "uploaded %s -> %s (%lld bytes)", local_path,
            public_url ? public_url : key, (long long)up.bytes);

    if (urls_out) {
        char **grown = mi_realloc(*urls_out, (*n_out + 1) * sizeof(char *));
        if (grown) {
            *urls_out = grown;
            (*urls_out)[*n_out] = mi_strdup(public_url ? public_url : key);
            (*n_out)++;
        }
    }
    free(public_url);
    free(key);
    mi_upload_result_free(&up);
    return MI_OK;
}

/* Upload a directory tree (used for HLS output: playlists + segments). */
static int upload_tree(const storage_opts *o, const char *dir,
                       char ***urls_out, size_t *n_out) {
    path_list files = {0};
    walk_dir(dir, &files);
    int rc = MI_OK;
    for (size_t i = 0; i < files.count; i++) {
        if (upload_one(o, dir, files.items[i], urls_out, n_out) != MI_OK) rc = MI_ERR;
    }
    path_list_free(&files);
    return rc;
}

static void maybe_purge(const storage_opts *o, char **urls, size_t n) {
    if (!o->purge || n == 0) return;
    if (!o->cf_token || !o->cf_zone) {
        mi_logf(MI_LOG_ERROR, "purge requested but --cf-token/--cf-zone missing");
        return;
    }
    mi_cdn_config cdn;
    mi_cdn_config_init(&cdn);
    cdn.api_token = o->cf_token;
    cdn.zone_id = o->cf_zone;

    mi_cdn_result res;
    if (mi_cdn_purge_urls(&cdn, (const char *const *)urls, n, &res) == MI_OK) {
        mi_logf(MI_LOG_INFO, "purged %zu URL(s) from CDN", n);
    } else {
        mi_logf(MI_LOG_ERROR, "CDN purge failed");
    }
    mi_cdn_result_free(&res);
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

static int cmd_detect(const char *url, char **headers, size_t header_count) {
    mi_download_config cfg;
    mi_download_config_init(&cfg);
    cfg.require_media = 0;
    cfg.headers = headers;
    cfg.header_count = header_count;

    mi_download_plan plan;
    if (mi_probe(&cfg, url, &plan) != MI_OK) {
        mi_logf(MI_LOG_ERROR, "probe failed");
        return 1;
    }
    printf("{\n  \"url\": ");
    json_escape_print(url);
    printf(",\n  \"final_url\": ");
    json_escape_print(plan.final_url ? plan.final_url : url);
    printf(",\n  \"content_type\": ");
    json_escape_print(plan.content_type ? plan.content_type : "");
    printf(",\n  \"total_bytes\": %lld,\n  \"accept_ranges\": %s,\n  \"media\": ",
           (long long)plan.total_bytes, plan.accept_ranges ? "true" : "false");
    if (plan.have_media) print_media_json(&plan.media);
    else printf("null");
    printf("\n}\n");
    mi_download_plan_free(&plan);
    return 0;
}

static int cmd_detect_file(const char *path) {
    mi_media_info info;
    if (mi_detect_file(path, 65536, &info) != MI_OK) {
        mi_logf(MI_LOG_ERROR, "cannot read %s", path);
        return 1;
    }
    print_media_json(&info);
    printf("\n");
    mi_media_info_free(&info);
    return 0;
}

static int cmd_fetch(const char *url, const char *out, const char *name,
                     int connections, int64_t chunk_size, int allow_non_video,
                     int64_t max_bytes, char **headers, size_t header_count,
                     int quiet) {
    mi_download_config cfg;
    mi_download_config_init(&cfg);
    cfg.connections = connections;
    cfg.chunk_size = chunk_size;
    cfg.require_media = !allow_non_video;
    cfg.max_bytes = max_bytes;
    cfg.headers = headers;
    cfg.header_count = header_count;

    if (mi_mkdir_p(out) != 0) {
        mi_logf(MI_LOG_ERROR, "cannot create %s", out);
        return 1;
    }
    char *dest = NULL;
    if (name) {
        dest = mi_path_join(out, name);
    } else {
        const char *base = strrchr(url, '/');
        base = base ? base + 1 : url;
        char *clean = mi_strdup(base);
        char *q = clean ? strpbrk(clean, "?#") : NULL;
        if (q) *q = '\0';
        dest = mi_path_join(out, (clean && *clean) ? clean : "download.bin");
        free(clean);
    }

    mi_download_result res;
    int rc = mi_download(&cfg, url, dest, quiet ? NULL : progress_cb, NULL, &res);
    if (rc != MI_OK) {
        mi_logf(MI_LOG_ERROR, "download failed (%d)", rc);
        free(dest);
        return 1;
    }
    printf("{\n  \"path\": ");
    json_escape_print(res.path);
    printf(",\n  \"bytes\": %lld,\n  \"seconds\": %.2f,\n  \"resumed\": %s,\n  \"media\": ",
           (long long)res.bytes_written, res.elapsed_seconds,
           res.resumed ? "true" : "false");
    print_media_json(&res.media);
    printf("\n}\n");

    mi_download_result_free(&res);
    free(dest);
    return 0;
}

static int cmd_hls(const char *input, const char *out_dir, const char *renditions,
                   int segment_seconds) {
    mi_hls_config cfg;
    mi_hls_config_init(&cfg);
    if (segment_seconds > 0) cfg.segment_seconds = segment_seconds;

    mi_rendition custom[16];
    size_t custom_count = 0;
    if (renditions && *renditions) {
        char *copy = mi_strdup(renditions);
        char *save = NULL;
        for (char *tok = strtok_r(copy, ",", &save); tok && custom_count < 16;
             tok = strtok_r(NULL, ",", &save)) {
            int h = atoi(tok);
            if (h <= 0) continue;
            custom[custom_count].height = h;
            /* Bitrate heuristic: ~ height^1.5 scaled; keep it simple/linear. */
            custom[custom_count].video_kbps = (int)(h * 4.6);
            custom[custom_count].audio_kbps = h >= 720 ? 128 : 96;
            custom_count++;
        }
        free(copy);
    }
    if (custom_count > 0) {
        cfg.renditions = custom;
        cfg.rendition_count = custom_count;
    }

    mi_hls_result res;
    int rc = mi_hls_transcode(&cfg, input, out_dir, &res);
    if (rc != MI_OK) {
        mi_logf(MI_LOG_ERROR, "transcode failed");
        return 1;
    }
    printf("{\n  \"master_playlist\": ");
    json_escape_print(res.master_playlist);
    printf(",\n  \"output_dir\": ");
    json_escape_print(res.output_dir);
    printf(",\n  \"variants\": [");
    for (size_t i = 0; i < res.rendition_count; i++) {
        if (i) printf(", ");
        json_escape_print(res.variant_playlists[i]);
    }
    printf("]\n}\n");
    mi_hls_result_free(&res);
    return 0;
}

static int cmd_ingest(const char *url, const char *out, const char *hls_dir,
                      int connections, int64_t chunk_size, char **headers,
                      size_t header_count, int quiet) {
    /* Download first. */
    mi_download_config cfg;
    mi_download_config_init(&cfg);
    cfg.connections = connections;
    cfg.chunk_size = chunk_size;
    cfg.headers = headers;
    cfg.header_count = header_count;

    if (mi_mkdir_p(out) != 0) return 1;
    char *dest = mi_path_join(out, "source.bin");
    mi_download_result res;
    int rc = mi_download(&cfg, url, dest, quiet ? NULL : progress_cb, NULL, &res);
    if (rc != MI_OK) {
        mi_logf(MI_LOG_ERROR, "download failed (%d)", rc);
        free(dest);
        return 1;
    }

    printf("{\n  \"downloaded\": ");
    json_escape_print(res.path);
    printf(",\n  \"bytes\": %lld,\n  \"media\": ", (long long)res.bytes_written);
    print_media_json(&res.media);
    printf(",\n  \"hls\": ");
    mi_download_result_free(&res);

    if (hls_dir && *hls_dir) {
        mi_hls_config hcfg;
        mi_hls_config_init(&hcfg);
        mi_hls_result hres;
        if (mi_hls_transcode(&hcfg, dest, hls_dir, &hres) == MI_OK) {
            printf("{");
            printf("\"master_playlist\": ");
            json_escape_print(hres.master_playlist);
            printf(", \"variants\": [");
            for (size_t i = 0; i < hres.rendition_count; i++) {
                if (i) printf(", ");
                json_escape_print(hres.variant_playlists[i]);
            }
            printf("]}");
            mi_hls_result_free(&hres);
        } else {
            printf("null");
        }
    } else {
        printf("null");
    }
    printf("\n}\n");
    free(dest);
    return 0;
}

/* ------------------------------------------------------------------ */
/* serve                                                               */
/* ------------------------------------------------------------------ */

static mi_server *g_server = NULL;

static void on_signal(int sig) {
    (void)sig;
    if (g_server) mi_server_stop(g_server);
}

static int cmd_serve(const char *host, int port, const char *work_dir,
                     const char *auth_token, const char *allow_origins,
                     const char *user_agent, int render, int render_timeout,
                     int render_concurrent, const char *render_cache_dir,
                     int render_cache_ttl, int interactive, int interactive_concurrent,
                     int interactive_idle, int interactive_keepalive,
                     int interactive_silence, int rtc, int rtc_concurrent, int rtc_fps,
                     int rtc_bitrate, const char *rtc_stun, const char *proxy,
                     const char *impersonate, int identity_retries) {
    mi_server_config cfg;
    mi_server_config_init(&cfg);
    if (host) cfg.host = host;
    if (port >= 0) cfg.port = port;
    if (work_dir) cfg.work_dir = work_dir;
    cfg.auth_token = (auth_token && *auth_token) ? auth_token : NULL;
    cfg.allow_origins = (allow_origins && *allow_origins) ? allow_origins : NULL;
    cfg.user_agent = (user_agent && *user_agent) ? user_agent : NULL;
    cfg.proxy = (proxy && *proxy) ? proxy : NULL;
    cfg.impersonate = (impersonate && *impersonate) ? impersonate : NULL;
    if (identity_retries > 0) cfg.identity_retries = identity_retries;
    cfg.render_enabled = render;
    if (render_timeout > 0) cfg.render_timeout_seconds = render_timeout;
    if (render_concurrent > 0) cfg.render_max_concurrent = render_concurrent;
    if (render_cache_dir) cfg.render_cache_dir = render_cache_dir;
    if (render_cache_ttl > 0) cfg.render_cache_ttl_seconds = render_cache_ttl;
    cfg.interactive_enabled = interactive;
    if (interactive_concurrent > 0) cfg.interactive_max_concurrent = interactive_concurrent;
    if (interactive_idle > 0) cfg.interactive_idle_seconds = interactive_idle;
    if (interactive_keepalive > 0) cfg.interactive_keepalive_ping_seconds = interactive_keepalive;
    if (interactive_silence > 0) cfg.interactive_keepalive_timeout_seconds = interactive_silence;
    cfg.rtc_enabled = rtc;
    if (rtc_concurrent > 0) cfg.rtc_max_concurrent = rtc_concurrent;
    if (rtc_fps > 0) cfg.rtc_fps = rtc_fps;
    if (rtc_bitrate > 0) cfg.rtc_bitrate_kbps = rtc_bitrate;
    cfg.rtc_stun_server = rtc_stun;

    mi_mkdir_p(cfg.work_dir);

    mi_server *srv = mi_server_new(&cfg);
    if (!srv) {
        mi_logf(MI_LOG_ERROR, "cannot allocate server");
        return 1;
    }
    if (mi_server_listen(srv) != MI_OK) {
        mi_logf(MI_LOG_ERROR, "cannot bind %s:%d", cfg.host, cfg.port);
        mi_server_free(srv);
        return 1;
    }
    g_server = srv;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* Emit the actual bound port on stdout so a supervisor can read it. */
    printf("{\"listening\":\"%s\",\"port\":%d,\"work_dir\":\"%s\"}\n",
           cfg.host, mi_server_port(srv), cfg.work_dir);
    fflush(stdout);

    mi_server_run(srv);
    mi_server_free(srv);
    g_server = NULL;
    return 0;
}

static int cmd_upload(const char *local, const storage_opts *o) {
    if (!o->bucket) {
        mi_logf(MI_LOG_ERROR, "upload requires --bucket (and --endpoint/--access-key/--secret-key)");
        return 2;
    }
    char **urls = NULL;
    size_t n_urls = 0;
    int rc;
    if (mi_path_is_dir(local)) {
        rc = upload_tree(o, local, &urls, &n_urls);
    } else {
        rc = upload_one(o, NULL, local, &urls, &n_urls);
    }
    maybe_purge(o, urls, n_urls);

    printf("{\n  \"uploaded\": %zu,\n  \"urls\": [", n_urls);
    for (size_t i = 0; i < n_urls; i++) {
        if (i) printf(", ");
        json_escape_print(urls[i]);
    }
    printf("]\n}\n");
    for (size_t i = 0; i < n_urls; i++) free(urls[i]);
    free(urls);
    return rc == MI_OK ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(void) {
    fprintf(stderr,
            "vyzor " MI_VERSION "\n"
            "Usage:\n"
            "  vyzor detect <url> [-H 'K: V']...\n"
            "  vyzor detect-file <path>\n"
            "  vyzor fetch <url> -o <dir> [-n name] [-c N] [--chunk-size N]\n"
            "                          [--allow-non-video] [--max-bytes N] [-q] [-H 'K: V']...\n"
            "  vyzor hls <input> -o <dir> [--renditions 1080,720,480] [--segment-seconds N]\n"
            "  vyzor ingest <url> -o <dir> [--hls <dir>] [--upload] [-c N] [-q] [-H 'K: V']...\n"
            "  vyzor serve [--host H] [--port N] [--work-dir D] [--auth-token T]\n"
            "                      [--allow-origin ORIGINS] [--user-agent UA] [--proxy URL] [--impersonate CLIENT] [--identity-retries N]\n"
            "                      [--render] [--render-timeout SECONDS] [--render-concurrent N]\n"
            "                      [--render-cache DIR] [--render-cache-ttl SECONDS] [--no-render-cache]\n"
            "                      [--interactive] [--interactive-concurrent N] [--interactive-idle SECONDS]\n"
            "                      [--interactive-keepalive SECONDS] [--interactive-silence SECONDS]\n"
            "                      [--rtc] [--rtc-concurrent N] [--rtc-fps N] [--rtc-bitrate KBPS]\n"
            "                      [--rtc-stun URL]\n"
            "  vyzor upload <file-or-dir> --bucket B --access-key K --secret-key S\n"
            "                          [--endpoint URL] [--region R] [--prefix P] [--cdn-base URL]\n"
            "                          [--path-style|--virtual-host] [--purge --cf-token T --cf-zone Z]\n"
            "Storage (S3-compatible: AWS S3, Cloudflare R2, MinIO, B2, Spaces):\n"
            "  --upload            after ingest, push the HLS output to storage\n"
            "  --endpoint URL      default https://s3.amazonaws.com\n"
            "  --region R          default us-east-1 (use 'auto' for R2)\n"
            "  --bucket B          target bucket\n"
            "  --access-key K      access key id (env MI_S3_ACCESS_KEY)\n"
            "  --secret-key S      secret access key (env MI_S3_SECRET_KEY)\n"
            "  --session-token T   optional STS session token\n"
            "  --prefix P          key prefix, e.g. hls/movie-42/\n"
            "  --content-type CT   override Content-Type for a single-file upload\n"
            "  --virtual-host      use bucket-in-host addressing (default is path-style)\n"
            "  --cdn-base URL      public CDN base used to build returned URLs\n"
            "  --purge             purge uploaded URLs from Cloudflare after upload\n"
            "  --cf-token T        Cloudflare API token (env MI_CF_API_TOKEN)\n"
            "  --cf-zone Z         Cloudflare zone id (env MI_CF_ZONE_ID)\n"
            "Global:\n"
            "  -v, --verbose   debug logging\n"
            "  -q, --quiet     errors only\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 2;
    }

    enum {
        CMD_NONE, CMD_DETECT, CMD_DETECT_FILE, CMD_FETCH, CMD_HLS, CMD_INGEST,
        CMD_UPLOAD, CMD_SERVE
    };
    const char *cmd = argv[1];
    int command = CMD_NONE;
    if (!strcmp(cmd, "detect")) command = CMD_DETECT;
    else if (!strcmp(cmd, "detect-file")) command = CMD_DETECT_FILE;
    else if (!strcmp(cmd, "fetch")) command = CMD_FETCH;
    else if (!strcmp(cmd, "hls")) command = CMD_HLS;
    else if (!strcmp(cmd, "ingest")) command = CMD_INGEST;
    else if (!strcmp(cmd, "upload")) command = CMD_UPLOAD;
    else if (!strcmp(cmd, "serve")) command = CMD_SERVE;
    else {
        usage();
        return 2;
    }

    const char *positional = NULL;
    const char *out = "downloads";
    const char *name = NULL;
    const char *hls_dir = NULL;
    const char *renditions = NULL;
    int connections = 8;
    int64_t chunk_size = 4 * 1024 * 1024;
    int64_t max_bytes = 0;
    int allow_non_video = 0;
    int quiet = 0;
    int verbose = 0;
    int segment_seconds = 6;
    char **headers = NULL;
    size_t header_count = 0;
    int do_upload = 0;
    /*
     * The serve settings, with environment fallbacks.
     *
     * A container is configured through its environment, not through a command
     * line assembled by hand — and a documented variable that nothing reads is
     * worse than no variable at all, because it looks like it worked. A flag
     * still wins, so the command line stays the more specific thing.
     */
    const char *serve_host = getenv("MI_HOST");
    int serve_port = -1;
    const char *serve_work_dir = getenv("MI_WORK_DIR");
    const char *serve_token = getenv("MI_AUTH_TOKEN");
    const char *serve_allow_origins = getenv("MI_ALLOW_ORIGINS");
    {
        const char *p = getenv("MI_PORT");
        if (p && *p) serve_port = atoi(p);
    }
    /* Some hosts refuse a request that does not look like a browser, so what the
     * Vyzor calls itself is settable rather than compiled in. */
    const char *serve_user_agent = getenv("MI_USER_AGENT");
    const char *serve_proxy = getenv("MI_PROXY");
    const char *serve_impersonate = getenv("MI_IMPERSONATE");
    int serve_identity_retries = 0;
    /* Rendering is opt-in: it costs about half a gigabyte per render and needs
     * a browser on the host. MI_RENDER=1 turns it on without a flag. */
    int serve_render = getenv("MI_RENDER") != NULL;
    int serve_render_timeout = 0;
    int serve_render_concurrent = 0;
    /* Live sessions are their own opt-in: MI_INTERACTIVE=1 turns them on. */
    int serve_interactive = getenv("MI_INTERACTIVE") != NULL;
    int serve_interactive_concurrent = 0;
    int serve_interactive_idle = 0;
    int serve_interactive_keepalive = 0;
    int serve_interactive_silence = 0;
    int serve_rtc = getenv("MI_RTC") != NULL;
    int serve_rtc_concurrent = 0;
    int serve_rtc_fps = 0;
    int serve_rtc_bitrate = 0;
    const char *serve_rtc_stun = NULL;
    /* The render cache is on by default, under the work dir; these only change
     * where it lives or how long an entry is trusted. */
    const char *serve_render_cache = NULL;
    int serve_render_cache_ttl = 0;

    storage_opts so;
    storage_opts_init(&so);
    /* Environment fallbacks (never echo secrets back). */
    so.access_key = getenv("MI_S3_ACCESS_KEY");
    so.secret_key = getenv("MI_S3_SECRET_KEY");
    so.session_token = getenv("MI_S3_SESSION_TOKEN");
    so.cf_token = getenv("MI_CF_API_TOKEN");
    so.cf_zone = getenv("MI_CF_ZONE_ID");
    const char *env_endpoint = getenv("MI_S3_ENDPOINT");
    const char *env_region = getenv("MI_S3_REGION");
    const char *env_bucket = getenv("MI_S3_BUCKET");
    if (env_endpoint) so.endpoint = env_endpoint;
    if (env_region) so.region = env_region;
    if (env_bucket) so.bucket = env_bucket;

    enum {
        OPT_CHUNK = 1000, OPT_MAXBYTES, OPT_ALLOWNONVIDEO, OPT_HLS, OPT_RENDITIONS,
        OPT_SEGMENT, OPT_UPLOAD, OPT_ENDPOINT, OPT_REGION, OPT_BUCKET, OPT_ACCESS,
        OPT_SECRET, OPT_SESSION, OPT_PREFIX, OPT_CONTENTTYPE, OPT_VHOST, OPT_PATHSTYLE,
        OPT_CDN_BASE, OPT_PURGE, OPT_CF_TOKEN, OPT_CF_ZONE,
        OPT_HOST, OPT_PORT, OPT_WORKDIR, OPT_AUTHTOKEN, OPT_ALLOWORIGIN, OPT_USERAGENT,
        OPT_PROXY, OPT_IMPERSONATE, OPT_IDENTITY_RETRIES,
        OPT_RENDER, OPT_RENDER_TIMEOUT, OPT_RENDER_CONCURRENT,
        OPT_RENDER_CACHE, OPT_RENDER_CACHE_TTL, OPT_NO_RENDER_CACHE,
        OPT_INTERACTIVE, OPT_INTERACTIVE_CONCURRENT, OPT_INTERACTIVE_IDLE,
        OPT_INTERACTIVE_KEEPALIVE, OPT_INTERACTIVE_SILENCE,
        OPT_RTC, OPT_RTC_CONCURRENT, OPT_RTC_FPS, OPT_RTC_BITRATE, OPT_RTC_STUN
    };
    static struct option long_opts[] = {
        {"output", required_argument, 0, 'o'},
        {"name", required_argument, 0, 'n'},
        {"connections", required_argument, 0, 'c'},
        {"chunk-size", required_argument, 0, OPT_CHUNK},
        {"max-bytes", required_argument, 0, OPT_MAXBYTES},
        {"allow-non-video", no_argument, 0, OPT_ALLOWNONVIDEO},
        {"hls", required_argument, 0, OPT_HLS},
        {"renditions", required_argument, 0, OPT_RENDITIONS},
        {"segment-seconds", required_argument, 0, OPT_SEGMENT},
        {"upload", no_argument, 0, OPT_UPLOAD},
        {"endpoint", required_argument, 0, OPT_ENDPOINT},
        {"region", required_argument, 0, OPT_REGION},
        {"bucket", required_argument, 0, OPT_BUCKET},
        {"access-key", required_argument, 0, OPT_ACCESS},
        {"secret-key", required_argument, 0, OPT_SECRET},
        {"session-token", required_argument, 0, OPT_SESSION},
        {"prefix", required_argument, 0, OPT_PREFIX},
        {"content-type", required_argument, 0, OPT_CONTENTTYPE},
        {"virtual-host", no_argument, 0, OPT_VHOST},
        {"path-style", no_argument, 0, OPT_PATHSTYLE},
        {"cdn-base", required_argument, 0, OPT_CDN_BASE},
        {"purge", no_argument, 0, OPT_PURGE},
        {"cf-token", required_argument, 0, OPT_CF_TOKEN},
        {"cf-zone", required_argument, 0, OPT_CF_ZONE},
        {"host", required_argument, 0, OPT_HOST},
        {"port", required_argument, 0, OPT_PORT},
        {"work-dir", required_argument, 0, OPT_WORKDIR},
        {"auth-token", required_argument, 0, OPT_AUTHTOKEN},
        {"allow-origin", required_argument, 0, OPT_ALLOWORIGIN},
        {"user-agent", required_argument, 0, OPT_USERAGENT},
        {"proxy", required_argument, 0, OPT_PROXY},
        {"impersonate", required_argument, 0, OPT_IMPERSONATE},
        {"identity-retries", required_argument, 0, OPT_IDENTITY_RETRIES},
        {"render", no_argument, 0, OPT_RENDER},
        {"render-timeout", required_argument, 0, OPT_RENDER_TIMEOUT},
        {"render-concurrent", required_argument, 0, OPT_RENDER_CONCURRENT},
        {"render-cache", required_argument, 0, OPT_RENDER_CACHE},
        {"render-cache-ttl", required_argument, 0, OPT_RENDER_CACHE_TTL},
        {"no-render-cache", no_argument, 0, OPT_NO_RENDER_CACHE},
        {"interactive", no_argument, 0, OPT_INTERACTIVE},
        {"interactive-concurrent", required_argument, 0, OPT_INTERACTIVE_CONCURRENT},
        {"interactive-idle", required_argument, 0, OPT_INTERACTIVE_IDLE},
        {"interactive-keepalive", required_argument, 0, OPT_INTERACTIVE_KEEPALIVE},
        {"interactive-silence", required_argument, 0, OPT_INTERACTIVE_SILENCE},
        {"rtc", no_argument, 0, OPT_RTC},
        {"rtc-concurrent", required_argument, 0, OPT_RTC_CONCURRENT},
        {"rtc-fps", required_argument, 0, OPT_RTC_FPS},
        {"rtc-bitrate", required_argument, 0, OPT_RTC_BITRATE},
        {"rtc-stun", required_argument, 0, OPT_RTC_STUN},
        {"header", required_argument, 0, 'H'},
        {"quiet", no_argument, 0, 'q'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};

    optind = 2; /* skip the subcommand */
    int c;
    while ((c = getopt_long(argc, argv, "o:n:c:H:qvh", long_opts, NULL)) != -1) {
        switch (c) {
            case 'o': out = optarg; break;
            case 'n': name = optarg; break;
            case 'c': connections = atoi(optarg); break;
            case 'H': headers = append_header(headers, &header_count, optarg); break;
            case 'q': quiet = 1; break;
            case 'v': verbose = 1; break;
            case 'h': usage(); return 0;
            case OPT_CHUNK: chunk_size = atoll(optarg); break;
            case OPT_MAXBYTES: max_bytes = atoll(optarg); break;
            case OPT_ALLOWNONVIDEO: allow_non_video = 1; break;
            case OPT_HLS: hls_dir = optarg; break;
            case OPT_RENDITIONS: renditions = optarg; break;
            case OPT_SEGMENT: segment_seconds = atoi(optarg); break;
            case OPT_UPLOAD: do_upload = 1; break;
            case OPT_ENDPOINT: so.endpoint = optarg; break;
            case OPT_REGION: so.region = optarg; break;
            case OPT_BUCKET: so.bucket = optarg; break;
            case OPT_ACCESS: so.access_key = optarg; break;
            case OPT_SECRET: so.secret_key = optarg; break;
            case OPT_SESSION: so.session_token = optarg; break;
            case OPT_PREFIX: so.prefix = optarg; break;
            case OPT_CONTENTTYPE: so.content_type = optarg; break;
            case OPT_VHOST: so.path_style = 0; break;
            case OPT_PATHSTYLE: so.path_style = 1; break;
            case OPT_CDN_BASE: so.cdn_base = optarg; break;
            case OPT_PURGE: so.purge = 1; break;
            case OPT_CF_TOKEN: so.cf_token = optarg; break;
            case OPT_CF_ZONE: so.cf_zone = optarg; break;
            case OPT_HOST: serve_host = optarg; break;
            case OPT_PORT: serve_port = atoi(optarg); break;
            case OPT_WORKDIR: serve_work_dir = optarg; break;
            case OPT_AUTHTOKEN: serve_token = optarg; break;
            case OPT_ALLOWORIGIN: serve_allow_origins = optarg; break;
            case OPT_USERAGENT: serve_user_agent = optarg; break;
            case OPT_PROXY: serve_proxy = optarg; break;
            case OPT_IMPERSONATE: serve_impersonate = optarg; break;
            case OPT_IDENTITY_RETRIES: serve_identity_retries = atoi(optarg); break;
            case OPT_RENDER: serve_render = 1; break;
            case OPT_RENDER_TIMEOUT: serve_render_timeout = atoi(optarg); break;
            case OPT_RENDER_CONCURRENT: serve_render_concurrent = atoi(optarg); break;
            case OPT_RENDER_CACHE: serve_render_cache = optarg; break;
            case OPT_RENDER_CACHE_TTL: serve_render_cache_ttl = atoi(optarg); break;
            case OPT_NO_RENDER_CACHE: serve_render_cache = ""; break;
            case OPT_INTERACTIVE: serve_interactive = 1; break;
            case OPT_INTERACTIVE_CONCURRENT: serve_interactive_concurrent = atoi(optarg); break;
            case OPT_INTERACTIVE_IDLE: serve_interactive_idle = atoi(optarg); break;
            case OPT_INTERACTIVE_KEEPALIVE: serve_interactive_keepalive = atoi(optarg); break;
            case OPT_INTERACTIVE_SILENCE: serve_interactive_silence = atoi(optarg); break;
            case OPT_RTC: serve_rtc = 1; break;
            case OPT_RTC_CONCURRENT: serve_rtc_concurrent = atoi(optarg); break;
            case OPT_RTC_FPS: serve_rtc_fps = atoi(optarg); break;
            case OPT_RTC_BITRATE: serve_rtc_bitrate = atoi(optarg); break;
            case OPT_RTC_STUN: serve_rtc_stun = optarg; break;
            default: usage(); return 2;
        }
    }
    if (optind < argc) positional = argv[optind];
    so.enabled = so.bucket != NULL;

    if (quiet) mi_log_set_level(MI_LOG_ERROR);
    else if (verbose) mi_log_set_level(MI_LOG_DEBUG);

    if (!positional && command != CMD_SERVE) {
        mi_logf(MI_LOG_ERROR, "missing required argument");
        usage();
        return 2;
    }

    int rc = 0;
    switch (command) {
        case CMD_DETECT:
            rc = cmd_detect(positional, headers, header_count);
            break;
        case CMD_DETECT_FILE:
            rc = cmd_detect_file(positional);
            break;
        case CMD_FETCH:
            rc = cmd_fetch(positional, out, name, connections, chunk_size,
                           allow_non_video, max_bytes, headers, header_count, quiet);
            break;
        case CMD_HLS:
            rc = cmd_hls(positional, out, renditions, segment_seconds);
            break;
        case CMD_INGEST:
            rc = cmd_ingest(positional, out, hls_dir, connections, chunk_size,
                            headers, header_count, quiet);
            if (rc == 0 && do_upload) {
                if (!so.enabled) {
                    mi_logf(MI_LOG_ERROR, "--upload needs --bucket (and credentials)");
                    rc = 2;
                } else {
                    const char *src = (hls_dir && *hls_dir) ? hls_dir : out;
                    char **urls = NULL;
                    size_t n = 0;
                    int urc = mi_path_is_dir(src) ? upload_tree(&so, src, &urls, &n)
                                                  : upload_one(&so, NULL, src, &urls, &n);
                    maybe_purge(&so, urls, n);
                    printf("{\n  \"uploaded\": %zu,\n  \"urls\": [", n);
                    for (size_t i = 0; i < n; i++) {
                        if (i) printf(", ");
                        json_escape_print(urls[i]);
                    }
                    printf("]\n}\n");
                    for (size_t i = 0; i < n; i++) free(urls[i]);
                    free(urls);
                    if (urc != MI_OK) rc = 1;
                }
            }
            break;
        case CMD_UPLOAD:
            rc = cmd_upload(positional, &so);
            break;
        case CMD_SERVE:
            rc = cmd_serve(serve_host, serve_port, serve_work_dir, serve_token,
                           serve_allow_origins, serve_user_agent, serve_render,
                           serve_render_timeout, serve_render_concurrent,
                           serve_render_cache, serve_render_cache_ttl,
                           serve_interactive, serve_interactive_concurrent,
                           serve_interactive_idle, serve_interactive_keepalive,
                           serve_interactive_silence, serve_rtc, serve_rtc_concurrent,
                           serve_rtc_fps, serve_rtc_bitrate, serve_rtc_stun,
                           serve_proxy, serve_impersonate, serve_identity_retries);
            break;
        default: rc = 2;
    }

    for (size_t i = 0; i < header_count; i++) free(headers[i]);
    free(headers);
    return rc;
}
