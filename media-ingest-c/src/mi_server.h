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
    /*
     * A shared crawl frontier, so several Vyzors walk one site once rather than
     * once each. When both are set, a crawl request's `run_id` puts its queue in
     * this Turso database. The token is a credential and is never returned.
     */
    const char *frontier_db_url;
    const char *frontier_token;
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
