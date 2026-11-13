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
# The extractors are their own artifact, unpacked under extractors/.
EXTRACTORS_DIR="$APP_DIR/extractors"

# The bundled tools take precedence over the rootfs's. The engine resolves each by
# name on PATH (mi_extract.c, mi_download.c), so putting the bundle's bin/ first is
# all that is needed for ffmpeg, ffprobe, aria2c, yt-dlp, gallery-dl and py1337x.
#
# They are optional: a device without this artifact still downloads a direct URL or
# an HLS manifest, and the engine reports the capability it actually has rather
# than failing. When present, the video and torrent modes light up.
if [ -d "$EXTRACTORS_DIR/bin" ]; then
    PATH="$EXTRACTORS_DIR/bin:$PATH"
    export PATH
fi

# The Python extractors are a directory of modules, not an installed package, so
# they are found by PYTHONPATH rather than by a site-packages install. The console
# scripts pip wrote (`yt-dlp`, `gallery-dl`) import from here.
if [ -d "$EXTRACTORS_DIR/py" ]; then
    PYTHONPATH="$EXTRACTORS_DIR/py${PYTHONPATH:+:$PYTHONPATH}"
    export PYTHONPATH
fi

# The bundled libraries take precedence over the rootfs's. The bundle is built on
# the same Debian suite the rootfs is, so these are consistent; LD_LIBRARY_PATH
# just makes the process find them without an ldconfig the rootfs may not run.
for libdir in "$ENGINE_DIR/lib" "$EXTRACTORS_DIR/lib"; do
    [ -d "$libdir" ] || continue
    LD_LIBRARY_PATH="$libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
done
if [ -n "${LD_LIBRARY_PATH:-}" ]; then
    export LD_LIBRARY_PATH
fi

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
