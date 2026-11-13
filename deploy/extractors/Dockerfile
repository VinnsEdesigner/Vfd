# deploy/extractors/Dockerfile — build the `extractors` artifact for a Vyzorix deploy.
#
# WHAT THIS IS
#
# The tools Vyzor shells out to, bundled so the device needs nothing installed:
#
#   ffmpeg, ffprobe   HLS transcode and the mux fallback
#   aria2c            fetch a magnet link (BitTorrent)
#   yt-dlp            resolve a video site to streams
#   gallery-dl        resolve an image host
#   py1337x           resolve a torrent index (the wrapper in scripts/ is the CLI)
#
# WHY THIS IS NOT IN THE ENGINE ARTIFACT
#
# They are large, they go stale, and they are optional: a device without them
# still downloads a direct URL or an HLS manifest. A separate artifact means the
# engine can be updated without re-shipping ~100 MB, and the extractors can be
# updated without touching the engine.
#
# WHY THERE IS NO QEMU, AND NO CROSS COMPILER
#
# The device is arm64. Nothing here is *compiled* or *executed*: ffmpeg is a
# prebuilt static arm64 binary, aria2c comes from Debian's arm64 archive (only
# unpacked, never run), and the Python wheels are resolved by pip for the target
# platform. So the whole build is downloads and file copies.
#
# WHY FFMPEG IS A STATIC BINARY AND NOT DEBIAN'S
#
# Debian's ffmpeg is dynamically linked and its closure is **199 libraries,
# ~200 MB** — opencv, blas, lapack, icu, flite, codec2 and more, almost none of
# which a downloader uses. Worse, several live in subdirectories
# (`blas/`, `lapack/`, `pulseaudio/`) that a flat soname search misses, so the
# closure is easy to get subtly wrong — and a wrong closure is a tool that fails
# only on the device. A static build is one file, ~50 MB, with no closure to get
# wrong. ffprobe is used too (mi_hls.c runs it for a stream's duration), so both
# come from the same static release.
FROM debian:bookworm-slim

ARG DEB_ARCH=arm64
ARG PY_VER=311
# The Debian *multiarch tuple*: `arm64` is the dpkg architecture name,
# `aarch64-linux-gnu` is the directory its libraries land in.
ARG MULTIARCH=aarch64-linux-gnu
# The static ffmpeg release. johnvansickle's builds are the long-standing source
# of static Linux ffmpeg; the `-arm64-static` asset is glibc-static.
ARG FFMPEG_URL=https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-arm64-static.tar.xz
# The matching `strip`, so the two binaries lose ~60 MB of symbols between them.
ARG STRIP=aarch64-linux-gnu-strip

RUN dpkg --add-architecture ${DEB_ARCH} \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl python3 python3-pip dpkg xz-utils \
      binutils binutils-${MULTIARCH}
# `binutils` gives `readelf`, which reads any architecture's ELF headers and so
# resolves the arm64 closure without executing anything; `binutils-${MULTIARCH}`
# gives the matching `strip`. The package lists are deliberately NOT removed:
# the next step installs the arm64 .debs, and a deleted list is why apt could
# not find them.

WORKDIR /work
RUN mkdir -p /out/extractors/bin /out/extractors/lib /debs

# ------------------------------------------------------------------ #
# ffmpeg + ffprobe — static, one file each                            #
# ------------------------------------------------------------------ #
RUN curl -fsSL -o ff.tar.xz "${FFMPEG_URL}" \
 && mkdir -p ff && tar xJf ff.tar.xz -C ff --strip-components=1 \
 && cp ff/ffmpeg ff/ffprobe /out/extractors/bin/ \
 && "${STRIP}" /out/extractors/bin/ffmpeg /out/extractors/bin/ffprobe \
 && rm -rf ff ff.tar.xz
# The version is not printed here: this runs on an amd64 builder, which cannot
# execute an arm64 binary. The architecture is asserted without executing, below.

# ------------------------------------------------------------------ #
# aria2c — Debian's arm64 build, with its closure                     #
#                                                                    #
# `--download-only` resolves the dependency closure from apt and fetches the
# .debs without unpacking or configuring them, so no arm64 code runs. Each is
# then unpacked with `dpkg-deb -x`, which also executes nothing.
# ------------------------------------------------------------------ #
RUN apt-get install -y --download-only --no-install-recommends \
      -o Dir::Cache::archives=/debs aria2:${DEB_ARCH} \
 && mkdir -p /rootfs \
 && for d in /debs/*.deb; do dpkg-deb -x "$d" /rootfs; done \
 && cp /rootfs/usr/bin/aria2c /out/extractors/bin/

# The closure, read from DT_NEEDED with readelf (never `ldd`, which would run
# the binary). The search list includes the subdirectories Debian puts some
# libraries in, so a dependency is not missed just for living in `blas/` etc.
COPY deploy/extractors/libclosure.py /usr/local/bin/libclosure.py
RUN python3 /usr/local/bin/libclosure.py /out/extractors/bin/aria2c /out/extractors/lib \
      /rootfs/usr/lib/${MULTIARCH} /rootfs/lib/${MULTIARCH} \
      /rootfs/usr/lib/${MULTIARCH}/blas /rootfs/usr/lib/${MULTIARCH}/lapack \
 && ls /out/extractors/lib | wc -l

# ------------------------------------------------------------------ #
# The Python extractors, as arm64 wheels                              #
#                                                                    #
# pip resolves these for the target platform, so the extension modules  #
# (`_cffi_backend`, curl_cffi's own .so) are the arm64 ones. py1337x is  #
# pure Python with no wheel, so it is built from its sdist — that build  #
# runs on the host and only writes files.                               #
# ------------------------------------------------------------------ #
COPY scripts/py1337x-json.py /out/extractors/bin/py1337x

RUN python3 -m pip install --quiet --target /out/extractors/py \
      --platform manylinux2014_aarch64 --python-version ${PY_VER} \
      --only-binary=:all: \
      yt-dlp gallery-dl curl_cffi requests beautifulsoup4 \
 && python3 -m pip install --quiet --target /out/extractors/py --no-deps py1337x \
 && chmod 0755 /out/extractors/bin/py1337x

# The console scripts pip wrote carry the build host's interpreter path in their
# shebang. On the device the interpreter is the rootfs's, found on PATH, so each
# shebang is rewritten to `env python3` — otherwise the tool exists and still
# reads as "not found" when the engine execs it.
RUN for f in yt-dlp gallery-dl; do \
      printf '#!/usr/bin/env python3\n' > /out/extractors/bin/$f; \
      tail -n +2 /out/extractors/py/bin/$f >> /out/extractors/bin/$f; \
      chmod 0755 /out/extractors/bin/$f; \
    done \
 && rm -rf /out/extractors/py/bin

# ------------------------------------------------------------------ #
# Assert the architecture before packing                              #
#                                                                    #
# A wrong-arch tool is a failure that appears only on the device, so it is a
# build failure here. `readelf` reads any architecture, so this runs on an amd64
# builder without executing anything.
# ------------------------------------------------------------------ #
RUN set -eu; \
    for b in ffmpeg ffprobe aria2c; do \
      arch=$(readelf -h /out/extractors/bin/$b | awk -F: '/Machine:/ {print $2}' | xargs); \
      [ "$arch" = "AArch64" ] || { echo "$b is $arch, expected AArch64" >&2; exit 1; }; \
    done; \
    for so in /out/extractors/lib/*.so*; do \
      arch=$(readelf -h "$so" | awk -F: '/Machine:/ {print $2}' | xargs); \
      [ "$arch" = "AArch64" ] || { echo "$so is $arch, expected AArch64" >&2; exit 1; }; \
    done; \
    echo "all binaries and libraries are AArch64"

RUN cd /out/extractors \
 && tar --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
        -czf /out/vyzor-extractors.tar.gz . \
 && du -sh /out/extractors/* && ls -lh /out/vyzor-extractors.tar.gz
