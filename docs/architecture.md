# Architecture

## Two pieces

| Piece | Language | Runs where | Talks to |
|---|---|---|---|
| Vyzor (`media-ingest-c/`) | C11 | anywhere with a disk and a browser | the internet |
| Console (`web/`) | React + TypeScript | a static host or CDN | a Vyzor over HTTP/WS |

The split is deliberate. The console is a static bundle, so it can sit on a CDN.
Vyzor needs a real disk and a real browser, so it runs wherever there is a volume.
The console finds Vyzor through `VITE_API_BASE_URL`, baked at build time, or set
in **Settings → Connection** at runtime.

Vyzor is also a CLI and a library; the console is one client of it, not the only
one.

## Vyzor's module map

Every module is prefixed `mi_`. The groups:

| Group | Modules | Job |
|---|---|---|
| Core | `mi_util`, `mi_json` | strings, logging, SHA-256/HMAC/base64, JSON parse and emit |
| Detection | `mi_detect`, `mi_identify` | what a byte stream is, from its container structure |
| Transfer | `mi_download`, `mi_hls`, `mi_storage` | ranged resumable fetch, FFmpeg to HLS, S3 upload + CDN purge |
| Browser | `mi_render`, `mi_interactive`, `mi_rtc`, `mi_ws`, `mi_cache`, `mi_sanitize` | render a page, drive a page, stream a page, cache, sanitise |
| Crawl | `mi_crawl`, `mi_frontier`, `mi_sitemap`, `mi_browse`, `mi_linkcheck` | walk a site, share a queue, seed from sitemaps, list a page |
| HTML | `mi_html`, `mi_html5`, `mi_css`, `mi_xpath`, `mi_charset`, `mi_structured` | an HTML5 tree builder, selectors, charset, structured data |
| Site identity | `mi_identity`, `mi_impersonate`, `mi_antibot`, `mi_captcha`, `mi_captcha_inject`, `mi_browser_headers`, `mi_fingerprint`, `mi_cookiejar`, `mi_form`, `mi_session`, `mi_politeness` | look like a real browser, hold a session, obey a host's rules |
| Extraction | `mi_extract`, `mi_extract_rows`, `mi_search`, `mi_lang`, `mi_mediawatch`, `mi_outbox` | resolve a stream with yt-dlp/gallery-dl, read a table, search, language |
| Server | `mi_server`, `mi_cli`, `mi_turso` | HTTP/WS server, CLI front end, Turso client |

`mi_ws` is a WebSocket *server* (RFC 6455 framing). Vyzor has no WebSocket
client; it talks to Chromium over a pipe instead.

## The request path

An HTTP request enters `mi_server.c`:

1. `accept()` on the listening socket, one thread per connection.
2. Route match by path and method (see [api.md](api.md)).
3. Work is done, then a JSON body is written.

Routes fall into three shapes:

- **Blocking**: a crawl or an extract runs to completion in the request and
  returns its result. Both are bounded by a budget, so they cannot run forever.
- **Job + poll**: a download returns `202` with an id and runs on its own thread;
  the client polls `GET /api/v1/downloads/{id}`.
- **Upgraded**: `/api/v1/interactive` is a WebSocket; `/api/v1/rtc` is signalling
  for a WebRTC connection.

Shared state (the download table, the render pool, the crawl frontier) is behind
one mutex, `srv->lock`. The render and interactive pools have their own gates so
a slow page cannot take the whole server.

## Concurrency model

- One thread per HTTP connection.
- Downloads, renders, interactive sessions and RTC sessions each run off the
  request thread.
- Chromium is spawned as a child; Vyzor is a child subreaper so orphaned browser
  helpers are reaped rather than accumulating as zombies.
- A render or a live session holds a slot from a bounded pool. A full pool is
  refused with `503`, not queued, because a queue behind a slow page grows
  without bound.

## What the console is not allowed to do

The console's layers are enforced by lint, not convention:

```
ui -> hooks -> rest -> domain
```

`ui` may import only `ui`, `hooks` and `stores`. A server change touches
`domain/normalize.ts` and `rest/`, never a component. The wire shape is
snake_case and lives only in `web/src/domain/wire.ts`; `normalize.ts` is the
single bridge between it and the app's own types.
