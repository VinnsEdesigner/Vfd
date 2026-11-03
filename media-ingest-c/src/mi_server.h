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
} mi_server_config;

typedef struct mi_server mi_server;

void mi_server_config_init(mi_server_config *cfg);

/* Create (does not bind). Returns NULL on allocation failure. */
mi_server *mi_server_new(const mi_server_config *cfg);
void mi_server_free(mi_server *srv);

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
