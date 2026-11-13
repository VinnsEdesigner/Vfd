#!/bin/sh
#
# run.sh — the app's entrypoint inside the device container.
#
# The manifest's `cmd` points here (`/home/apps/vfd/engine/run.sh`). The device
# executes it without a shell, argv split on whitespace, so this file exists to
# do the two things a bare command cannot: locate the app's own directory and
# point the engine at the console that shipped beside it.
#
# The layout after the device installs both artifacts:
#
#   /home/apps/vfd/
#     engine/            the `engine` artifact (this file, bin/vyzor, lib/)
#       run.sh           <- cmd
#       bin/vyzor
#       lib/             every shared library vyzor needs, so the device's
#                        minimal rootfs does not have to provide them
#     console/www/       the `console` artifact (kind = webroot)
#     data/              downloads and state, created on first run
#
# Nothing here is secret and nothing needs a shell, which is the point: `cmd` is
# a code-execution surface, and this is the smallest one that works.

set -e

ENGINE_DIR=$(cd "$(dirname "$0")" && pwd)
APP_DIR=$(dirname "$ENGINE_DIR")

# The console is a webroot artifact, so it unpacks under console/www.
CONSOLE_DIR="$APP_DIR/console/www"
WORK_DIR="$APP_DIR/data"

# The bundled libraries take precedence over the rootfs's. The bundle is built on
# the same Debian suite the rootfs is, so these are consistent; LD_LIBRARY_PATH
# just makes the process find them without an ldconfig the rootfs may not run.
if [ -d "$ENGINE_DIR/lib" ]; then
    LD_LIBRARY_PATH="$ENGINE_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LD_LIBRARY_PATH
fi

# The extractors (yt-dlp, gallery-dl, py1337x, aria2, ffmpeg) are not bundled:
# they are large, they go stale, and a device that lacks them still downloads and
# transcodes what a direct URL or HLS manifest gives it. When they are present on
# PATH, the console offers video and gallery modes; when they are not, it offers
# what the engine can do alone. No flag is needed — the engine resolves each by
# name and reports the capability it actually has.
mkdir -p "$WORK_DIR"

# Feature flags come from the environment the manifest sets, so the deployment's
# capabilities are visible in the manifest rather than buried here. A flag is
# added only for a truthy value: the engine enables a feature when the variable
# is merely *present* (`getenv(...) != NULL`), so `MI_RENDER=0` would otherwise
# turn rendering ON. Treating "0"/"false" as off here keeps the manifest honest.
set -- serve \
    --host "${MI_HOST:-127.0.0.1}" \
    --port "${MI_PORT:-8787}" \
    --work-dir "$WORK_DIR" \
    --allow-origin "${MI_ALLOW_ORIGINS:-*}"

[ -d "$CONSOLE_DIR" ] && set -- "$@" --webroot "$CONSOLE_DIR"

truthy() { case "${1:-}" in 1|true|yes|on) return 0 ;; *) return 1 ;; esac; }
truthy "$MI_RENDER"      && set -- "$@" --render
truthy "$MI_INTERACTIVE" && set -- "$@" --interactive

exec "$ENGINE_DIR/bin/vyzor" "$@"
