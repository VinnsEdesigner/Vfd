# media-ingest-c

A fast, resumable download engine with **content-based video detection** and an
**FFmpeg → HLS** transcoding pipeline. Written in C11.

Built for ingesting media you own or are licensed to distribute.

## Components

| File | Purpose |
|---|---|
| `src/mi_detect.[ch]` | Content-based media detection (magic bytes / container structure) |
| `src/mi_download.[ch]` | Concurrent, resumable ranged downloader (libcurl + pthreads) |
| `src/mi_hls.[ch]` | FFmpeg → HLS adaptive-bitrate transcode pipeline |
| `src/mi_storage.[ch]` | S3-compatible upload (AWS SigV4) + Cloudflare cache purge |
| `src/mi_util.[ch]` | Helpers, logging, SHA-256/HMAC-SHA256, SigV4 timestamps |
| `src/mi_cli.c` | Command-line front end |

## Build

Requires a C11 compiler, libcurl development headers, and pthreads.

```bash
# Debian/Ubuntu
sudo apt-get install -y build-essential libcurl4-openssl-dev ffmpeg

make            # builds build/media-ingest
make test       # builds + runs the test suite
```

## CLI

```bash
# Identify a URL without downloading it
media-ingest detect https://cdn.example.com/clip.mp4

# Sniff a local file
media-ingest detect-file ./mystery.bin

# Download (8 connections, resumable)
media-ingest fetch https://cdn.example.com/clip.mp4 -o downloads

# Transcode to an HLS ladder
media-ingest hls ./clip.mp4 -o ./hls --renditions 1080,720,480 --segment-seconds 6

# End to end: download -> detect -> HLS
media-ingest ingest https://cdn.example.com/clip.mp4 -o ./out --hls ./out/hls

# Upload a file or a whole directory tree to S3-compatible storage
media-ingest upload ./out/hls --bucket my-media --prefix hls/clip-42/ \
  --endpoint https://<account>.r2.cloudflarestorage.com --region auto \
  --access-key "$S3_KEY" --secret-key "$S3_SECRET" \
  --cdn-base https://cdn.example.com

# Ingest + HLS + upload + CDN purge in one shot
media-ingest ingest https://cdn.example.com/clip.mp4 -o ./out --hls ./out/hls --upload \
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

## Download engine

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
