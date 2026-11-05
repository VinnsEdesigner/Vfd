# vyzor — Vyzor, with the browser it renders with and streams.
#
# WHY THIS IS NOT A SCRATCH IMAGE
#
# Rendering a page means running a real browser, so the image carries Chromium
# and the fonts a page needs. Without the fonts, every page renders as boxes —
# measured: a bare image has eight font families, which is not enough for a real
# site. Without Chromium the renderer reports itself unavailable and the console
# hides the control, which is a graceful degradation rather than a crash, but it
# is not what anyone wants in production.
#
# SIZE AND MEMORY
#
# Chromium makes this image large (roughly 600 MB) and a render costs about
# 500 MB of proportional set size at its peak. Vyzor therefore needs a plan
# with at least 2 GB and a render concurrency of one or two — see README. This
# is not a container to run on a 512 MB instance.
#
# THE LIVE-VIDEO PATH ADDS TWO THINGS
#
# A live-video session captures the page as raw video, which needs a display to
# capture from and a media stack to encode it:
#
#   Xvfb            a virtual X server. Chromium is NOT run headless here,
#                   because headless composites internally and never touches X,
#                   so there would be nothing for the capture to read.
#   GStreamer       ximagesrc -> vp8enc -> rtpvp8pay -> webrtcbin. `libnice`
#                   (in the -bad set) is what gives webrtcbin ICE; without it
#                   webrtcbin exists but cannot connect, and the failure is a
#                   null pad at runtime rather than a missing package.
#
# Both are optional at runtime: if they are absent Vyzor reports the
# capability unavailable and the console hides the control, rather than
# accepting a session it cannot serve.

# ------------------------------------------------------------------ #
# Build                                                               #
# ------------------------------------------------------------------ #
FROM debian:trixie-slim AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential \
      pkg-config \
      libcurl4-openssl-dev \
      libgstreamer1.0-dev \
      libgstreamer-plugins-base1.0-dev \
      libgstreamer-plugins-bad1.0-dev \
      ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Makefile ./
COPY src ./src
RUN make

# ------------------------------------------------------------------ #
# Run                                                                 #
# ------------------------------------------------------------------ #
FROM debian:trixie-slim

# Chromium and the things it needs to lay a page out.
#
# `chromium` pulls in the libraries; the font packages are what stop text from
# rendering as boxes; `fonts-noto-color-emoji` is what stops emoji from doing
# the same. `curl` is here only for the healthcheck below.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      chromium \
      xvfb \
      fonts-liberation \
      fonts-dejavu-core \
      fonts-noto-core \
      fonts-noto-color-emoji \
      ca-certificates \
      libcurl4 \
      curl \
      tini \
      gstreamer1.0-tools \
      gstreamer1.0-plugins-base \
      gstreamer1.0-plugins-good \
      gstreamer1.0-plugins-bad \
      gstreamer1.0-plugins-ugly \
      gstreamer1.0-libav \
      gstreamer1.0-nice \
 && rm -rf /var/lib/apt/lists/*

# A non-root user, because Vyzor runs a browser on pages it does not
# control and nothing here needs root. Note that Chromium still runs with
# --no-sandbox: an unprivileged container cannot use its namespace sandbox, so
# the isolation is the container and the resource limits, not Chromium's own.
RUN useradd --create-home --uid 10001 --shell /usr/sbin/nologin vyzorix \
 && mkdir -p /data/downloads \
 && chown -R vyzorix:vyzorix /data

COPY --from=build /src/build/vyzor /usr/local/bin/vyzor

# Vyzor writes downloads, rendered-page caches and browser profiles here.
# Mount a volume at /data so they survive a restart and so the size is the
# operator's to choose.
VOLUME ["/data"]

ENV MI_WORK_DIR=/data/downloads \
    MI_RENDER=1 \
    MI_INTERACTIVE=1 \
    MI_RTC=1 \
    MI_HOST=0.0.0.0 \
    MI_PORT=8787

USER vyzorix
WORKDIR /home/vyzorix

# tini reaps orphans. It matters more than usual here: Vyzor starts a
# browser that forks its own tree, and a container's PID 1 should be something
# that actually reaps rather than Vyzor itself.
ENTRYPOINT ["/usr/bin/tini", "--", "vyzor"]

# The defaults are the useful ones for a container: listen on every interface,
# render, and keep the cache beside the downloads. Everything can be overridden
# — `docker run … serve --port 9000 --no-render-cache`, or the MI_* variables.
CMD ["serve", \
     "--host", "0.0.0.0", \
     "--port", "8787", \
     "--work-dir", "/data/downloads", \
     "--render", \
     "--interactive", \
     "--rtc"]

EXPOSE 8787

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
  CMD curl -fsS http://127.0.0.1:8787/healthz || exit 1
