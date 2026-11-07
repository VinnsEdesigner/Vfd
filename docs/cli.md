# CLI

`vyzor` is the binary built by `make` into `media-ingest-c/build/vyzor`. It has
two jobs: run the server, and do single operations on their own.

```
vyzor detect <url> [-H 'K: V']...
vyzor detect-file <path>
vyzor fetch <url> -o <dir> [-n name] [-c N] [--chunk-size N]
                    [--allow-non-video] [--max-bytes N] [-q] [-H 'K: V']...
vyzor hls <input> -o <dir> [--renditions 1080,720,480] [--segment-seconds N]
vyzor ingest <url> -o <dir> [--hls <dir>] [--upload] [-c N] [-q] [-H 'K: V']...
vyzor serve [options]
vyzor upload <file-or-dir> --bucket B --access-key K --secret-key S [options]
```

Global flags: `-v` / `--verbose` (debug logging), `-q` / `--quiet` (errors only).

## detect

Reports what a URL is, without downloading it. `detect-file` does the same for a
local file. Both print the detected kind, container, confidence and media flags.

```bash
vyzor detect https://cdn.example.com/clip.mp4
vyzor detect-file ./mystery.bin
```

Detection reads the bytes, never the extension or `Content-Type`.

## fetch

Downloads one URL. Ranged and parallel by default; resumable, because each range
lands in `<dest>.parts/` with a state file, so re-running the same destination
continues rather than restarting.

```bash
vyzor fetch https://cdn.example.com/clip.mp4 -o downloads
```

| Flag | Meaning |
|---|---|
| `-o <dir>` | output directory |
| `-n <name>` | output name |
| `-c N` | connections (default 8) |
| `--chunk-size N` | range size |
| `--max-bytes N` | stop after N bytes |
| `--allow-non-video` | accept a payload the detector does not recognise |
| `-H 'K: V'` | extra request header, repeatable |

A non-video payload is refused before the download starts unless
`--allow-non-video` is given. An HLS or DASH URL is read as a manifest and its
segments are stitched into one file — see [media-pipeline.md](media-pipeline.md).

## hls

Transcodes a local file into an HLS ladder.

```bash
vyzor hls ./clip.mp4 -o ./hls --renditions 1080,720,480 --segment-seconds 6
```

Rungs above the source height are skipped, so it never upscales. Output is a
`master.m3u8` plus per-rendition playlists and segments.

## ingest

Download, detect and (optionally) transcode and upload, in one command.

```bash
vyzor ingest https://cdn.example.com/clip.mp4 -o ./out --hls ./out/hls
```

Add `--upload` with the storage flags below to push the result.

## upload

Uploads a file or a directory tree to S3-compatible storage (AWS S3, Cloudflare
R2, MinIO, Backblaze B2, DigitalOcean Spaces, Wasabi) using SigV4.

```bash
vyzor upload ./out/hls --bucket my-media --prefix hls/clip-42/ \
  --endpoint https://<account>.r2.cloudflarestorage.com --region auto \
  --access-key "$S3_KEY" --secret-key "$S3_SECRET" \
  --cdn-base https://cdn.example.com
```

| Flag | Meaning |
|---|---|
| `--endpoint URL` | default `https://s3.amazonaws.com` |
| `--region R` | default `us-east-1`; use `auto` for R2 |
| `--bucket B` | target bucket |
| `--access-key K` / `--secret-key S` | credentials (env `MI_S3_ACCESS_KEY`, `MI_S3_SECRET_KEY`) |
| `--session-token T` | optional STS token |
| `--prefix P` | key prefix, e.g. `hls/movie-42/` |
| `--content-type CT` | override for a single file |
| `--virtual-host` | bucket-in-host addressing (default is path-style) |
| `--cdn-base URL` | public base used to build returned URLs |
| `--purge` | purge uploaded URLs from Cloudflare after upload |
| `--cf-token T` / `--cf-zone Z` | Cloudflare token and zone (env `MI_CF_API_TOKEN`, `MI_CF_ZONE_ID`) |

Content-Type is inferred from the object key (`.m3u8` to
`application/vnd.apple.mpegurl`, `.ts` to `video/mp2t`) unless overridden.

## serve

Runs the server. Each page feature is its own opt-in, because each needs a
browser and real memory.

```bash
vyzor serve --host 0.0.0.0 --port 8787 --work-dir ./downloads \
  --render --interactive --allow-origin http://localhost:5173
```

Common flags:

| Flag | Meaning |
|---|---|
| `--host H` / `--port N` | where to listen |
| `--work-dir D` | downloads and the render cache |
| `--auth-token T` | bearer token required on every route but `/healthz` |
| `--allow-origin ORIGINS` | comma-separated CORS origins |
| `--user-agent UA` | the User-Agent to send |
| `--proxy URL` | egress proxy |
| `--impersonate CLIENT` | curl_cffi impersonation target, e.g. `chrome` |
| `--identity-retries N` | how many identities to try |
| `--frontier-db PATH` | SQLite file for the shared crawl frontier |
| `--render` | enable page rendering |
| `--render-timeout S` | per-render wall clock |
| `--render-concurrent N` | render slots (default 1) |
| `--render-cache DIR` / `--render-cache-ttl S` / `--no-render-cache` | the render cache |
| `--interactive` | enable live page sessions |
| `--interactive-concurrent N` | session slots (default 1) |
| `--interactive-idle S` | close a session nobody watches |
| `--rtc` | enable live video over WebRTC |
| `--rtc-concurrent N` / `--rtc-fps N` / `--rtc-bitrate KBPS` | the RTC bounds |
| `--rtc-stun URL` | STUN server for ICE |

Every flag has an `MI_*` environment equivalent; see
[configuration.md](configuration.md). A flag wins where both are given.
