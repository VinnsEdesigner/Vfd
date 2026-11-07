# Media pipeline

This is the path a piece of media takes: detect, download, transcode, upload,
purge.

## Detection

Detection reads the **bytes**, never the extension or `Content-Type`. A file is
what its container structure says it is.

Recognised: MP4/MOV/3GP (ISO-BMFF), Matroska/WebM, AVI, FLV, Ogg, ASF/WMV,
MPEG-TS, MPEG-PS, plus HLS (`.m3u8`) and DASH (`.mpd`) manifests — from which the
referenced segment URLs are extracted. A non-video payload is refused by default,
*before* the download starts.

Two facts about detection worth knowing:

- It is run on a bounded prefix of the stream, so a format whose index sits at
  the *end* (some MP4s put their `moov` atom last) cannot be identified from the
  head alone. A ranged download still works; only the up-front detection is
  limited.
- A manifest is detected as a manifest, not as a file. The detector correctly
  calls an `.m3u8` `hls`; the transfer path then reads it as a manifest rather
  than saving the playlist as "the download".

## Download

The downloader is ranged and parallel: `Accept-Ranges: bytes` becomes N ranges on
threads, stitched at the end.

- **Resumable.** Each range lands in `<dest>.parts/` with a `state.txt` sidecar.
  Re-running the same destination continues instead of restarting.
- **Retries** with exponential backoff, per range.
- **Streaming fallback** for servers without range support.
- **SHA-256** of the finished file is available via `mi_sha256_file()`.

## Manifests: HLS and DASH

An HLS or DASH URL answers with a text playlist, not media. The transfer path
reads it with ffmpeg, follows every segment, and muxes them into one file.

- ffmpeg does the fetching, so a cookie or a proxy must reach ffmpeg too — the
  resolve's cookies only got the *resolve* past a wall.
- The output container is stated explicitly (`-f mp4`) because the destination
  name is Vyzor's own `source.bin`, from which ffmpeg cannot infer a container.
- The remux is a copy, never a re-encode: the segments are already encoded, so
  this takes seconds rather than the length of the video.
- No audio bitstream filter is forced. Forcing `aac_adtstoasc` aborts a stream
  whose audio is not AAC (Apple's example defaults to ac-3), and ffmpeg's mp4
  muxer applies the conversion itself when it is needed.

A manifest stream cannot be resumed, because it is one ffmpeg invocation. A
pause kills the child and discards the partial.

## Streaming pages

A video the extractor can read is not one URL. A watch page resolves to separate
video and audio streams — often zero formats carry both — so neither alone is
playable. ffmpeg muxes them.

The page is resolved **at download time**, not when a crawl found it: a resolved
stream URL is signed, bound to the session that asked, and expires within hours,
so a URL recorded by a crawl would be dead by the time anyone downloaded it.

When the page resolves to a *listing* rather than a video — archive.org's
`/details/` pages do this for some items, returning `_type: playlist` with
entries and zero formats — the walk resolves the last usable entry and uses its
formats.

## HLS ladder (transcode)

`vyzor hls` (and `--hls` on ingest) transcodes to an adaptive-bitrate ladder
(default 1080/720/480/360), skipping rungs above the source height so it never
upscales. Output is per-rendition `index.m3u8` + `.ts` segments and a
`master.m3u8`. x264, `main` profile, `yuv420p` for HLS compatibility, with
configurable GOP and segment length.

## Object storage and CDN

`mi_storage` implements the S3 REST API with **AWS Signature Version 4**, so it
works against AWS S3, Cloudflare R2, MinIO, Backblaze B2, DigitalOcean Spaces and
Wasabi.

- Content-Type is inferred from the object key (`.m3u8` to
  `application/vnd.apple.mpegurl`, `.ts` to `video/mp2t`, …), overridable.
- `Host` is signed **with its port** and pinned on the request, so signatures
  stay valid against endpoints on non-default ports (MinIO `:9000`, R2, local
  mocks).
- After upload, uploaded URLs can be purged from Cloudflare.

Credentials live in memory for the session and travel as request headers. They
are never written to disk or logged.

## Failure modes

Each of these has a specific cause; none is a silent success.

| Symptom | Cause |
|---|---|
| `refusing unrecognised payload (detected unknown)` | the bytes are not a recognised container and `--allow-non-video` was not given |
| `the page could not be resolved to media, and it is not a file` | the URL probed as a page, the extractor was asked, and it could not resolve — the media is not available |
| `ffmpeg could not mux the streams (exit 234)` | a codec that cannot be copied into MP4, or a source ffmpeg cannot seek over HTTP |
| `ffmpeg could not read the stream` | the manifest or its segments were refused by the host |
| a `403` from a CDN in ffmpeg's output | the stream URL is bound to the signature of the request that resolved it (YouTube's googlevideo does this) |

A URL with real bytes still falls through after an extractor failure: the
extractor not knowing a site says nothing about a URL that is plainly a file.
