/* mi_download.h - fast, resumable, concurrent download Vyzor (libcurl). */
#ifndef MI_DOWNLOAD_H
#define MI_DOWNLOAD_H

#include <stddef.h>
#include <stdint.h>

#include "mi_detect.h"
#include "mi_antibot.h"

typedef struct {
    char *url;
    int64_t total_bytes; /* -1 when unknown */
    int accept_ranges;
    char *content_type;
    char *final_url;
    mi_media_info media;
    int have_media;
    /* What the probe saw, so a refusal can be named rather than guessed at. */
    int status;
    mi_block_kind block;
} mi_download_plan;

typedef struct {
    int64_t downloaded_bytes;
    int64_t total_bytes; /* -1 when unknown */
    double started_at;
    int finished;
} mi_progress;

typedef void (*mi_progress_fn)(const mi_progress *p, void *user);

/* Cooperative control for a running download. A worker polls `stop()` between
 * chunks; when it reports true the transfer is abandoned. The partial data is
 * left on disk and the state file records it, so the same call resumes later —
 * that is what makes pause/resume cheap rather than a restart.
 *
 * Both callbacks are invoked from worker threads and must be thread-safe. */
typedef struct {
    int (*stop)(void *user);
    void *user;
} mi_download_control;

typedef struct {
    int connections;
    int64_t chunk_size;
    int retries;
    double backoff;
    long timeout_seconds;
    /* Refuse payloads the detector does not recognise as video, image or
     * audio. Named for what it means now that all three are supported. */
    int require_media;
    int64_t max_bytes; /* 0 == unlimited */
    char **headers;    /* extra "Key: Value" headers */
    size_t header_count;
    /*
     * The User-Agent to send. NULL uses the built-in default.
     *
     * Worth being able to change: some hosts refuse a request that does not
     * look like a browser, and a caller fetching a page on the user's behalf
     * may need to say so. It is a request header like any other, so it travels
     * in the same list the extra headers do.
     */
    const char *user_agent;
    /*
     * The `Accept-Language` header value, e.g. "en,fr;q=0.9". NULL sends none.
     *
     * A site that negotiates serves the language the caller asks for; without
     * this it serves its default, which may be one the crawl would then drop.
     */
    const char *accept_language;
    /* Optional: lets a caller stop a running download (pause/cancel). */
    const mi_download_control *control;
    /*
     * The ffmpeg binary, for a manifest.
     *
     * An HLS or DASH URL is a playlist, not a file, so it is read by ffmpeg —
     * which fetches every segment and muxes them. NULL uses "ffmpeg" from PATH.
     */
    const char *ffmpeg_path;
    /*
     * Whether to ask the extractor when the URL is a *page* rather than a file.
     *
     * A streaming site's media is not at the page URL, so an ordinary transfer
     * of it fetches HTML. With this set, a URL that probes as a non-video
     * document is offered to the extractor first, and its streams are muxed.
     * Off by default so a plain file download never pays for a subprocess.
     */
    int extract_pages;
    /* The extractor binary; NULL uses "yt-dlp" from PATH. */
    const char *extractor_path;
    /* The gallery extractor's binary; NULL uses "gallery-dl" from PATH. */
    const char *gallery_extractor_path;
    /* The torrent extractor's binary; NULL uses "py1337x" from PATH. */
    const char *torrent_extractor_path;
    /* A BitTorrent index to search, e.g. "1337x"; NULL uses the default. */
    const char *torrent_site;
    /*
     * The BitTorrent downloader binary, for a magnet link; NULL uses "aria2c"
     * from PATH.
     *
     * A magnet is not a file and not a manifest: it names a swarm, and the bytes
     * arrive from peers. No HTTP client can fetch it, so it is handed to a
     * BitTorrent client — the same division of labour as ffmpeg for a manifest
     * and yt-dlp for a page. aria2c is the one that is a single static binary
     * with a magnet on its command line, so it is the default.
     */
    const char *magnet_downloader_path;
    /*
     * The raw bytes of a `.torrent`, when the caller opened one.
     *
     * This is handed to the BitTorrent client as a *file*, not converted to a
     * magnet — because a `.torrent` carries **webseeds** (`url-list`) and the
     * piece hashes, and a magnet carries neither. Measured: an archive.org
     * torrent has no peers at all and downloads only from its HTTP webseed, so
     * converting it to a magnet produced zero bytes while the `.torrent` itself
     * fetched the whole thing. The magnet is still built for display and for the
     * record, but the client gets the file.
     *
     * NULL when the caller gave a magnet instead.
     */
    const unsigned char *torrent_bytes;
    size_t torrent_len;
    /*
     * Seconds a torrent may take, which is not the HTTP timeout.
     *
     * A torrent is a swarm: it may spend a minute finding peers before any byte
     * arrives, and a large one takes many minutes. Reusing the 30-second HTTP
     * timeout — which is right for a single request — made every torrent fail at
     * 30 seconds. 0 uses the default (15 minutes).
     */
    double torrent_timeout_seconds;
    /*
     * Which files of a torrent to fetch, 1-based, or NULL for all of them.
     *
     * A torrent is often a *set*: a film and its subtitles, or several
     * renditions. Fetching all of it to get one file wastes the difference, so
     * a caller that knows the file list (from a `.torrent`, whose metadata is
     * the file itself) can name the ones it wants and the client takes only
     * those. Indices are aria2's own numbering, which is 1-based and matches the
     * order in the torrent's `files` list.
     *
     * NULL or a count of 0 means the whole torrent.
     */
    const int *magnet_files;
    size_t magnet_file_count;
    /*
     * Cookies for a site that refuses an anonymous request.
     *
     * Netscape format, as a browser export gives. Two consumers need them and in
     * two different shapes: the extractor takes the file, and ffmpeg — which does
     * the actual fetch — takes a `Set-Cookie` string. The conversion is the
     * extractor module's job; this carries the input.
     *
     * NULL sends none. A credential, so it is never logged.
     */
    const char *cookies_text;
    /*
     * A browser whose own cookie store the extractor should read, e.g. "chrome"
     * or "firefox" (yt-dlp's `--cookies-from-browser`). NULL or "" is off.
     *
     * The alternative to an export the operator makes by hand: on a host where
     * they are already signed in to the site in a browser, the session is read
     * directly. Only the extractor can do this — ffmpeg, which fetches the
     * segments, cannot — so a caller that relies on it is relying on the
     * extractor's own downloader, not on the mux.
     */
    const char *cookies_from_browser;
    /*
     * A live cookie jar, or NULL.
     *
     * When set, the request's `Cookie` header is built from the jar and every
     * `Set-Cookie` the response carries is stored back. This is what makes a
     * *session* work: a login sets a cookie, the next request must send it, and
     * the site rotates it as it goes. The static `cookies_text` above is the
     * one-shot case; this is the live one. Both may be set.
     */
    struct mi_cookiejar *jar;
    /*
     * A proxy every outbound request goes through, e.g. "http://host:port" or
     * "socks5://host:port".
     *
     * A site may refuse the host's own address, and a proxy is the way out. It
     * reaches curl for the ordinary fetch and ffmpeg for a manifest, so both
     * the transfer and the resolve behind it use the same egress. NULL is
     * direct.
     */
    const char *proxy;
    /*
     * A yt-dlp / curl_cffi impersonation target for the resolve, e.g. "chrome".
     *
     * A site that fingerprints TLS sees libcurl and blocks it; naming a real
     * browser makes the resolve look like one. NULL is off. The download's own
     * fetch is libcurl and cannot impersonate, so this only reaches the
     * extractor.
     */
    const char *impersonate;
    /*
     * Conditional-GET validators: what the last fetch of this URL returned.
     *
     * Sending these back is how an incremental crawl avoids re-downloading an
     * unchanged page — the server answers 304 and the body is not transferred.
     * NULL sends no condition, which fetches unconditionally.
     */
    const char *if_none_match;     /* an ETag */
    const char *if_modified_since; /* an HTTP date */
    /*
     * The anti-bot brain, shared across downloads so a host is solved once.
     *
     * NULL creates a temporary one for this call, which is fine for a single
     * download but forgets everything after it. A long-running Vyzor passes one
     * brain so the identity that worked for a host is reused rather than
     * rediscovered on every request — the difference between rotating and
     * sticking.
     */
    mi_antibot *brain;
    /*
     * Whether a refused download is retried with a different identity.
     *
     * A refusal is often of the *identity* — a bot User-Agent, a datacenter
     * address — so retrying the same way retries the same refusal. With this
     * set, each retry gets a browser User-Agent and, when more than one proxy is
     * configured, a different proxy. 0 retries nothing (the old behaviour); N
     * allows up to N fresh-identity retries.
     */
    int identity_retries;
    /*
     * Whether the identity actually changes between retries. Off pins every
     * attempt to the operator's own configuration, which is right when they
     * meant that exact identity.
     */
    int rotate_identity;
    /*
     * The height to prefer when resolving a page, e.g. 720.
     *
     * A stream offers many renditions and the choice is the viewer's: a phone on
     * mobile data does not want the 4K master. 0 takes the largest, which is the
     * old behaviour and the right default when nobody said.
     *
     * A request that cannot be met exactly takes the *nearest at or below* it,
     * because a viewer asking for 720 is asking to avoid the larger file; giving
     * them 1080 would defeat the point.
     */
    int preferred_height;
} mi_download_config;

typedef struct {
    char *url;
    char *path;
    int64_t bytes_written;
    mi_media_info media;
    int resumed;
    double elapsed_seconds;
    /* The last HTTP status seen, and what it was classified as. */
    int status;
    mi_block_kind block;
} mi_download_result;

void mi_download_config_init(mi_download_config *cfg);

/* Fetch up to `max_bytes` of a URL as NUL-terminated text (for manifests).
 * Returns a malloc'd string, or NULL. Caller frees. */
char *mi_download_fetch_text(const mi_download_config *cfg, const char *url,
                             size_t max_bytes);

/*
 * What a fetch learned about the response, beyond its bytes.
 *
 * `framable` answers the one question that decides whether a page can be shown
 * inside the console: whether the site permits being embedded. `X-Frame-Options`
 * and a CSP `frame-ancestors` directive are the two ways a site says no, and
 * both are read here — a browser would enforce them, and reporting the refusal
 * up front is more useful than an embed that silently stays blank.
 */
typedef struct {
    char *final_url;     /* after any redirects; NULL when unchanged */
    char *content_type;  /* as sent; NULL when absent */
    int framable;        /* 1 unless the response forbids framing */
    char *frame_blocker; /* the header that forbade it, when one did */
    /*
     * The validators the response carried, so a caller can send them back on
     * the next fetch and be told 304 instead of the whole page.
     */
    char *etag;
    char *last_modified;
    /* The encoding the body was decoded from, for reporting. NULL when the
     * fetch did not decode (a non-document fetch). */
    char *encoding;
    /* 1 when the server answered 304 Not Modified: the body is empty and the
     * caller's cached copy is still current. */
    int not_modified;
    /*
     * The `Location` header, when the response was a redirect that was not
     * followed. A POST does not follow its own redirect (the cookie it sets must
     * reach the jar first), so the caller needs the target to follow itself.
     */
    char *location;
    /* The HTTP status of the response, for a caller that judges success from it
     * (a login, a link check). 0 when the fetch failed before a response. */
    int status;
} mi_fetch_info;

void mi_fetch_info_free(mi_fetch_info *info);

/*
 * POST `body` to `url` with `content_type`, and return the response body.
 *
 * This is what a login (or any form submission) needs: the credentials go in the
 * body, and the response's `Set-Cookie` is stored in the jar. A redirect is
 * followed, so a site that answers a successful login with a 302 to the account
 * page ends on the account page.
 */
char *mi_download_post(const mi_download_config *cfg, const char *url,
                       const char *body, const char *content_type,
                       size_t max_bytes, mi_fetch_info *info);

/* Like mi_download_fetch_text, but also reports what the response said. */
char *mi_download_fetch_document(const mi_download_config *cfg, const char *url,
                                 size_t max_bytes, mi_fetch_info *info);

/* Probe a URL without downloading the whole object. Returns 0/-1. */
int mi_probe(const mi_download_config *cfg, const char *url,
             mi_download_plan *out);
void mi_download_plan_free(mi_download_plan *plan);

/* Download `url` to `dest`, resuming from a prior partial if present.
 * `on_progress` (may be NULL) is invoked from worker threads and must be
 * thread-safe. Returns 0 or a negative MI_ERR_*. */
int mi_download(const mi_download_config *cfg, const char *url, const char *dest,
                mi_progress_fn on_progress, void *user,
                mi_download_result *out);
void mi_download_result_free(mi_download_result *res);

#endif /* MI_DOWNLOAD_H */
