#!/usr/bin/env bash
#
# build.sh — build the two artifacts a Vyzorix deploy ships, and their digests.
#
#   ./deploy/build.sh                 build for the host architecture
#   ./deploy/build.sh --arch arm64    build the engine for arm64 (needs qemu)
#   ./deploy/build.sh --arch all      build every ABI the device may be
#
# Produces, in deploy/dist/:
#
#   vfd-engine-<abi>.tar.gz   the `engine` artifact, one per architecture
#   vfd-console.tar.gz        the `console` artifact (a webroot, architecture-free)
#   SHA256SUMS                the digests the manifest's `sha256` fields carry
#
# WHY THE DIGEST MATTERS
#
# The manifest references each artifact by URL *and* digest, and the device
# verifies the digest over the whole file. The URL is not trusted; the bytes are.
# So the manifest is only correct when it carries the digest of the exact tarball
# that was published, which is why this script writes SHA256SUMS and the manifest
# is rendered from it rather than by hand.
#
# WHY DOCKER FOR THE ENGINE
#
# See deploy/bundle/Dockerfile: the engine links libcurl/sqlite3/GStreamer, and a
# Debian container of the target architecture has all three, so the build is the
# host's `make` and the libraries it ships are the ones it linked. arm64 and
# armhf need qemu + binfmt, which CI provides (`docker/setup-qemu-action`); on a
# host without it, `--arch` for a foreign ABI is refused with that reason.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/deploy/dist"

HOST_ARCH="$(uname -m)"
case "$HOST_ARCH" in
    x86_64|amd64) HOST_ABI="x86_64" ;;
    aarch64|arm64) HOST_ABI="arm64" ;;
    armv7l|armhf) HOST_ABI="armeabi-v7a" ;;
    *) HOST_ABI="" ;;
esac

ARCH="host"

while [ $# -gt 0 ]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        --out)  OUT="$2";  shift 2 ;;
        -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

case "$ARCH" in
    host)        ABIS="$HOST_ABI" ;;
    all)         ABIS="arm64 x86_64" ;;
    arm64|armeabi-v7a|x86_64) ABIS="$ARCH" ;;
    *) echo "--arch must be host, all, arm64, armeabi-v7a or x86_64 (got '$ARCH')" >&2; exit 2 ;;
esac
[ -n "$ABIS" ] || { echo "cannot infer the host ABI; pass --arch" >&2; exit 2; }

platform_for() { # abi
    case "$1" in
        arm64)       printf 'linux/arm64' ;;
        armeabi-v7a) printf 'linux/arm/v7' ;;
        x86_64)      printf 'linux/amd64' ;;
    esac
}

log() { printf '\n=== %s\n' "$*"; }
die() { printf 'build: %s\n' "$*" >&2; exit 1; }

command -v docker >/dev/null 2>&1 || die "docker is required (it builds the engine bundle)"
command -v npm >/dev/null 2>&1 || die "npm is required (it builds the console)"

mkdir -p "$OUT"
rm -f "$OUT"/vfd-engine-*.tar.gz "$OUT/vfd-console.tar.gz" "$OUT/SHA256SUMS"

# ------------------------------------------------------------------ #
# The console — architecture-free, built once                        #
# ------------------------------------------------------------------ #
log "Building the console"
if [ -d "$ROOT/web/node_modules" ]; then
    ( cd "$ROOT/web" && env -u VITE_API_BASE_URL npm run build )
else
    ( cd "$ROOT/web" && env -u VITE_API_BASE_URL npm ci && env -u VITE_API_BASE_URL npm run build )
fi
[ -f "$ROOT/web/dist/index.html" ] || die "the console build produced no index.html"

# The device unpacks a `webroot` artifact into <app>/console/www/, so the tarball
# holds the dist contents at its root.
tar -czf "$OUT/vfd-console.tar.gz" -C "$ROOT/web/dist" .

# ------------------------------------------------------------------ #
# The engine — one tarball per architecture                          #
# ------------------------------------------------------------------ #
for abi in $ABIS; do
    platform="$(platform_for "$abi")"
    log "Building the engine for $abi ($platform)"

    stage="$(mktemp -d)"
    # `--target bundle` stops at the assembled stage and copies its filesystem
    # out, so the artifact is /out/vyzor-engine.tar.gz inside the stage.
    if ! docker buildx build \
            --platform "$platform" \
            --target bundle \
            --output "type=local,dest=$stage" \
            -f "$ROOT/deploy/bundle/Dockerfile" \
            "$ROOT"; then
        rm -rf "$stage"
        die "the engine build failed for $abi (a foreign ABI needs qemu + binfmt; run docker/setup-qemu-action in CI)"
    fi

    built="$stage/out/vyzor-engine.tar.gz"
    [ -f "$built" ] || { rm -rf "$stage"; die "the build produced no tarball for $abi"; }
    mv "$built" "$OUT/vfd-engine-$abi.tar.gz"
    rm -rf "$stage"
done

# ------------------------------------------------------------------ #
# The digests                                                        #
# ------------------------------------------------------------------ #
log "Digests"
( cd "$OUT" && sha256sum vfd-engine-*.tar.gz vfd-console.tar.gz > SHA256SUMS )
cat "$OUT/SHA256SUMS"

printf '\nArtifacts in %s\n' "$OUT"
printf 'Next: publish them, then render the manifest with deploy/manifest.sh\n'
