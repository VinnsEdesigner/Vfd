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
./deploy/build.sh --arch all      # arm64 + x86_64
```

This writes, into `deploy/dist/`:

- `vfd-engine-<abi>.tar.gz` — the engine, per architecture. It carries `bin/vyzor`,
  its shared-library closure under `lib/`, and `run.sh`.
- `vfd-console.tar.gz` — the console build. Architecture-free, because it is static
  files.
- `SHA256SUMS` — the digests the manifest's `sha256` fields carry.

The engine is built on the **native** platform in every case, so **no qemu is
needed**:

- `x86_64` — the host's own `make`, in `bundle/Dockerfile`.
- `arm64` / `armhf` — a **cross** build in `bundle/Dockerfile.arm64`, using
  `gcc-aarch64-linux-gnu` and the `:arm64` dev libraries, on an amd64 host.

That cross-build runs inside a **bookworm** container on purpose. The device's
rootfs is Debian bookworm (glibc 2.36); building on a newer suite would link
symbols the rootfs does not have, and the binary would fail on the device with
`version 'GLIBC_2.xx' not found` — a failure that appears nowhere but on the
device. The Dockerfile asserts the arch and prints the highest GLIBC symbol the
engine needs, so a bad build fails here rather than there.

The library closure is resolved by `bundle/libclosure.py` (readelf, not `ldd` —
`ldd` would try to run a foreign binary) and copied into the artifact, minus the
glibc core, which the rootfs provides.

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
  --base-url https://github.com/<owner>/<repo>/releases/download/v1.0.0 \
  --arch   arm64-v8a
# -> deploy/dist/vyzorix.toml
```

Every digest comes from `SHA256SUMS`; the device refuses a mismatch
(`digest_mismatch`), so a digest typed by hand is a deploy that cannot land.

### One device, one `arch`

`--arch` names the **target device's** ABI (`arm64-v8a`, `armeabi-v7a`, `x86_64`)
and the renderer emits **one** engine artifact for it.

Listing one engine per ABI in a single manifest is a trap: the entries share the
artifact `name`, so they install to the same path, and a device that takes them
all is left with whichever came last — which is how a phone ends up running a
foreign binary. The device now selects by `arch` (the best match for its own
`Build.SUPPORTED_ABIS`, or the only candidate), and refuses with
`no_artifact_for_arch` when nothing matches — but the manifest should still
declare the one build the device needs. Build every ABI (`--arch all`), publish
them all, and render a manifest per device ABI.

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

## The extractors artifact

`yt-dlp`, `gallery-dl`, `py1337x`, `aria2c`, `ffmpeg` and `ffprobe` ship as a
**third artifact**, per ABI, built by `deploy/extractors/Dockerfile`. The engine
resolves each by name on `PATH` (`mi_extract.c`), and `run.sh` puts the bundle's
`bin/` first, so they light up the video, gallery and torrent modes.

They are optional by design: a device without the artifact still downloads a
direct URL or an HLS manifest, and the engine reports the capability it actually
has rather than failing. Keeping them out of the engine artifact means the engine
can be updated without re-shipping ~100 MB.

Nothing here is compiled or executed during the build — ffmpeg/ffprobe are a
prebuilt **static** arm64 release, `aria2c` comes from Debian's arm64 archive
(only unpacked), and pip resolves the Python wheels for the target platform — so
a foreign ABI needs **no qemu**.

ffmpeg is static on purpose. Debian's is dynamic and its closure is **199
libraries, ~200 MB** (opencv, blas, lapack, icu, flite, codec2, …), several in
subdirectories a flat soname search misses — and a closure that is subtly wrong
is a tool that fails only on the device. A static build is one file with no
closure to get wrong.
