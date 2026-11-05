# media-ingest-c

A fast, resumable media downloader with **content-based video detection** and an
**FFmpeg → HLS** transcoding pipeline. Written in C11.

Built for ingesting media you own or are licensed to distribute.

## Components

| File | Purpose |
|---|---|
| `src/mi_detect.[ch]` | Content-based media detection (magic bytes / container structure) |
| `src/mi_download.[ch]` | Concurrent, resumable ranged downloader (libcurl + pthreads) |
| `src/mi_hls.[ch]` | FFmpeg → HLS adaptive-bitrate transcode pipeline |
| `src/mi_storage.[ch]` | S3-compatible upload (AWS SigV4) + Cloudflare cache purge |
| `src/mi_util.[ch]` | Helpers, logging, SHA-256/HMAC-SHA256/SHA-1, base64, SigV4 timestamps |
| `src/mi_render.[ch]` | Headless-Chromium page renderer (picture or sanitised DOM) |
| `src/mi_cache.[ch]` | Bounded LRU disk cache for rendered pages |
| `src/mi_sanitize.[ch]` | HTML allowlist sanitiser for rendered DOM |
| `src/mi_ws.[ch]` | WebSocket framing (RFC 6455), for live page sessions |
| `src/mi_interactive.[ch]` | Live, driveable page sessions over the Chromium CDP pipe |
| `src/mi_rtc.[ch]` | Live video over WebRTC: raw capture from an X display, input on a data channel |
| `src/mi_cli.c` | Command-line front end |

## Build

Requires a C11 compiler, libcurl development headers, and pthreads.

```bash
# Debian/Ubuntu
sudo apt-get install -y build-essential libcurl4-openssl-dev ffmpeg

make            # builds build/vyzor
make test       # builds + runs the test suite
```

## CLI

```bash
# Identify a URL without downloading it
vyzor detect https://cdn.example.com/clip.mp4

# Sniff a local file
vyzor detect-file ./mystery.bin

# Download (8 connections, resumable)
vyzor fetch https://cdn.example.com/clip.mp4 -o downloads

# Transcode to an HLS ladder
vyzor hls ./clip.mp4 -o ./hls --renditions 1080,720,480 --segment-seconds 6

# End to end: download -> detect -> HLS
vyzor ingest https://cdn.example.com/clip.mp4 -o ./out --hls ./out/hls

# Upload a file or a whole directory tree to S3-compatible storage
vyzor upload ./out/hls --bucket my-media --prefix hls/clip-42/ \
  --endpoint https://<account>.r2.cloudflarestorage.com --region auto \
  --access-key "$S3_KEY" --secret-key "$S3_SECRET" \
  --cdn-base https://cdn.example.com

# Ingest + HLS + upload + CDN purge in one shot
vyzor ingest https://cdn.example.com/clip.mp4 -o ./out --hls ./out/hls --upload \
  --bucket my-media --prefix hls/clip-42/ --cdn-base https://cdn.example.com \
  --purge --cf-token "$CF_TOKEN" --cf-zone "$CF_ZONE"
```

Storage options: `--endpoint`, `--region`, `--bucket`, `--access-key`,
`--secret-key`, `--session-token`, `--prefix`, `--content-type`,
`--path-style` / `--virtual-host`, `--cdn-base`, `--purge`, `--cf-token`,
`--cf-zone`. Credentials can also come from `MI_S3_ACCESS_KEY`,
`MI_S3_SECRET_KEY`, `MI_S3_SESSION_TOKEN`, `MI_S3_ENDPOINT`, `MI_S3_REGION`,
`MI_S3_BUCKET`, `MI_CF_API_TOKEN`, `MI_CF_ZONE_ID`.

Useful flags: `-c N` connections, `--chunk-size`, `--max-bytes`, `--allow-non-video`,
`-H "Authorization: Bearer …"`, `-q` / `-v`.

## Library

```c
#include "mi_download.h"
#include "mi_hls.h"

mi_download_config cfg;
mi_download_config_init(&cfg);
cfg.connections = 8;

mi_download_result res;
if (mi_download(&cfg, url, "movie.mp4", NULL, NULL, &res) == MI_OK) {
    /* res.media.kind tells you what it actually was */
    mi_download_result_free(&res);
}

mi_hls_config hcfg;
mi_hls_config_init(&hcfg);
mi_hls_result hres;
if (mi_hls_transcode(&hcfg, "movie.mp4", "hls", &hres) == MI_OK) {
    /* hres.master_playlist -> "hls/master.m3u8" */
    mi_hls_result_free(&hres);
}
```

## Detection

Detection reads the **bytes**, never the extension or `Content-Type`. Recognised:
MP4/MOV/3GP (ISO-BMFF), Matroska/WebM, AVI, FLV, Ogg, ASF/WMV, MPEG-TS,
MPEG-PS, plus HLS (`.m3u8`) and DASH (`.mpd`) manifests — from which the
referenced segment URLs are extracted. Non-video payloads are refused by
default, *before* the download starts.

## Downloader

- **Ranged parallelism** — `Accept-Ranges: bytes` → N ranges on threads, then stitched.
- **Resumable** — each range lands in `<dest>.parts/` with a `state.txt` sidecar;
  re-running the same destination continues instead of restarting.
- **Retries** with exponential backoff, per range.
- **Streaming fallback** for servers without range support.
- **SHA-256** of the finished file via `mi_sha256_file()`.

## HLS pipeline

Transcodes to an adaptive-bitrate ladder (default 1080/720/480/360), skipping
rungs above the source height so it never upscales. Emits per-rendition
`index.m3u8` + `.ts` segments and a `master.m3u8`. x264, `main` profile,
`yuv420p` (HLS-compatible), configurable GOP/segment length.

## Object storage + CDN

`mi_storage` implements the S3 REST API with **AWS Signature Version 4**, so it
works against AWS S3, Cloudflare R2, MinIO, Backblaze B2, DigitalOcean Spaces,
and Wasabi. Highlights:

- Content-Type is inferred from the object key (`.m3u8` →
  `application/vnd.apple.mpegurl`, `.ts` → `video/mp2t`, …), overridable.
- `Host` is signed **with its port** and pinned on the request, so signatures
  stay valid against endpoints on non-default ports (MinIO `:9000`, R2, local
  mocks). This was a real bug caught by the test suite.
- Optional Cloudflare zone cache purge (`purge_cache`) after upload.

## Page rendering

`serve --render` turns on a second job: drawing a page with a real browser.

A page is not its bytes. A site that builds itself with JavaScript arrives as a
document whose script must run before there is anything to show, and a site that
forbids being framed cannot be embedded at all. So Vyzor runs headless
Chromium and returns either a PNG of the page or its rendered DOM:

```
GET /api/v1/render?url=https://example.com/&mode=image   # a picture
GET /api/v1/render?url=https://example.com/&mode=html    # the rendered DOM
```

`mode=html` is sanitised before it is served — an allowlist of tags and
attributes, no script or frame content, escaped attribute values, and a strict
`default-src 'none'` policy injected. Links are rewritten to route back through
the renderer so following one stays in the app. `X-Vyzorix-Sanitized: yes` says
whether that happened.

### What it costs, and what it needs

A render is a browser, so it is not cheap:

| | |
|---|---|
| Peak memory | ~500 MB (proportional set size; measured on example.com, Google and Wikipedia) |
| Time | ~1–1.5 s for a typical page |
| Concurrency | 1 by default. Two renders need roughly twice the memory. |

**It needs at least 2 GB of RAM.** On a 512 MB instance it will be killed
mid-render. `--render-concurrent` is the knob that decides the requirement.

It also needs a browser and fonts. Without Chromium the renderer reports itself
unavailable and `/api/v1/capabilities` says so, so a console can hide the control
rather than offering one that fails. Without fonts, pages render as boxes —
which is why the image installs them.

### Bounds, and what is not one

A render is a **resource-bounded subprocess, not a security sandbox**. An
unprivileged container cannot use Chromium's namespace sandbox, so it runs with
`--no-sandbox`. What is enforced:

- a hard wall-clock kill. Chromium's own `--virtual-time-budget` bounds *page*
  time and does nothing about a page that never settles, so Vyzor kills the
  child's whole process group at its own deadline;
- a per-render V8 heap cap (`ulimit -v` cannot be used — Chromium reserves a huge
  virtual address space and dies at once under it);
- a concurrency gate that refuses with `503` and `Retry-After` rather than
  queueing, because a queue behind a slow page grows without bound;
- an output size cap, a throwaway browser profile per render, and the process
  being a child subreaper so orphaned helpers are reaped rather than accumulating;
- only public `http(s)` pages are rendered: `file://` would read this host's disk,
  and loopback and link-local addresses are refused so a render cannot reach the
  Vyzor's own port or a cloud metadata endpoint.

Run Vyzor where nothing sensitive lives.

### The cache

Rendered pages are cached under the work dir, keyed by URL and mode. It is
bounded — a byte budget and an entry count, least-recently-used eviction, and a
TTL — because an unbounded cache is a slow disk leak. A hit is served before the
concurrency slot is taken, so a repeat view does not compete with a fresh one:
measured, the same page goes from a 760 ms miss to a 0 ms hit. `--no-render-cache`
turns it off; `/api/v1/capabilities` reports entries, bytes, hits and misses.

## Live pages

Rendering draws a page once. That is the wrong shape for a page you want to *use*:
a picture has no search box you can type in and no link you can follow. `serve
--interactive` keeps a browser alive instead and streams it, so the page can be
driven:

```
GET /api/v1/interactive?url=https://example.com/&w=1024&h=768   (WebSocket upgrade)
```

Vyzor streams screencast frames down and takes input events up over one
socket. A text message introduces each image, then the image arrives as a binary
frame:

```
{"type":"ready","url":…,"width":…,"height":…}   once, on connect
{"type":"state","page":{"url":…,"title":…,"loading":…}}   when the page changes
{"type":"frame","format":"jpeg","width":…,"height":…,"seq":N}   then one binary frame
{"type":"shot", …}   then one binary frame, for a full-resolution still
{"type":"closed","reason":…}   when the session ends
```

The console sends JSON commands the other way — `navigate`, `reload`, `resize`,
`screenshot`, and `mouse`/`wheel`/`key`/`text`/`touch` for input. Vyzor
acknowledges each screencast frame itself, so the console does not have to.

### How it talks to the browser, and why

Over Chromium's `--remote-debugging-pipe`, **not** a debugging TCP port. A
debugging port is an unauthenticated control socket that any process on the host
could drive; a pipe is private to Vyzor by construction. There is no
WebSocket *client* in Vyzor either — the CDP transport is two file
descriptors, each message a NUL-terminated JSON object.

### Bounds

A live session is a whole browser held open, so it has its own opt-in and its own
gate, separate from rendering:

- `--interactive-concurrent` (default 1) bounds how many sessions exist at once.
  A full gate is refused with `503` rather than queued;
- `--interactive-idle` (default 180 s) closes a session nobody is watching. An
  abandoned browser is a leak the size of a browser;
- only public `http(s)` pages are driven — the same URL rules as rendering;
- the close path kills the browser's whole process group and reaps it, and the
  Vyzor is a child subreaper so helpers do not accumulate as zombies.

Like a render, a live page runs with `--no-sandbox`: an unprivileged container
cannot use Chromium's namespace sandbox. **A page driven here runs unsandboxed.**
That is why the console says so plainly and why Vyzor only opens pages the
operator would be willing to visit.

## Live video over WebRTC

The live-page path above streams **JPEG frames over a WebSocket**: capture with a
CDP screencast, encode, send, decode in the browser, draw into an `<img>`. Every
step is a cost, and the whole chain is gated on a CDP acknowledgement round-trip,
which caps the frame rate.

`serve --rtc` sends **raw video over WebRTC** instead:

```
chromium (software GL, on a real X display)
   | paints
Xvfb :99
   | ximagesrc
raw I420 --> vp8enc --> rtpvp8pay --> webrtcbin --SRTP--> browser
```

No JPEG, no base64, no per-frame acknowledgement, and a transport built for media:
SRTP over UDP with congestion control, rather than a TCP stream that
head-of-line blocks.

### Signalling, and why it is HTTP and not a socket

WebRTC cannot carry its own bootstrap — the two peers have to agree on
*something* before a peer connection exists, and that something cannot be WebRTC.
It does not have to be a long-lived socket either. Here it is four plain HTTP
requests:

```
POST   /api/v1/rtc                 {url}    -> {token, sdp, width, height, gathered}
POST   /api/v1/rtc/{token}/answer  {sdp}    -> {ok}
GET    /api/v1/rtc/{token}                  -> {state, connected, reason}
DELETE /api/v1/rtc/{token}                  -> {ok}
```

Input does not need a socket either: it rides a **WebRTC data channel**, inside
the peer connection the video already uses. Vyzor creates (or accepts) a
channel labelled `input`, and maps the console's own vocabulary — `navigate`,
`reload`, `resize`, `mouse`, `wheel`, `key`, `text`, `touch` — onto CDP over the
pipe it already held. So the whole session, media and control, is one WebRTC
connection.

### Gathering is awaited, not trickled

Trickle ICE exists to avoid waiting for gathering: candidates are sent as they are
found, over the signalling channel. With no signalling socket there is nowhere to
send them, so Vyzor waits instead. `mi_rtc_open` returns immediately and
`mi_rtc_wait_offer` blocks until gathering has finished, then re-reads the local
description so the candidates are *in* the SDP. The cost is a second or so at
startup; the gain is a handshake with no mid-flight messages.

`--rtc-stun` sets the STUN server used for gathering. Without one only host
candidates are produced, which works on a LAN and not across a NAT.

### Requirements

This path needs three things the JPEG path does not:

- **Xvfb** — a virtual X server. Chromium is **not** run headless here, because
  headless composites internally and never touches X, so there would be nothing
  for the capture to read. It runs with `--use-gl=swiftshader` because the virtual
  display has no GPU.
- **GStreamer** with `ximagesrc`, `vp8enc`, `rtpvp8pay` and `webrtcbin`.
- **`libnice`** (the `gstreamer1.0-nice` package) — what gives `webrtcbin` ICE.
  Without it `webrtcbin` exists but cannot connect, and the failure appears as a
  null pad at runtime rather than a missing package, which is why Vyzor
  probes for the *elements* rather than the libraries.

When any is absent the capability reports unavailable and the console hides the
control, rather than accepting a session it cannot serve.

### Bounds

- `--rtc-concurrent` (default 1) bounds sessions. A session holds an Xvfb *and* a
  Chromium, so this is the heaviest of the three page features; a full gate is
  refused with `503` rather than queued;
- `--rtc-idle` (default 180 s) closes a session nobody is watching;
- `--rtc-fps` (default 25) and `--rtc-bitrate` (kbit/s, default 2500) bound the
  encode, because every frame costs one and the encoder shares a host with the
  browser;
- only public `http(s)` pages are streamed — the same URL rules as rendering and
  the JPEG live path;
- like the others, the browser runs `--no-sandbox`, because an unprivileged
  container cannot use Chromium's namespace sandbox.

### The Origin check

A WebSocket handshake is **not** subject to CORS, so Vyzor checks the
`Origin` header itself against `MI_ALLOW_ORIGINS` and refuses a handshake from an
origin that is not listed. (The live-video routes above use plain HTTP, so they
*are* subject to CORS and get the usual treatment — plus the session token,
which is what stops one session steering another.) Without that, any page a user visits could open a
session to their Vyzor. The bearer token cannot travel as a header on a
WebSocket, so it is accepted as a `token` query parameter — the Origin check is
what makes that safe.

### Input, and the two things that are easy to get wrong

The console sends `mouse`, `wheel`, `key`, `text` and `touch` commands; Vyzor
forwards each to the matching CDP input method. Two details matter more than they
look:

- **Touch is not mouse.** A phone sends a finger, so the console forwards
  `touch` events, and `?touch=1` tells Vyzor to emulate a touch device (the
  page then lays itself out the way it would in any other browser on that
  device). The console also sends the click a tap implies, because a field takes
  focus from that click and the on-screen keyboard follows the focus. Without
  this the stage is inert on a touch device: no scroll, and no keyboard.
- **Focus emulation.** A headless browser has no window, so the page is treated
  as unfocused and a clicked field never takes the caret — typing then goes
  nowhere. `Emulation.setFocusEmulationEnabled` is what makes a live page
  typeable, and it is set for every session, not only touch ones.

### A vanished client

A tab closed behind a proxy can leave a connection that never reports EOF, and
the page may keep painting, so the idle timeout would never fire and the session
would hold its concurrency slot forever. Vyzor therefore pings an idle
socket and drops a client that has been silent too long
(`keepalive_ping_seconds` / `keepalive_timeout_seconds`), which is what keeps the
gate from filling with sessions nobody can use.

## Deployment

```bash
docker compose up --build
```

The image carries Chromium and fonts, runs as a non-root user, and puts downloads
and the render cache on a volume at `/data`. See `docker-compose.yml` for the
memory limit and why it is set where it is.

Configuration comes from the environment, which is how a container is configured
— a flag still wins where both are given:

| Variable | Meaning |
|---|---|
| `MI_HOST` / `MI_PORT` | where to listen |
| `MI_WORK_DIR` | downloads and the render cache |
| `MI_ALLOW_ORIGINS` | comma-separated origins the console may call from |
| `MI_AUTH_TOKEN` | bearer token required on every route but `/healthz` |
| `MI_USER_AGENT` | what Vyzor calls itself when fetching |
| `MI_RENDER` | set to anything to turn rendering on |
| `MI_INTERACTIVE` | set to anything to turn live pages on |
| `MI_S3_*`, `MI_CF_*` | storage and CDN credentials |

`MI_ALLOW_ORIGINS` matters more than it looks: without the console's origin on it
the browser blocks every request and the console reports "Disconnected", which
reads like Vyzor is down when it is not.

## Tests

`make test` runs real in-process servers (HTTP with and without range support,
plus a mock S3 endpoint that **independently re-derives and verifies the SigV4
signature from the wire headers**). It asserts byte-exact parallel downloads,
resume, the non-video refusal, manifest detection, streaming fallback,
RFC 4231 HMAC vectors, the AWS `get-vanilla` SigV4 vector, and the upload path.
The suite is clean under `-fsanitize=address,undefined`.

## What this is not

A downloader and transcoder for content you have the right to serve. It does
not include, and is not intended for, scraping or redistributing third-party
copyrighted media. Feed it your own uploads, licensed assets, or public-domain
material.

## Pipeline position

```
ingest  ->  HLS ladder  ->  S3-compatible upload  ->  CDN purge  ->  catalog API
       (mi_download)   (mi_hls)        (mi_storage)      (mi_storage)
```

All four stages are implemented here, so a single `ingest ... --upload` call
takes a URL all the way to playable, cache-warm HLS on a CDN.
