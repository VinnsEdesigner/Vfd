#!/usr/bin/env bash
#
# manifest.sh — render deploy/dist/vyzorix.toml from the template and the build.
#
#   ./deploy/manifest.sh --device d_01H... --org org_01H... \
#                        --base-url https://host/path
#
# Reads the digests from deploy/dist/SHA256SUMS (written by deploy/build.sh) and
# substitutes them, with the device id, org and artifact base URL, into
# deploy/vyzorix.toml. The result is written to deploy/dist/vyzorix.toml.
#
# WHY A SCRIPT
#
# A `sha256` typed by hand drifts from the artifact the moment either changes,
# and the device refuses a mismatch (`digest_mismatch`). Taking every digest from
# SHA256SUMS makes that impossible: the manifest is a function of the build.
#
# The base URL is a flag, not a constant, because a device fetches anonymously:
# a private repository's release assets 404 without a token. The artifacts must
# be served from a public location — a public bucket, or a public repo's release.
# See deploy/README.md.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEMPLATE="$ROOT/deploy/vyzorix.toml"
DIST="$ROOT/deploy/dist"
SUMS="$DIST/SHA256SUMS"

DEVICE=""
ORG=""
BASE_URL=""
OUT="$DIST/vyzorix.toml"

while [ $# -gt 0 ]; do
    case "$1" in
        --device)   DEVICE="$2"; shift 2 ;;
        --org)      ORG="$2"; shift 2 ;;
        --base-url) BASE_URL="${2%/}"; shift 2 ;;
        --out)      OUT="$2"; shift 2 ;;
        -h|--help)  sed -n '2,24p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

die() { printf 'manifest: %s\n' "$*" >&2; exit 1; }

[ -n "$DEVICE" ]   || die "--device is required (the target device id)"
[ -n "$ORG" ]      || die "--org is required (the owning organization id)"
[ -n "$BASE_URL" ] || die "--base-url is required (where the artifacts are served, https)"
case "$BASE_URL" in
    https://*) ;;
    *) die "--base-url must be https (the manifest schema requires it)" ;;
esac
[ -f "$TEMPLATE" ] || die "no template at $TEMPLATE"
[ -f "$SUMS" ]     || die "no $SUMS — run deploy/build.sh first"

# sha_for FILE -> the digest recorded for that file, or a refusal naming it.
sha_for() {
    local file="$1" line
    line=$(grep -E "  ${file}\$" "$SUMS" | head -1 || true)
    [ -n "$line" ] || die "no digest for $file in $SUMS — run deploy/build.sh"
    printf '%s' "${line%% *}"
}

SHA_ENGINE_ARM64=$(sha_for vfd-engine-arm64.tar.gz)
SHA_ENGINE_X86_64=$(sha_for vfd-engine-x86_64.tar.gz)
SHA_CONSOLE=$(sha_for vfd-console.tar.gz)

mkdir -p "$DIST"
sed \
    -e "s|__DEVICE_ID__|${DEVICE}|g" \
    -e "s|__ORG__|${ORG}|g" \
    -e "s|__BASE_URL__|${BASE_URL}|g" \
    -e "s|__SHA_ENGINE_ARM64__|${SHA_ENGINE_ARM64}|g" \
    -e "s|__SHA_ENGINE_X86_64__|${SHA_ENGINE_X86_64}|g" \
    -e "s|__SHA_CONSOLE__|${SHA_CONSOLE}|g" \
    "$TEMPLATE" > "$OUT"

# A placeholder left behind means a substitution the template wanted but the
# renderer did not do — better to fail here than to upload a manifest the server
# refuses with a less specific message.
if grep -q '__[A-Z_]*__' "$OUT"; then
    die "a placeholder was left unsubstituted in $OUT:"
    grep -o '__[A-Z_]*__' "$OUT" | sort -u >&2
    exit 1
fi

printf 'Rendered %s\n' "$OUT"
printf '  device   %s\n' "$DEVICE"
printf '  org      %s\n' "$ORG"
printf '  base url %s\n' "$BASE_URL"
printf '\nDeploy it with:  vyzorix deploy --file %s\n' "$OUT"
