/* mi_server.h - HTTP API server for the ingest pipeline.
 *
 * A small threaded HTTP/1.1 server exposing a JSON REST API over the ingest,
 * transcode and storage modules. Endpoints:
 *
 *   GET    /healthz
 *   GET    /api/v1/capabilities
 *   POST   /api/v1/probe              {url}
 *   POST   /api/v1/downloads               {url, hls?, upload?, ...}
 *   GET    /api/v1/downloads
 *   GET    /api/v1/downloads/{id}
 *   PATCH  /api/v1/downloads/{id}       {name?, move_to?}
 *   DELETE /api/v1/downloads/{id}
 *   POST   /api/v1/detect             (multipart or octet-stream sniff)
 *   GET    /api/v1/system/storage      (work dir, volume, library figures)
 *   PUT    /api/v1/storage/location    {dir} - set the download directory
 *   GET    /api/v1/storage/config
 */
#ifndef MI_SERVER_H
#define MI_SERVER_H

#include <stddef.h>
#include <stdint.h>

#include "mi_storage.h"

typedef struct {
    const char *host;      /* bind address, default 127.0.0.1 */
    int port;              /* default 8787 */
    int workers;           /* max concurrent connections */
    int max_body_bytes;    /* request body cap, default 64 MiB */
    const char *work_dir;  /* where downloads write artifacts */
    /*
     * Which browser origins may call this API.
     *
     * `allow_origins` is a comma-separated list, or "*" for any. It is checked
     * against the request's Origin header and echoed back verbatim, because a
     * browser refuses `Access-Control-Allow-Origin: *` on a credentialed
     * request. A request from an origin not on the list gets no CORS headers at
     * all, so the browser blocks the response — which is the point: without this
     * any website a user visits could read their library and start downloads.
     */
    const char *allow_origins;
    const char *auth_token;/* optional Bearer token; NULL disables auth */
    int max_downloads;          /* cap on retained download records */
    /*
     * The User-Agent Vyzor sends when it fetches on the caller's behalf —
     * a download, a probe, or a page read for Browse.
     *
     * Some hosts refuse a request that does not look like a browser, so a
     * deployment can set this without a rebuild. NULL keeps the built-in
     * default, which identifies Vyzor honestly.
     */
    const char *user_agent;
    /*
     * Rendering a page with headless Chromium.
     *
     * `render_enabled` off means the endpoint answers 503 and the capability is
     * reported false, which is what a host without a browser should do rather
     * than accepting a request it cannot serve. The rest tune the renderer; 0
     * means "use the built-in default" in each case.
     */
    int render_enabled;
    int render_timeout_seconds;   /* hard wall-clock limit per render */
    int render_max_concurrent;    /* renders at once; each costs ~500 MB */
    int render_v8_heap_mb;        /* per-render V8 heap cap */
    int render_viewport_width;
    int render_viewport_height;
    const char *render_browser;   /* explicit Chromium path, else searched */
    const char *render_profile_root; /* where throwaway profiles are made */
    /*
     * A profile directory the live browser keeps between sessions, so a sign-in
     * done once in the live view survives — and is on disk for the extractor to
     * read as a cookie source. NULL is a throwaway profile per session.
     */
    const char *persistent_profile;
    /*
     * Rendered-page cache. NULL `render_cache_dir` turns it off; the budget and
     * TTL bound it, and 0 means the built-in default in each case. It is on by
     * default, under the work dir, because a render is expensive enough that
     * repeating one is worth avoiding.
     */
    const char *render_cache_dir;
    size_t render_cache_max_bytes;
    size_t render_cache_max_entries;
    int render_cache_ttl_seconds;
    /*
     * Live, driveable pages (Chromium over CDP).
     *
     * Distinct from rendering: a render draws a page once, a live session keeps
     * a browser alive so the page can be used. It costs a whole browser per
     * session, so `interactive_enabled` is its own opt-in and the concurrency is
     * its own gate. `interactive_idle_seconds` closes a session nobody is
     * watching, because an abandoned browser is a leak the size of a browser.
     */
    int interactive_enabled;
    int interactive_max_concurrent;
    int interactive_idle_seconds;
    int interactive_v8_heap_mb;
    /* Keepalive for a live session, in seconds. 0 uses the built-in default. */
    int interactive_keepalive_ping_seconds;
    int interactive_keepalive_timeout_seconds;
    /*
     * Live video over WebRTC (raw capture rather than JPEG frames).
     *
     * The most demanding of the three page features: a session holds a browser
     * *and* an X server, and the frames are encoded. It needs Xvfb and the
     * GStreamer webrtc plugins, so it is its own opt-in. 0 means the built-in
     * default for each numeric field.
     */
    int rtc_enabled;
    int rtc_max_concurrent;
    int rtc_idle_seconds;
    int rtc_v8_heap_mb;
    int rtc_width;
    int rtc_height;
    int rtc_fps;
    int rtc_bitrate_kbps;
    const char *rtc_stun_server; /* e.g. stun://host:19302; NULL = host only */
    /*
     * The media extractor, for a site whose stream URL is not in the page.
     *
     * A streaming site computes its media URL in JavaScript at play time and
     * signs it, so it is in neither the HTML nor the browser's traffic. NULL
     * means "yt-dlp" from PATH; an operator may point at their own build, since
     * this is the one component whose job is keeping up with those sites.
     */
    const char *extractor_path;
    /*
     * The gallery extractor's binary (gallery-dl).
     *
     * A second tool for a second question: yt-dlp resolves video streams,
     * gallery-dl lists the images on a page. NULL means "gallery-dl" from PATH;
     * when it is absent the console offers no gallery mode rather than one that
     * cannot work.
     */
    const char *gallery_extractor_path;
    /*
     * The torrent extractor's binary (py1337x).
     *
     * A third tool for a third question: yt-dlp resolves video streams,
     * gallery-dl lists the images on a page, and py1337x resolves a torrent index
     * to magnet links. NULL means "py1337x" from PATH; when it is absent the
     * console offers no torrent mode rather than one that cannot work.
     */
    const char *torrent_extractor_path;
    /*
     * A BitTorrent index to search, e.g. "1337x", or NULL for the extractor's
     * default. A torrent index is a search box, so a search of it needs to name
     * which one.
     */
    const char *torrent_site;
    /*
     * The BitTorrent downloader binary (aria2c) for a magnet link.
     *
     * A magnet is fetched from a swarm, which no HTTP client can do; NULL means
     * "aria2c" from PATH. When it is absent a magnet download fails with a clear
     * message rather than a network error from a scheme libcurl cannot open.
     */
    const char *magnet_downloader_path;
    /*
     * Cookies for a site that refuses an anonymous request.
     *
     * Netscape format. Set from the settings endpoint or MI_COOKIES_FILE, and
     * never logged: it is a credential for whatever account the operator signed
     * in with, and a site that demands a sign-in is one they have an account on.
     *
     * It is a *setting* rather than a per-request field because it is a
     * credential — one per Vyzor, chosen by the operator, not something a
     * download request should be able to change.
     */
    const char *cookies_text;
    /*
     * A browser whose own cookie store the extractor should read, e.g. "chrome"
     * or "firefox" (yt-dlp's `--cookies-from-browser`). NULL or "" is off.
     *
     * The alternative to an export the operator makes by hand: on a host where
     * they are already signed in, the session is read straight from the browser.
     * Only the extractor can do this, so it reaches the resolve and the
     * extractor's own downloader, not ffmpeg.
     */
    const char *cookies_from_browser;
    /*
     * A proxy every outbound request goes through, e.g. "http://host:port" or
     * "socks5://host:port".
     *
     * A site may refuse the host's own address — a datacenter range, or a
     * country — and a proxy is the way out. One setting, applied everywhere a
     * request leaves the process: the crawler's page fetches, the extractor,
     * ffmpeg's transfer and the browser. Set from the settings endpoint or
     * MI_PROXY. NULL is direct.
     */
    const char *proxy;
    /*
     * A yt-dlp / curl_cffi impersonation target, e.g. "chrome".
     *
     * A site that fingerprints the TLS handshake sees libcurl and refuses it.
     * Naming a real browser makes the extractor's request look like one. Set
     * from the settings endpoint or MI_IMPERSONATE. NULL is off.
     */
    const char *impersonate;
    /*
     * How many times a refused download is retried with a *different* identity.
     *
     * A refusal is often of the identity, so the retry presents a browser
     * User-Agent and, when more than one proxy is configured, a different proxy.
     * 0 retries nothing (the old behaviour).
     */
    int identity_retries;
    /*
     * Whether the identity changes between retries. Off pins every attempt to
     * the operator's own configuration.
     */
    int rotate_identity;
    /*
     * The captcha solver, for a page behind a challenge.
     *
     * There is no library that solves a captcha, so this is a seam: the provider
     * is named and a key supplied, and the solver is asked through it. NULL or
     * "stub" uses the built-in stub, which contacts nothing. Set from the
     * settings endpoint or MI_CAPTCHA_PROVIDER / MI_CAPTCHA_KEY. The key is
     * never returned by the API.
     */
    const char *captcha_provider;
    const char *captcha_api_key;
    /* An endpoint override for the captcha provider (a gateway or a test
     * server). NULL uses the provider's own. */
    const char *captcha_base_url;
    /*
     * Per-site extractor arguments applied to *every* resolve and download, e.g.
     * `youtube:player_client=visionos`.
     *
     * This is the operator-level half of the same seam the per-request
     * `extractor_args` uses, and it is where a YouTube PO-token provider is
     * configured: yt-dlp takes `youtube:po_token=...` (or a `getpot_bgutil` /
     * `bgutil:...` provider) through `--extractor-args`, so naming it here wires
     * it into every YouTube job without a code change.
     *
     * It exists because the player client and the token are a *policy* decision
     * about the host's egress, not a property of one download — the same reason
     * `impersonate` and the captcha provider are settings. Set from the settings
     * endpoint or MI_EXTRACTOR_ARGS (comma-separated).
     */
    const char *extractor_args;
    /*
     * A shared crawl frontier, so several Vyzors walk one site once rather than
     * once each. When both are set, a crawl request's `run_id` puts its queue in
     * this Turso database. The token is a credential and is never returned.
     */
    const char *frontier_db_url;
    const char *frontier_token;
    /*
     * A local SQLite file for the crawl frontier, so a single server is durable
     * and resumable with no external database. Used when a run id is given and
     * no remote url is set. Set from the settings endpoint or MI_FRONTIER_DB.
     */
    const char *frontier_db_path;
    /*
     * Whether to share one anti-bot brain across crawls and downloads.
     *
     * On by default: a host solved once stays solved. Off gives each operation a
     * fresh brain, which is only useful for reproducing a first-contact block.
     */
    int share_brain;
    /*
     * Whether a crawl honours robots.txt. On by default: that is what a crawler
     * is. A user's explicit fetch of one URL is not a crawl and is not subject
     * to it.
     */
    int obey_robots;
    /*
     * The default minimum seconds between requests to one host, when the host
     * published no Crawl-delay. 0 disables pacing.
     */
    double crawl_interval_seconds;
    /*
     * The ffmpeg binary.
     *
     * Used to read a manifest (an HLS or DASH URL is a playlist, not a file) and
     * to transcode. NULL means "ffmpeg" from PATH; an operator may point at
     * their own build.
     */
    const char *ffmpeg_path;
} mi_server_config;

typedef struct mi_server mi_server;

void mi_server_config_init(mi_server_config *cfg);

/* Create (does not bind). Returns NULL on allocation failure. */
mi_server *mi_server_new(const mi_server_config *cfg);
void mi_server_free(mi_server *srv);

/*
 * Whether this server can render pages, i.e. rendering is on and a browser was
 * found. Exposed so a caller — a test, or a startup check — can ask without
 * knowing the renderer's internals.
 */
int mi_server_can_render(const mi_server *srv);

/* The same question for live, driveable sessions. */
int mi_server_can_drive(const mi_server *srv);

/* Bind + listen. Returns 0 or a negative MI_ERR_*. */
int mi_server_listen(mi_server *srv);

/* Actual bound port (useful when port 0 was requested). */
int mi_server_port(const mi_server *srv);

/* Serve until mi_server_stop() is called. Blocking. */
int mi_server_run(mi_server *srv);

/* Ask a running server to stop. Async-signal-safe. */
void mi_server_stop(mi_server *srv);

/* Convenience: listen + run. */
int mi_server_start(mi_server *srv);

#endif /* MI_SERVER_H */
