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

## Torrents

A torrent index is a third kind of source, and it is not a stream or a file. The
page carries no media URL and no file listing; the download is a **magnet link**,
and the bytes arrive from a swarm. Neither yt-dlp nor gallery-dl resolves one, so
the torrent index is its own extractor kind.

Two tools do the two halves:

- **`py1337x`** (via the `scripts/py1337x-json.py` wrapper) resolves an index —
  1337x — to magnet links. It answers a *search* as a list, one entry per
  torrent, and each entry's URL is the magnet.
- **`aria2`** fetches the magnet. A crawl or a download of a magnet is handed to
  `aria2c`, which joins the swarm, writes the files, and is stopped once the
  first file is complete. The largest completed file is what the caller gets —
  the video, for a media torrent.

Routing is by host: a `1337x` URL goes to the torrent extractor automatically. A
**bare search** — "find this on the index" — has no host to go on, so it is only
routed when `torrent_site` is configured (`MI_TORRENT_SITE`).

**1337x sits behind Cloudflare**, and answers a datacenter address with a JS
challenge (HTTP 403, "Just a moment…") that no client clears — not plain
requests, not a TLS-impersonating client, not headless or headful Chromium. The
wrapper routes its requests through `curl_cffi`'s browser fingerprint, which is
strictly better than plain requests but is not a guarantee. On such a host a
torrent search returns **zero results with no error**, which is why the crawl
reports `pages_failed`/`blocked` rather than claiming the index is empty. A
residential or proxied egress is what actually fixes it.

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

## Why YouTube fails, and what fixes it

A YouTube watch page resolves fine — yt-dlp lists its formats — but the fetch of
the media itself can fail with a `403`, or with a `Sign in to confirm you're not
a bot` wall. This is not a defect in Vyzor, and it is worth understanding because
the fix is a setting, not a code change.

**What is happening.** YouTube decides whether to serve the media bytes by two
things, and either can refuse:

1. **The address the request comes from.** A datacenter range (a cloud VM, a
   container host) is treated as suspicious, and the media URL is refused even
   though the metadata endpoint answered.
2. **Whether the request looks like a real, signed-in session.** YouTube has been
   moving to PO-token and SABR checks that bind a stream to a proof of origin.
   Against these the IP is not the deciding factor; a genuine session is.

**The two levers, and how they differ.**

| Lever | Setting | Fixes |
|---|---|---|
| A session's cookies | `MI_COOKIES_FILE`, or the console's Settings → Egress cookies | the bot wall, and a PO-token challenge — works from any address |
| A residential or mobile address | `MI_PROXY`, or run Vyzor on the device | the address-reputation refusal only |

Cookies are the reliable lever, because they do not depend on the host's address
being trusted. A proxy helps when the refusal is about reputation, but it is the
same *kind* of signal as the address itself, so it can work today and stop
working when the checks tighten.

**There is no "use the device's IP".** Vyzor's browser is a Chromium on Vyzor's
own host — the same machine, the same network, the same egress address as the
downloader. It is not a phone, and an IP is not something one process can read
out of another and borrow. The only ways to change the egress are to run Vyzor
where that address is (on the device), or to route it through a proxy that has
it. Both are configuration.

**Where the cookies go.** A resolve and a download are two requests, so the
credential must reach both: yt-dlp takes `--cookies`, and ffmpeg — which fetches
the segments — takes `-cookies`. Vyzor passes the same export to both, so a
cookie set once covers the resolve, the extractor's own downloader and the mux.

**Three ways to give Vyzor the cookies**, in precedence order:

| Source | Setting | Notes |
|---|---|---|
| A browser on Vyzor's host | `MI_COOKIES_FROM_BROWSER=chrome`, or Settings → Egress | no export to make: yt-dlp reads the profile. The extractor does this, so it covers the resolve and the extractor's downloader — not ffmpeg |
| A Netscape file | `MI_COOKIES_FILE=/path/cookies.txt` | the path, not the contents, so the credential lives in a file you can `chmod 600` |
| Pasted into the console | Settings → Egress → Cookies | held in memory for the session; never written to disk |

They are exclusive, not layered: yt-dlp lets `--cookies-from-browser` win over
`--cookies`, so naming both would make the file silently ineffective. Vyzor sends
whichever one source was given.

`--cookies-from-browser` **aborts the run** when the named browser has no cookie
store rather than continuing anonymously — measured, `could not find chromium
cookies database`. So it is worth setting only on a host where that browser is
actually signed in, and the failure is a clear message rather than a silent
fallback. The browser may be left running: yt-dlp copies the cookie database to
a temp file before reading it.

**How it reads them.** For a Chromium-family browser, yt-dlp finds the profile's
`Cookies` SQLite database under the browser's user-data directory, copies it
aside, and reads the `cookies` table. The values are encrypted at rest, so it
also derives the decryption key — from the OS keyring (Secret Service / KWallet)
on Linux, the Keychain on macOS, the DPAPI on Windows — and decrypts each row
(`v10`/`v11` schemes). For Firefox it reads `cookies.sqlite` directly; those are
not encrypted. Either way the result is a cookie jar for that one run, which is
exactly what a `cookies.txt` export would have contained.

That is why it needs the *same machine*: the key lives in that machine's keyring,
and the profile in that machine's filesystem. A container running Vyzor cannot
reach the host's browser store unless the profile is mounted into it.

### Signing in through Vyzor's own browser

The tidiest route needs no export and no browser on the host: sign in once
through the **live page view**, and let Vyzor keep the profile.

```bash
MI_PERSISTENT_PROFILE=/data/vyzor-profile ./build/vyzor serve \
  --render --interactive …
```

With that set:

- the live browser uses that directory as its `--user-data-dir` and **does not
  delete it** when the session ends (the default is a throwaway profile per
  session, removed on close);
- the browser is launched with `--password-store=basic`, so the cookie store is
  one yt-dlp can decrypt without a system keyring — a headless host usually has
  none, and a keyring-encrypted store would be unreadable;
- the extractor is pointed at that profile automatically
  (`--cookies-from-browser chrome:<dir>`), so there is no second setting to
  forget.

The flow is then: open a live page, navigate to the site, sign in, close it — and
the session is on disk for every later resolve and download.

Two details that matter:

- **The browser is closed gracefully.** Chromium buffers its cookie database and
  writes it on a clean shutdown; a `SIGKILL` loses it. Measured: a graceful
  `Browser.close` leaves the cookie on disk, a kill does not. So when a
  persistent profile is configured, Vyzor asks the browser to close and waits
  (up to ~6 s) before the firm kill. Without this the sign-in would be lost
  every time.
- **The profile is a credential.** It holds a live session for every site signed
  into through it. Put it on the same volume as the downloads, keep it out of
  version control, and treat it like the cookie file.

**If it still fails.** Read the Logs tab in the console, or `GET /api/v1/logs`.
A `403` from a `googlevideo.com` URL is the address refusal; a message naming
`Sign in to confirm you're not a bot` is the session wall. The two need different
fixes, and the log says which one you have.

## Failure modes

Each of these has a specific cause; none is a silent success.

| Symptom | Cause |
|---|---|
| `refusing unrecognised payload (detected unknown)` | the bytes are not a recognised container and `--allow-non-video` was not given |
| `the page could not be resolved to media, and it is not a file` | the URL probed as a page, the extractor was asked, and it could not resolve — the media is not available |
| `ffmpeg could not mux the streams (exit 234)` | a codec that cannot be copied into MP4, or a source ffmpeg cannot seek over HTTP |
| `ffmpeg could not read the stream` | the manifest or its segments were refused by the host |
| a `403` from a CDN in ffmpeg's output | the stream URL is bound to the signature of the request that resolved it, or the host refuses this address (YouTube's googlevideo does both) |
| `Sign in to confirm you're not a bot` | YouTube's session wall; set cookies. See the section above |

A URL with real bytes still falls through after an extractor failure: the
extractor not knowing a site says nothing about a URL that is plainly a file.
