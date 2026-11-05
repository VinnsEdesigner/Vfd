#!/usr/bin/env bash
#
# install-toolchain.sh — everything Vfd needs, in one command.
#
#   ./scripts/install-toolchain.sh
#
# WHAT IT INSTALLS, AND WHY EACH THING IS HERE
#
#   build-essential, pkg-config        compile Vyzor (C11) and find its libs
#   libcurl4-openssl-dev               Vyzor's downloader and HTTP client
#   libgstreamer1.0-dev + base/bad     Vyzor's live-video build deps
#   ca-certificates                    TLS trust store
#   chromium                           the browser Vyzor renders and drives with
#   xvfb                               a virtual X display for live video
#   fonts-*                            without them every page renders as boxes
#   gstreamer1.0-* (base/good/bad/…)   the runtime pipeline for live video
#   gstreamer1.0-nice                  gives webrtcbin ICE; without it live
#                                      video fails at runtime with a null pad
#   ffmpeg                             HLS transcoding and conversion
#   nodejs, npm                        build and test the console
#
# GStreamer is only needed for live video, and Chromium/Xvfb only for the
# browser features. Both are installed by default because "install the
# toolchain" should give you a Vyzor that can do everything; use
# `--runtime-only` to skip the compiler and dev headers, or `--no-media` to
# skip the browser and media stack.
#
# It is safe to re-run. Every step checks first and says what it skipped.

set -uo pipefail

# ------------------------------------------------------------------ #
# Output                                                              #
# ------------------------------------------------------------------ #
if [ -t 1 ]; then
    C_RESET=$'\033[0m'; C_BOLD=$'\033[1m'; C_DIM=$'\033[2m'
    C_OK=$'\033[32m'; C_WARN=$'\033[33m'; C_ERR=$'\033[31m'
else
    C_RESET=''; C_BOLD=''; C_DIM=''; C_OK=''; C_WARN=''; C_ERR=''
fi

step()  { printf '%s==>%s %s\n' "$C_BOLD" "$C_RESET" "$1"; }
ok()    { printf '  %s✓%s %s\n' "$C_OK" "$C_RESET" "$1"; }
warn()  { printf '  %s!%s %s\n' "$C_WARN" "$C_RESET" "$1"; }
fail()  { printf '  %s✗%s %s\n' "$C_ERR" "$C_RESET" "$1"; }
note()  { printf '  %s%s%s\n' "$C_DIM" "$1" "$C_RESET"; }

die() { fail "$1"; exit 1; }

# ------------------------------------------------------------------ #
# Options                                                             #
# ------------------------------------------------------------------ #
RUNTIME_ONLY=0
NO_MEDIA=0
NO_NODE=0
NO_NPM=0
NO_BUILD=0
DRY_RUN=0
PARTIAL=0

usage() {
    sed -n '3,40p' "$0" | sed 's/^# \{0,1\}//'
    cat <<'EOF'

Options:
  --runtime-only   skip the C compiler and -dev headers (just run a build)
  --no-media       skip Chromium, Xvfb, fonts and GStreamer (no page features)
  --no-node        skip Node.js and npm
  --no-npm         skip `npm ci` in web/
  --no-build       skip building Vyzor and the console
  --dry-run        print what would be installed, change nothing
  -h, --help       this message
EOF
}

for arg in "$@"; do
    case "$arg" in
        --runtime-only) RUNTIME_ONLY=1 ;;
        --no-media)     NO_MEDIA=1 ;;
        --no-node)      NO_NODE=1 ;;
        --no-npm)       NO_NPM=1 ;;
        --no-build)     NO_BUILD=1 ;;
        --dry-run)      DRY_RUN=1 ;;
        -h|--help)      usage; exit 0 ;;
        *) die "unknown option: $arg (try --help)" ;;
    esac
done

# ------------------------------------------------------------------ #
# Where are we                                                       #
# ------------------------------------------------------------------ #
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(dirname -- "$SCRIPT_DIR")
[ -d "$ROOT/media-ingest-c" ] || die "no media-ingest-c/ under $ROOT — run this from the repository"
[ -d "$ROOT/web" ] || die "no web/ under $ROOT — run this from the repository"

# ------------------------------------------------------------------ #
# Privilege                                                          #
# ------------------------------------------------------------------ #
if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
elif command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
else
    die "need root or sudo to install packages"
fi

# ------------------------------------------------------------------ #
# Distribution                                                       #
# ------------------------------------------------------------------ #
DISTRO_ID=""; DISTRO_LIKE=""; CODENAME=""
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    DISTRO_ID=${ID:-}; DISTRO_LIKE=${ID_LIKE:-}; CODENAME=${VERSION_CODENAME:-}
fi

case "$DISTRO_ID $DISTRO_LIKE" in
    *debian*|*ubuntu*) ;;
    *) die "this script supports Debian and Ubuntu (found: ${DISTRO_ID:-unknown}).
    On another distribution install the equivalents of: build-essential pkg-config
    libcurl4-openssl-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
    libgstreamer-plugins-bad1.0-dev chromium xvfb ffmpeg nodejs npm" ;;
esac

# ------------------------------------------------------------------ #
# Package list                                                       #
# ------------------------------------------------------------------ #
BUILD_PKGS="build-essential pkg-config libcurl4-openssl-dev ca-certificates"
GST_BUILD_PKGS="libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev"
MEDIA_PKGS="chromium xvfb fonts-liberation fonts-dejavu-core fonts-noto-core fonts-noto-color-emoji \
gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
gstreamer1.0-plugins-ugly gstreamer1.0-libav gstreamer1.0-nice ffmpeg yt-dlp"

WANT=""
[ "$RUNTIME_ONLY" -eq 0 ] && WANT="$WANT $BUILD_PKGS $GST_BUILD_PKGS"
[ "$NO_MEDIA" -eq 0 ] && WANT="$WANT $MEDIA_PKGS"
WANT=$(printf '%s' "$WANT" | tr -s ' ' | sed 's/^ //; s/ $//')

# ------------------------------------------------------------------ #
# apt helpers                                                        #
# ------------------------------------------------------------------ #
APT_UPDATED=0
apt_update() {
    [ "$APT_UPDATED" -eq 1 ] && return 0
    step "Updating package lists"
    if [ "$DRY_RUN" -eq 1 ]; then note "would run: $SUDO apt-get update"; APT_UPDATED=1; return 0; fi
    if $SUDO apt-get update -qq; then ok "package lists updated"; else warn "apt-get update failed; continuing"; fi
    APT_UPDATED=1
}

# Is a package name known to apt? Keeps one unavailable package (Chromium is
# packaged differently on some releases) from failing the whole install.
pkg_available() { apt-cache show "$1" >/dev/null 2>&1; }

pkg_installed() { dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q 'install ok installed'; }

install_pkgs() {
    # Split the list into what is missing and what apt actually offers.
    local to_install="" missing="" p
    for p in $1; do
        if pkg_installed "$p"; then continue; fi
        if pkg_available "$p"; then to_install="$to_install $p"; else missing="$missing $p"; fi
    done
    to_install=$(printf '%s' "$to_install" | sed 's/^ //')

    if [ -n "$missing" ]; then
        warn "not available on this distribution:$missing"
    fi
    if [ -z "$to_install" ]; then
        ok "already installed"
        return 0
    fi
    note "installing:$to_install"
    if [ "$DRY_RUN" -eq 1 ]; then return 0; fi

    # A mirror can serve a transient 5xx for one archive (Debian's snapshot
    # mirror does this often), and `apt-get install` then fails as a whole —
    # losing every package that would have installed fine. So: retry once as a
    # batch, and if that still fails, fall back to one package at a time so a
    # single bad fetch costs only that package.
    local attempt
    for attempt in 1 2; do
        # DEBIAN_FRONTEND avoids a prompt for a package that wants one.
        if $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends $to_install; then
            ok "installed"
            return 0
        fi
        if [ "$attempt" -eq 1 ]; then
            warn "batch install failed (often a transient mirror error); refreshing lists and retrying"
            sleep 2
            $SUDO apt-get update -qq 2>/dev/null || true
        fi
    done

    warn "batch install still failing; trying each package on its own"
    local failed="" one
    for one in $to_install; do
        if $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends "$one" 2>/dev/null; then
            ok "$one"
        else
            failed="$failed $one"
        fi
    done
    if [ -n "$failed" ]; then
        warn "could not install:$failed"
        note "re-run this script to retry — mirrors often recover"
        return 1
    fi
    return 0
}

# ------------------------------------------------------------------ #
# 1. System packages                                                 #
# ------------------------------------------------------------------ #
if [ -n "$WANT" ]; then
    apt_update
    step "Installing system packages"
    # A partial failure (a mirror hiccup on one archive) should not abort the
    # run: everything else — Node, npm, the build — is still worth doing, and
    # the summary at the end is where the user sees what is missing.
    install_pkgs "$WANT" || PARTIAL=1
else
    step "System packages"
    note "nothing requested (--runtime-only with --no-media)"
fi

# ------------------------------------------------------------------ #
# 2. Node.js                                                         #
# ------------------------------------------------------------------ #
NODE_MAJOR=0
if command -v node >/dev/null 2>&1; then
    NODE_MAJOR=$(node -v 2>/dev/null | sed 's/^v//; s/\..*//')
    case "$NODE_MAJOR" in ''|*[!0-9]*) NODE_MAJOR=0 ;; esac
fi

if [ "$NO_NODE" -eq 1 ]; then
    step "Node.js"
    note "skipped (--no-node)"
elif [ "$NODE_MAJOR" -ge 20 ] 2>/dev/null; then
    step "Node.js"
    ok "node $(node -v) is already new enough (>= 20)"
    command -v npm >/dev/null 2>&1 || { warn "npm is missing; installing"; apt_update; install_pkgs "npm"; }
else
    step "Node.js"
    if [ "$NODE_MAJOR" -gt 0 ]; then
        warn "node $(node -v) is older than 20; installing the distribution's Node"
    else
        note "node not found; installing"
    fi
    apt_update
    install_pkgs "nodejs npm" || warn "could not install Node; the console will not build"
fi

# ------------------------------------------------------------------ #
# 3. Console dependencies                                            #
# ------------------------------------------------------------------ #
if [ "$NO_NPM" -eq 1 ]; then
    step "Console dependencies"
    note "skipped (--no-npm)"
elif ! command -v npm >/dev/null 2>&1; then
    step "Console dependencies"
    warn "npm is not installed; skipping"
elif [ -d "$ROOT/web/node_modules" ]; then
    step "Console dependencies"
    ok "web/node_modules already present (delete it and re-run to refresh)"
else
    step "Console dependencies"
    if [ "$DRY_RUN" -eq 1 ]; then
        note "would run: npm ci (in web/)"
    elif [ -f "$ROOT/web/package-lock.json" ]; then
        if (cd "$ROOT/web" && npm ci --no-audit --no-fund); then ok "npm ci done"; else warn "npm ci failed; try 'npm install' in web/"; fi
    else
        if (cd "$ROOT/web" && npm install --no-audit --no-fund); then ok "npm install done"; else warn "npm install failed"; fi
    fi
fi

# ------------------------------------------------------------------ #
# 3b. A current yt-dlp                                               #
#                                                                    #
# The distribution's copy is the one that matters here, and it can be  #
# badly stale: Debian trixie ships 2025.04.30, which is eighteen       #
# months behind and could not see YouTube's audio formats at all — it  #
# reported 21 formats, none of them audio, where the current release   #
# reports 48 including five. The tool's whole job is keeping up with   #
# sites that change, so a stale one is worse than none.                #
# ------------------------------------------------------------------ #
if [ "$NO_MEDIA" -eq 0 ] && command -v yt-dlp >/dev/null 2>&1; then
    step "Updating yt-dlp"
    if [ "$DRY_RUN" -eq 1 ]; then
        note "would run: pip install --upgrade yt-dlp"
    elif command -v pip >/dev/null 2>&1 || command -v pip3 >/dev/null 2>&1; then
        PIP=$(command -v pip3 || command -v pip)
        # Not fatal: the distribution's copy still works for many sites, and a
        # failure here should not fail the whole install.
        if $SUDO $PIP install --quiet --upgrade --break-system-packages yt-dlp 2>/dev/null \
           || $PIP install --quiet --upgrade --break-system-packages yt-dlp 2>/dev/null; then
            ok "yt-dlp $(yt-dlp --version 2>/dev/null)"
        else
            warn "could not update yt-dlp; the packaged version may be stale"
        fi
    else
        warn "no pip, so yt-dlp stays at its packaged version"
    fi
fi

# ------------------------------------------------------------------ #
# 4. Build                                                           #
# ------------------------------------------------------------------ #
if [ "$NO_BUILD" -eq 1 ]; then
    step "Build"
    note "skipped (--no-build)"
elif [ "$RUNTIME_ONLY" -eq 1 ]; then
    step "Build"
    note "skipped: --runtime-only means you already have a build"
elif ! command -v make >/dev/null 2>&1 || ! command -v cc >/dev/null 2>&1; then
    step "Build"
    warn "no make/cc; skipping the Vyzor build"
else
    step "Building Vyzor"
    if [ "$DRY_RUN" -eq 1 ]; then
        note "would run: make (in media-ingest-c/)"
    elif (cd "$ROOT/media-ingest-c" && make -s); then
        ok "built media-ingest-c/build/vyzor"
    else
        fail "the build failed — the packages above are present, so read the errors above"
    fi
fi

# ------------------------------------------------------------------ #
# 5. What you ended up with                                          #
# ------------------------------------------------------------------ #
step "Summary"
check() { # name, command
    if command -v "$2" >/dev/null 2>&1; then ok "$1: $("$2" --version 2>&1 | head -1)"; else warn "$1: not found"; fi
}
check "C compiler" cc
check "make" make
check "pkg-config" pkg-config
check "node" node
check "npm" npm
check "ffmpeg" ffmpeg
check "chromium" chromium
check "yt-dlp" yt-dlp

if command -v pkg-config >/dev/null 2>&1; then
    if pkg-config --exists gstreamer-webrtc-1.0 2>/dev/null; then
        ok "GStreamer webrtc: present (live video can build)"
    else
        warn "GStreamer webrtc: missing (Vyzor will report live video unavailable)"
    fi
fi

if [ -x "$ROOT/media-ingest-c/build/vyzor" ]; then
    ok "Vyzor binary: media-ingest-c/build/vyzor"
fi

printf '\n'
if [ "$DRY_RUN" -eq 1 ]; then
    printf '%sDry run: nothing was changed.%s\n' "$C_BOLD" "$C_RESET"
elif [ "$PARTIAL" -eq 1 ]; then
    printf '%sSome packages could not be installed%s (see the warnings above).\n' "$C_BOLD" "$C_RESET"
    printf 'Mirrors often recover; re-run this script to retry the missing ones.\n\n'
else
    printf '%sNext:%s\n' "$C_BOLD" "$C_RESET"
    printf '  cd media-ingest-c && make test          # the server test suite\n'
    printf '  cd web && npm run typecheck && npm test # the console suite\n'
    printf '  ./media-ingest-c/build/vyzor serve --render --interactive\n'
fi

[ "$PARTIAL" -eq 1 ] && exit 1
exit 0
