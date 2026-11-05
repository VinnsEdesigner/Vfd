# Vfd

A self-hosted media downloader and browser console.

**Vfd** is two pieces that work together:

- **`media-ingest-c/`** — the server, called **Vyzor**. A C11 program that
  downloads media, transcodes it to HLS, uploads it to S3-compatible storage,
  and can drive a real browser: render a page, hold a page open for you to use,
  or stream it as video.
- **`web/`** — the console. A React/TypeScript single-page app, deployed as a
  static build, that talks to a Vyzor over HTTP and WebSocket.

The split is deliberate. The console is static, so it can live on a CDN; the
Vyzor needs a real disk and a real browser, so it runs wherever you have a
volume. Point one at the other with `VITE_API_BASE_URL`.

## Quick start

**1. Build and run Vyzor.**

```bash
cd media-ingest-c
sudo apt-get install -y build-essential libcurl4-openssl-dev ffmpeg
make
./build/vyzor serve --host 0.0.0.0 --port 8787 --work-dir ./downloads
```

Rendering, live pages and live video are each their own opt-in, because each
needs a browser and real memory:

```bash
./build/vyzor serve --render --interactive --rtc \
  --allow-origin http://localhost:5173
```

The binary also downloads, transcodes and uploads on its own — see
`media-ingest-c/README.md` for the full CLI.

**2. Run the console.**

```bash
cd web
npm install
npm run dev        # http://localhost:5173
```

Then set Vyzor's address in **Settings → Connection**, or bake it in at
build time:

```bash
VITE_API_BASE_URL=https://vyzor.example npm run build
```

## Layout

```
media-ingest-c/   Vyzor — C11 server, CLI and library
  src/            modules, all prefixed mi_
  tests/          the C test suite
web/              the console — React + TypeScript
  src/            layered: domain -> rest -> hooks -> ui
  tests/          integration, boundaries, presentation
scripts/          maintenance scripts
```

## Requirements

| Piece | Needs |
|---|---|
| Vyzor, to build | a C11 compiler, `libcurl` headers, `pkg-config`, GStreamer (`webrtc` plugins from `-bad`) |
| Vyzor, to run | FFmpeg for transcoding; Chromium, Xvfb and fonts for the browser features |
| Console | Node 20+ |

GStreamer is only needed for live video. Without it Vyzor still builds and
reports the capability as unavailable, and the console hides the control.

On Debian or Ubuntu, one command installs all of it:

```bash
./scripts/install-toolchain.sh
```

That is safe to re-run — it checks each package first and reports what it
skipped. A mirror hiccup on one archive no longer loses the whole install: it
retries, then falls back to installing one package at a time. Useful flags:

```bash
./scripts/install-toolchain.sh --dry-run        # say what it would do
./scripts/install-toolchain.sh --runtime-only   # skip the compiler and -dev headers
./scripts/install-toolchain.sh --no-media       # skip Chromium, Xvfb, GStreamer, FFmpeg
./scripts/install-toolchain.sh --no-npm         # skip npm ci in web/
```

## Scripts

| Script | Purpose |
|---|---|
| `scripts/install-toolchain.sh` | install everything the repository needs, in one command |
| `scripts/server.sh` | start Vyzor, point the console at it, and publish it |

### server.sh

One command to bring the whole stack up:

```bash
./scripts/server.sh            # start Vyzor, build + deploy the console
./scripts/server.sh status     # what is running, and where
./scripts/server.sh stop       # stop Vyzor
```

It exists because the console is a static bundle with Vyzor's URL baked in at
build time, and this sandbox's public host is per-sandbox — so after a restart
the old bundle points at a host that no longer exists. Doing start, point and
publish together is what keeps them from drifting apart.

It derives the public address from `RUNTIME_ID` rather than hardcoding it, and
reads the console's URL back from Cloudflare rather than assuming it is
`<project>.pages.dev` (it need not be — a Pages subdomain is assigned at
creation and does not move when a project is renamed).

Useful flags: `--foreground` (run attached), `--no-deploy` (build only),
`--local` (build for `localhost`), `--port N`, `--no-render`,
`--no-interactive`, `--allow-origin O`.

## Testing

Both halves have their own suite, and the console's integration suite drives the
**real** Vyzor binary over HTTP rather than a mock.

```bash
# The server
cd media-ingest-c && make test

# The console (builds nothing, but the integration suite needs the binary)
cd web && npm run typecheck && npm run lint && npm test
```

Build Vyzor first, or the integration suite fails with "server binary not
found" — the harness starts the compiled binary on an ephemeral port.

## Design notes

- **Detection reads bytes, never the extension or `Content-Type`.** A file is
  what its container structure says it is.
- **Rendering is resource-bounded, not a sandbox.** Chromium runs with
  `--no-sandbox` because an unprivileged container cannot use its namespace
  sandbox. Only public `http(s)` pages are opened, and the operator is expected
  to run Vyzor where nothing sensitive lives.
- **The console's layers are enforced by lint**, not convention: `ui` may import
  only `ui`, `hooks` and `stores`. A server change touches `domain/normalize.ts`
  and `rest/`, never a component.
- **Secrets never persist.** Storage credentials live in memory for the session
  and travel as request headers.
- **Rendering is optional at every layer.** No browser, no render control — a
  graceful degradation rather than an error.

## Licence

GPL-3.0. See `LICENSE`.
