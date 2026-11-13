# Deploying Vfd to a device

Vfd runs on a **Vyzorix device** — an Android phone running the Vyzorix agent and
its Debian container. This directory holds everything needed to build and deploy
it there. It replaces the previous Cloudflare Pages and Fly.io deployments.

## What changed, and why

| Before | After |
|:--|:--|
| Console on Cloudflare Pages (a CDN site) | The console ships inside the app and is served by the engine at the device's own origin |
| Engine on a Fly.io machine | The engine runs in the device's container |
| Two hosts, `VITE_API_BASE_URL` baked in at build time | One app, one origin, no baked URL and no CORS |

The console and the engine are two **artifacts** of one app in a Vyzorix manifest.
The device fetches them, verifies each against its `sha256`, unpacks them, and the
supervisor starts the app. Nothing is served from a datacenter; the phone is the
host.

## The pieces

```
deploy/
  vyzorix.toml        the manifest template (device, org, artifacts) — one file a human authors
  manifest.sh         renders vyzorix.toml from the build's digests
  build.sh            builds both artifacts and SHA256SUMS
  app/run.sh          the app's entrypoint inside the container (the manifest's `cmd`)
  bundle/Dockerfile   builds the engine artifact for one architecture
  dist/               build output (git-ignored)
```

## Build

```bash
./deploy/build.sh                 # the host's architecture
./deploy/build.sh --arch all      # arm64 + x86_64 (needs qemu + binfmt, as CI has)
```

This writes, into `deploy/dist/`:

- `vfd-engine-<abi>.tar.gz` — the engine, per architecture. It carries `bin/vyzor`,
  its shared-library closure under `lib/`, and `run.sh`.
- `vfd-console.tar.gz` — the console build. Architecture-free, because it is static
  files.
- `SHA256SUMS` — the digests the manifest's `sha256` fields carry.

The engine is built in a container of the target architecture (`bundle/Dockerfile`),
so the libraries it links are the ones it ships. arm64 and armhf need qemu + binfmt;
CI provides that with `docker/setup-qemu-action`.

## Publish the artifacts

The device fetches artifacts **anonymously**, so they must be at a **public** URL.
A private repository's release assets return `404` without a token, and a device
cannot provide one. Two options that work:

1. **A public repository's release.** Tag `v*`; the workflow attaches the three
   files to the release. The base URL is then
   `https://github.com/<owner>/<repo>/releases/download/<tag>`.
2. **A public bucket** (R2, S3 with a public prefix). The base URL is the prefix
   the three files sit under.

Set the base URL as the repository variable `ARTIFACT_BASE_URL`.

## Render the manifest

```bash
./deploy/manifest.sh \
  --device d_01H... \
  --org    org_01H... \
  --base-url https://github.com/<owner>/<repo>/releases/download/v1.0.0
# -> deploy/dist/vyzorix.toml
```

Every digest comes from `SHA256SUMS`; the device refuses a mismatch
(`digest_mismatch`), so a digest typed by hand is a deploy that cannot land.

## Deploy

```bash
vyzorix login --server https://vyzorix.fly.dev --api-key <key>   # once
vyzorix deploy --device d_01H... --file deploy/dist/vyzorix.toml
```

In CI, use a **device-scoped deploy credential** instead of a person's API key —
it reaches exactly one device and is revocable. The workflow does this when
`secrets.VYZORIX_DEPLOY_CREDENTIAL` is set. See
`VYZORIX/DOCUMENTATION/DEPLOY_CREDENTIALS.md`.

A `--dry-run` compiles and signs on the server but stores and delivers nothing: it
is how you confirm a manifest will be accepted, and see the version it would get,
without touching a device.

## What the device does with it

1. Verifies the config's signature against the pinned server key.
2. Fetches each artifact and verifies its `sha256` over the whole file.
3. Unpacks atomically: `engine/` and `console/www/` under `/home/apps/vfd/`.
4. Writes `apps.json` and pings the supervisor.
5. The supervisor starts `run.sh`, which starts the engine serving the console at
   `http://127.0.0.1:8787/`.

A failure at any step changes nothing — the previous version keeps running — and
the device reports the reason (`digest_mismatch`, `no_artifact_for_arch`,
`cmd_not_executable`, …).

## Reaching the console

The app is `expose = "loopback"`, so it is reachable from the device itself. Open a
terminal from the Vyzorix dashboard and browse to `http://127.0.0.1:8787/`, or set
`expose = "lan"` (with a `port`) to reach it from the local network with a token.
`expose = "cloud"` is refused in v1.

## Not deployed here: the extractors

`yt-dlp`, `gallery-dl`, `py1337x`, `aria2` and `ffmpeg` are **not** in the bundle.
They are large and they go stale, and a device without them still downloads direct
URLs and HLS manifests. When they are present on the container's `PATH`, the
console offers video and gallery modes. Install them into the container's overlay
(`/usr/local`) if you want them; the engine resolves each by name and reports the
capability it actually has.
