# deploy/bundle/Dockerfile — build the `engine` artifact for a Vyzorix deploy.
#
# WHAT THIS PRODUCES
#
#   vyzor-engine.tar.gz   the app's `engine` artifact. Unpacks at
#                         /home/apps/vfd/engine/ and contains:
#                           run.sh        the manifest's `cmd`
#                           bin/vyzor     the media engine
#                           lib/          every shared library vyzor needs
#
# WHY A CONTAINER, AND NOT A CROSS-COMPILER
#
# The engine links libcurl, sqlite3 and GStreamer. Cross-compiling it means a
# cross libcurl, a cross sqlite3 and a cross GStreamer (with its own plugins) —
# three toolchains to keep in step, for a program that builds in seconds. A
# Debian container of the target architecture has all three, so the build is the
# same `make` as on the host, and the libraries it links against are the ones it
# ships.
#
# THE ARCHITECTURE IS THE IMAGE'S
#
# The build and bundle stages are both the target architecture (`--platform`
# picks it), so nothing is emulated at run time and the copied libraries are the
# right ones. A CI runner builds both ABIs with buildx + qemu.
#
# WHY THE LIBRARIES ARE BUNDLED
#
# The device's rootfs is Debian 12 (glibc 2.36) with the core userland and no
# GStreamer. Rather than assume the rootfs provides libcurl/sqlite/GStreamer, the
# bundle carries the exact libraries it was built against and points the engine
# at them with LD_LIBRARY_PATH (see run.sh). The glibc core is left out — it is
# the one thing the rootfs must provide, and it does.

# ------------------------------------------------------------------ #
# Build                                                              #
# ------------------------------------------------------------------ #
FROM debian:bookworm-slim AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential pkg-config \
      libcurl4-openssl-dev libsqlite3-dev zlib1g-dev \
      libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
      libgstreamer-plugins-bad1.0-dev \
      ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY media-ingest-c/Makefile ./
COPY media-ingest-c/src ./src
RUN make

# The library closure is computed HERE, where every dependency is present, and
# not in the bundle stage: there, a missing library makes `ldd` print "not found"
# and the grep silently drops it — an incomplete bundle that only fails on the
# device. Every shared library the engine needs is copied except the glibc core,
# which is the one thing the device's rootfs must provide (and does).
#
# `cp -L` follows the soname symlinks to real files, and the original path is
# preserved under lib/ so a soname lookup finds the right file.
RUN mkdir -p /out/engine/lib \
 && ldd build/vyzor \
      | grep -oE '/[^ ]+\.so[^ ]*' \
      | sort -u \
      | grep -vE '/(libc|libm|libpthread|libdl|librt|ld-linux)[.-]' \
      | while read -r so; do cp -L "$so" /out/engine/lib/; done \
 && cp build/vyzor /out/engine/bin_vyzor

# ------------------------------------------------------------------ #
# Assemble                                                           #
# ------------------------------------------------------------------ #
# This stage only writes files; it never runs the engine, so it needs no
# libraries of its own — just `tar`, which the base image provides.
FROM debian:bookworm-slim AS bundle

WORKDIR /out/engine
COPY --from=build /out/engine/bin_vyzor bin/vyzor
COPY --from=build /out/engine/lib lib
COPY deploy/app/run.sh run.sh
RUN chmod 0755 run.sh bin/vyzor \
 && ls lib | wc -l

RUN tar -czf /out/vyzor-engine.tar.gz -C /out/engine .
