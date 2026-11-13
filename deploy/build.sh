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

# The engine is built on the native (amd64/arm64) platform in every case:
#   x86_64      the host's own `make`, in deploy/bundle/Dockerfile
#   arm64/armhf a **cross** build in deploy/bundle/Dockerfile.arm64 — no qemu,
#               because cross-compiling needs none, and inside a bookworm image so
#               the glibc it links matches the device rootfs (see that file).
#
# The tarball is pulled out with `docker create` + `docker cp` rather than
# buildx's `--output type=local`. Both work, but `type=local`'s exporter stalled
# on this host after every build step had completed, and the artifact is a single
# file, so copying it out of a stopped container is simpler and does not hang.
build_engine() { # abi, out-file
    local abi="$1" out_file="$2"
    local tag="vfd-engine-build:$abi" dockerfile extra="" cid
    case "$abi" in
        arm64)
            dockerfile="Dockerfile.arm64"
            extra="--build-arg MULTIARCH=aarch64-linux-gnu --build-arg DEB_ARCH=arm64" ;;
        armeabi-v7a)
            dockerfile="Dockerfile.arm64"
            extra="--build-arg MULTIARCH=arm-linux-gnueabihf --build-arg DEB_ARCH=armhf" ;;
        x86_64)
            dockerfile="Dockerfile"
            extra="--target bundle" ;;
    esac

    # The classic builder (`DOCKER_BUILDKIT=0`) is used deliberately: on this host
    # buildx's image exporter stalled on the final "unpacking" step even though the
    # build itself had finished, and its `type=local` exporter stalled too. The
    # classic builder writes straight to the daemon and always completes.
    # shellcheck disable=SC2086
    DOCKER_BUILDKIT=0 docker build \
        $extra -t "$tag" -f "$ROOT/deploy/bundle/$dockerfile" "$ROOT" || return 1

    cid=$(docker create "$tag") || return 1
    docker cp "$cid:/out/vyzor-engine.tar.gz" "$out_file" || { docker rm -f "$cid" >/dev/null; return 1; }
    docker rm -f "$cid" >/dev/null
    docker rmi -f "$tag" >/dev/null 2>&1 || true
}

# The extractors, per ABI. Everything is downloaded and unpacked — nothing is
# compiled or executed — so a foreign ABI needs no qemu; only the multiarch tuple,
# the Debian architecture and the ffmpeg release name change.
build_extractors() { # abi, out-file
    local abi="$1" out_file="$2"
    local tag="vfd-extractors-build:$abi" tuple="" debarch="" ff="" cross="" machine="" pyplatform="" cid
    case "$abi" in
        arm64)
            tuple="aarch64-linux-gnu"; debarch="arm64"; ff="arm64"
            cross="binutils-aarch64-linux-gnu"; machine="AArch64"; pyplatform="manylinux2014_aarch64" ;;
        armeabi-v7a)
            tuple="arm-linux-gnueabihf"; debarch="armhf"; ff="armhf"
            cross="binutils-arm-linux-gnueabihf"; machine="ARM"; pyplatform="manylinux2014_armv7l" ;;
        x86_64)
            tuple="x86_64-linux-gnu"; debarch="amd64"; ff="amd64"
            cross=""; machine="Advanced Micro Devices X86-64"; pyplatform="manylinux2014_x86_64" ;;
    esac

    DOCKER_BUILDKIT=0 docker build \
        --build-arg MULTIARCH="$tuple" \
        --build-arg DEB_ARCH="$debarch" \
        --build-arg CROSS_BINUTILS="$cross" \
        --build-arg STRIP="${tuple}-strip" \
        --build-arg EXPECT_MACHINE="$machine" \
        --build-arg PY_VER=311 \
        --build-arg PY_PLATFORM="$pyplatform" \
        --build-arg FFMPEG_URL="https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-${ff}-static.tar.xz" \
        -t "$tag" -f "$ROOT/deploy/extractors/Dockerfile" "$ROOT" || return 1

    cid=$(docker create "$tag") || return 1
    docker cp "$cid:/out/vyzor-extractors.tar.gz" "$out_file" || { docker rm -f "$cid" >/dev/null; return 1; }
    docker rm -f "$cid" >/dev/null
    docker rmi -f "$tag" >/dev/null 2>&1 || true
}

log() { printf '\n=== %s\n' "$*"; }
die() { printf 'build: %s\n' "$*" >&2; exit 1; }

command -v docker >/dev/null 2>&1 || die "docker is required (it builds the engine bundle)"
command -v npm >/dev/null 2>&1 || die "npm is required (it builds the console)"

mkdir -p "$OUT"
# Remove only what this run will rebuild: building one ABI must not delete another
# ABI's artifact. SHA256SUMS is rewritten from whatever artifacts are present.
rm -f "$OUT/vfd-console.tar.gz" "$OUT/SHA256SUMS"
for abi in $ABIS; do
    rm -f "$OUT/vfd-engine-$abi.tar.gz" "$OUT/vfd-extractors-$abi.tar.gz"
done

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
    log "Building the engine for $abi"
    out_file="$OUT/vfd-engine-$abi.tar.gz"
    if ! build_engine "$abi" "$out_file"; then
        die "the engine build failed for $abi"
    fi
    [ -f "$out_file" ] || die "the build produced no tarball for $abi"
    printf '  %s (%s bytes)\n' "$out_file" "$(stat -c%s "$out_file")"
done

# ------------------------------------------------------------------ #
# The extractors — one tarball per architecture                       #
# ------------------------------------------------------------------ #
for abi in $ABIS; do
    log "Building the extractors for $abi"
    out_file="$OUT/vfd-extractors-$abi.tar.gz"
    if ! build_extractors "$abi" "$out_file"; then
        die "the extractors build failed for $abi"
    fi
    [ -f "$out_file" ] || die "the extractors build produced no tarball for $abi"
    printf '  %s (%s bytes)\n' "$out_file" "$(stat -c%s "$out_file")"
done

# ------------------------------------------------------------------ #
# The digests                                                        #
# ------------------------------------------------------------------ #
log "Digests"
( cd "$OUT" && sha256sum vfd-engine-*.tar.gz vfd-extractors-*.tar.gz vfd-console.tar.gz > SHA256SUMS )
cat "$OUT/SHA256SUMS"

printf '\nArtifacts in %s\n' "$OUT"
printf 'Next: publish them, then render the manifest with deploy/manifest.sh\n'
