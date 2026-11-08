# HTTP and WebSocket API

Vyzor serves JSON over HTTP and two upgrades (WebSocket, WebRTC signalling).
Every route is under `/api/v1` except `/healthz`.

## Conventions

- **Auth.** When `--auth-token` (or `MI_AUTH_TOKEN`) is set, every route but
  `/healthz` requires `Authorization: Bearer <token>`. A WebSocket handshake
  cannot carry that header reliably, so `/api/v1/interactive` also accepts
  `?token=<token>`; the Origin check is what makes that safe.
- **CORS.** `--allow-origin` (`MI_ALLOW_ORIGINS`) is a comma-separated list.
  A WebSocket handshake is not subject to CORS, so Vyzor checks `Origin` itself.
- **Errors.** `{"error": "..."}` with an HTTP status. `503` means a feature is
  disabled or its pool is full; `400` means the input is wrong; `502` means an
  upstream fetch failed.
- **URL rule.** Any URL Vyzor fetches must be public `http(s)`. Loopback,
  link-local and `file://` are refused, so a request cannot reach Vyzor's own
  port or a cloud metadata endpoint.

## Health and capability

| Route | Method | Returns |
|---|---|---|
| `/healthz` | GET | `{"status":"ok"}`. No auth. |
| `/api/v1/capabilities` | GET | version, detectors, and whether ffmpeg, render, interactive, rtc, extract and gallery are available |

`/api/v1/capabilities` is how a client decides what controls to show. A disabled
or unavailable feature is reported here rather than offered and then failing.

## Detection and probing

| Route | Method | Body | Returns |
|---|---|---|---|
| `/api/v1/detect` | POST | `{url}` | `{kind, container, confidence, is_video, ...}` |
| `/api/v1/probe` | POST | `{url}` | the transfer plan: final URL, content type, size, range support, detected media |

Detection reads the bytes, never the extension or `Content-Type`.

## Downloads

| Route | Method | Body / query | Returns |
|---|---|---|---|
| `/api/v1/downloads` | POST | `{url, hls, upload, purge, connections, quality, hls_dir, prefix, allow_non_media}`, or a torrent: `{torrent_file}` (base64) and/or `{magnet_files:[1,3]}` | `202` with the new record |
| `/api/v1/downloads` | GET | — | `{items: [...]}` |
| `/api/v1/downloads/{id}` | GET | — | the record |
| `/api/v1/downloads/{id}` | PATCH | `{"action":"pause"}` or `{"action":"resume"}` or `{"action":"convert"}` | the record |
| `/api/v1/downloads/{id}` | DELETE | — | `{"ok":true}` |
| `/api/v1/downloads/{id}/content` | GET | — | the original bytes |
| `/api/v1/downloads/{id}/converted` | GET | — | the MP4 from a conversion, `404` until done |
| `/api/v1/downloads/{id}/hls/{path}` | GET | — | a transcoded HLS artifact (m3u8/ts/m4s/mp4) |

A record carries `id`, `name`, `filename`, `url`, `state`
(`queued|running|done|failed|cancelled|paused`), `progress`
(`downloaded_bytes`, `total_bytes`, `bytes_per_second`), `media` (detected kind,
container, dimensions) and `error`. `pause` keeps the partial file, so `resume`
continues from disk. `convert` remuxes to MP4 off-thread and reports
`conversion:{state,error}`.

A download is a job, not a blocking call: it returns `202` and the client polls.
This is different from a crawl or an extract, which run to completion in the
request.

A **magnet** `url` is fetched by aria2 instead of HTTP. A `torrent_file` is
base64 `.torrent` bytes; the server converts it to a magnet (a `.torrent` *is*
the metadata, so the content is identical) and downloads that. `magnet_files` is
a list of 1-based indices into the torrent's own file list — the same numbering
`torrent/inspect` reports — and is passed to aria2 as `--select-file`, so only
the chosen files are fetched.

## Resolution and browsing

| Route | Method | Body | Returns |
|---|---|---|---|
| `/api/v1/extract` | POST | `{url}` or `{search, limit, site}`, plus `timeout_seconds`, `enumerate`, `playlist_items`, `match_filter`, `date_after`, `date_before`, `extractor_args` | title, extractor, duration, thumbnail, formats, entries |
| `/api/v1/extract-rows` | POST | `{url, selector?}` | rows read from a table on a page |
| `/api/v1/torrent/inspect` | POST | `{base64}` (a `.torrent`) or `{magnet}` | name, info-hash, file list, trackers, piece size |
| `/api/v1/browse` | POST | `{url, filter}` | the media a page or directory offers (`filter: media` or `all`) |

`extract` asks yt-dlp (video), gallery-dl (images) or py1337x (torrents) —
chosen by host, or by `site` for a bare search. It never downloads. A playlist,
channel or torrent search returns `entries[]` instead of `formats[]`; a torrent
entry carries the index's own fields (`seeders`, `leechers`, `bytes`,
`size_text`, `category`, `uploaded_text`, `page_url`, `is_torrent`), and its
`url` is the magnet link.

`torrent/inspect` reads what a torrent *contains* before it is downloaded.
`complete` is the honest distinction: a `.torrent` file **is** the metadata, so
its `files[]`, `piece_length` and `trackers[]` are real and `complete` is true; a
magnet names only the info-hash and the display name, so `files[]` is empty and
`complete` is false until a client fetches the metadata from the swarm. See
[media-pipeline.md](media-pipeline.md#torrents).

## Crawl

| Route | Method | Body | Returns |
|---|---|---|---|
| `/api/v1/crawl` | POST | see below | the hits and what the walk did |

`POST /api/v1/crawl` is **blocking**: it is bounded by `budget_seconds`, so a
job-and-poll dance would add machinery for no gain.

Body: `{url, term, depth, max_pages, budget_seconds, same_host_only, use_browser,
max_browser_pages, use_extractor, click_selectors, click_consent, settle_seconds,
run_id}`.

Response: `ok`, `found`, `pages_read`, `pages_failed`, `pages_rendered`,
`skipped_offsite`, `challenges_seen`, `blocked`, `not_modified`, `duplicates`,
and `items[]` of `{url, filename, title, mime, score, found_on, via_browser,
resolved, thumbnail, qualities[]}`. The counters exist so an empty result can be
explained: "40 pages read, 12 needed the browser, none matched" is a different
answer from "the host refused us". See [crawling.md](crawling.md).

## Browser features

| Route | Method | Query | Returns |
|---|---|---|---|
| `/api/v1/render` | GET | `url`, `mode` (`image` or `html`) | a PNG, or the sanitised rendered DOM |
| `/api/v1/page` | GET | `url` | a fetched page's HTML |
| `/api/v1/interactive` | GET (upgrade) | `url`, `w`, `h`, `touch`, `token` | a WebSocket streaming frames down and input up |
| `/api/v1/rtc` | POST | `{url}` | `{token, sdp, width, height, gathered}` |
| `/api/v1/rtc/{token}/answer` | POST | `{sdp}` | `{ok}` |
| `/api/v1/rtc/{token}` | GET | — | `{state, connected, reason}` |
| `/api/v1/rtc/{token}` | DELETE | — | `{ok}` |

`mode=html` is sanitised: an allowlist of tags and attributes, no script or frame
content, escaped attribute values, and a `default-src 'none'` policy. Links are
rewritten to route back through the renderer. `X-Vyzorix-Sanitized: yes` says
whether that happened.

See [browser-features.md](browser-features.md) for the frame protocol and the
bounds.

## Settings and storage

| Route | Method | Body | Returns |
|---|---|---|---|
| `/api/v1/settings/egress` | GET | — | cookies (names only), proxy, impersonate, captcha provider, frontier |
| `/api/v1/settings/egress` | PUT | `{cookies, proxy, impersonate, identity_retries, captcha_*}` | the new settings |
| `/api/v1/storage/config` | GET | — | endpoint, bucket, configured |
| `/api/v1/storage/location` | PUT | `{endpoint, bucket, ...}` | the new location |
| `/api/v1/system/storage` | GET | — | work dir, writability, filesystem bytes, library and download counts |
| `/api/v1/captcha/solve` | POST | `{...}` | a solved token |

Egress settings live in memory for the session. Credentials travel as request
headers on a download; they are never written to disk or logged.

## The interactive WebSocket

After the upgrade, Vyzor sends text frames and binary frames. A text message
introduces each image, then the image arrives as a binary frame:

```
{"type":"ready","url":…,"width":…,"height":…}            once, on connect
{"type":"state","page":{"url":…,"title":…,"loading":…}}  when the page changes
{"type":"frame","format":"jpeg","width":…,"seq":N}       then one binary frame
{"type":"shot", …}                                        then one binary frame
{"type":"closed","reason":…}                             when the session ends
```

The client sends JSON commands the other way: `navigate`, `reload`, `resize`,
`screenshot`, and `mouse`/`wheel`/`key`/`text`/`touch`. Vyzor acknowledges each
screencast frame itself, so the client does not have to.
