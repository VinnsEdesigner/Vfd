# Operations

How to build, run, test and deploy Vfd, and what to do when something fails.

## Build

```bash
./scripts/install-toolchain.sh     # everything, one command
```

That installs the compiler, the dev headers, GStreamer, Chromium, Xvfb, fonts,
FFmpeg, the extractors and the console's dependencies, then builds Vyzor. It is
safe to re-run: it checks each package first and reports what it skipped.

Then the console:

```bash
cd web && npm install
```

Useful flags: `--dry-run` (say what it would do), `--runtime-only` (skip the
compiler and dev headers), `--no-media` (skip the browser and media stack),
`--no-npm`, `--no-build`.

## Test

```bash
cd media-ingest-c && make test        # the server: 36 binaries, ~1300 checks
cd web && npm run typecheck && npm run lint && npm test
```

The console's integration suite drives the **real** Vyzor binary over HTTP, so
build Vyzor first or it fails with "server binary not found".

`make test` compiles every module once per test binary. A clean run takes several
minutes on four cores; it rebuilds only what changed.

## Run

```bash
cd media-ingest-c && ./build/vyzor serve --host 0.0.0.0 --port 8787 \
  --work-dir ./downloads --render --interactive
```

Or, for the whole stack — Vyzor, the console built to point at it, and the
console published:

```bash
./scripts/server.sh --port 12001
```

`server.sh` exists because the console is a static bundle with Vyzor's URL baked
in at build time, and a sandbox's public host is per-sandbox — so after a restart
the old bundle points at a host that no longer exists. Doing start, point and
publish together is what keeps them from drifting apart. It derives the public
address from `RUNTIME_ID` rather than hardcoding it, and reads the console's URL
back from Cloudflare rather than assuming `<project>.pages.dev`.

```bash
./scripts/server.sh status     # what is running, and where
./scripts/server.sh stop       # stop Vyzor
```

## Deploy

Vfd deploys to a **Vyzorix device** — an Android phone running the Vyzorix agent
and its Debian container. The console and the engine are two artifacts of one app
in a `vyzorix.toml` manifest; the device fetches them, verifies each digest,
unpacks them, and the supervisor starts the app. The console is served by the
engine at the device's own origin, so there is no CDN site and no baked API URL.

Build, render the manifest and push it:

```bash
./deploy/build.sh --arch all          # artifacts + SHA256SUMS
./deploy/manifest.sh --device d_... --org org_... --base-url https://<public>
vyzorix deploy --device d_... --file deploy/dist/vyzorix.toml
```

The artifacts must be served from a **public** URL: a device fetches them
anonymously, and a private repository's release assets `404` without a token.
`deploy/README.md` is the full runbook; `deploy/vyzorix.toml` is the manifest and
`.github/workflows/deploy-vyzorix.yml` builds and deploys from CI.

There is **no Cloudflare Pages deploy and no Fly.io deploy**. The console is not a
CDN site and the engine is not a hosted machine. A `webroot` artifact plus
`--webroot` on the engine replaced both, and removed CORS along with them.

### Running locally, without a device

`server.sh` builds the console, starts the engine, and serves the console from the
engine's own webroot — the same same-origin shape the device runs, one command:

```bash
./scripts/server.sh --port 12001     # start, build, serve
./scripts/server.sh status           # what is running, and where
./scripts/server.sh stop             # stop the engine
```

`--no-console` runs the engine as a pure API.

## The OpenHands sandbox

This repository is developed in a sandbox with two properties worth knowing:

- **Everything outside `/workspace` is wiped on a restart.** So the toolchain
  lives under `/workspace` where it can: the extractors in a virtualenv at
  `<repo>/.toolchain/venv`, the `.deb` cache at `<repo>/.toolchain/deb`. The
  packages themselves still install into `/usr` and must be reinstalled each
  restart, but from the cache, which needs no network.
- **The public host is per-sandbox**, derived from `RUNTIME_ID` (ports 12000 and
  12001 publish as `work-1-<id>` and `work-2-<id>.prod-runtime.all-hands.dev`).

After a restart, in order:

```bash
./scripts/install-toolchain.sh          # reinstalls from the cache, rebuilds
./scripts/server.sh --port 12001        # start, point, publish
```

## Failure modes

| Symptom | Cause and fix |
|---|---|
| build dies on `sqlite3.h` | `libsqlite3-dev` and `zlib1g-dev` missing — the Makefile links `-lsqlite3 -lz` |
| build dies on `gst/gst.h` | the GStreamer dev packages are missing; `mi_rtc.c` is always compiled |
| `install-toolchain.sh` aborts immediately | `set -u` and an unset variable; re-run after pulling the fix |
| every extract exits 2 in ~0.2 s | the `yt-dlp` on `PATH` is too old for a flag Vyzor passes; use the venv build |
| the venv `yt-dlp` segfaults on `--version` | the Python that built the venv has a broken `ctypes`; build the venv with the system Python |
| `the page could not be resolved to media` | the extractor could not resolve the page: site-side, stale extractor, or a sign-in wall |
| `ffmpeg could not mux the streams (exit 234)` | a codec that cannot be copied into MP4, or a source ffmpeg cannot seek |
| `403` from a CDN in ffmpeg's output | the stream URL is bound to the signature of the request that resolved it |
| `503` from a browser route | the feature is disabled, no browser is installed, or the pool is full |
| the console loads but cannot reach the engine | the page and the API are different origins; serve the console from the engine's webroot (`--webroot`) so they share one |
| a route 404s in the console but the API works | the bundle is not being served; start the engine with `--webroot web/dist`, or run `server.sh` |
| `digest_mismatch` on a deploy | the manifest's `sha256` is not the digest of the published artifact; re-render it with `deploy/manifest.sh` |
| `no_artifact_for_arch` on a deploy | the manifest has no artifact for the device's ABI; build and add it |

`GET /api/v1/capabilities` is the first thing to check: it says what this Vyzor
can actually do, rather than what was asked of it.

## What is verified

Working, checked against real sites:

- Detection; ranged resumable downloads; HLS and DASH stitching.
- Crawl with robots, sitemaps, dedup, browser fallback and a shared frontier.
- Page rendering, live pages, S3 upload and Cloudflare purge.

Known limits, all site-side or environmental:

- YouTube resolves but the final mux fails: googlevideo 403s ffmpeg fetching the
  CDN URLs directly.
- Vimeo requires a sign-in for most videos; supply cookies.
- A source in a container ffmpeg cannot remux into MP4 is resolved but not
  converted.
