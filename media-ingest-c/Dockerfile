# media-ingest — the engine, with the browser it renders with.
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
# 500 MB of proportional set size at its peak. The engine therefore needs a plan
# with at least 2 GB and a render concurrency of one or two — see README. This
# is not a container to run on a 512 MB instance.

# ------------------------------------------------------------------ #
# Build                                                               #
# ------------------------------------------------------------------ #
FROM debian:trixie-slim AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential \
      libcurl4-openssl-dev \
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
      fonts-liberation \
      fonts-dejavu-core \
      fonts-noto-core \
      fonts-noto-color-emoji \
      ca-certificates \
      libcurl4 \
      curl \
      tini \
 && rm -rf /var/lib/apt/lists/*

# A non-root user, because the engine runs a browser on pages it does not
# control and nothing here needs root. Note that Chromium still runs with
# --no-sandbox: an unprivileged container cannot use its namespace sandbox, so
# the isolation is the container and the resource limits, not Chromium's own.
RUN useradd --create-home --uid 10001 --shell /usr/sbin/nologin vyzorix \
 && mkdir -p /data/downloads \
 && chown -R vyzorix:vyzorix /data

COPY --from=build /src/build/media-ingest /usr/local/bin/media-ingest

# The engine writes downloads, rendered-page caches and browser profiles here.
# Mount a volume at /data so they survive a restart and so the size is the
# operator's to choose.
VOLUME ["/data"]

ENV MI_WORK_DIR=/data/downloads \
    MI_RENDER=1 \
    MI_INTERACTIVE=1 \
    MI_HOST=0.0.0.0 \
    MI_PORT=8787

USER vyzorix
WORKDIR /home/vyzorix

# tini reaps orphans. It matters more than usual here: the engine starts a
# browser that forks its own tree, and a container's PID 1 should be something
# that actually reaps rather than the engine itself.
ENTRYPOINT ["/usr/bin/tini", "--", "media-ingest"]

# The defaults are the useful ones for a container: listen on every interface,
# render, and keep the cache beside the downloads. Everything can be overridden
# — `docker run … serve --port 9000 --no-render-cache`, or the MI_* variables.
CMD ["serve", \
     "--host", "0.0.0.0", \
     "--port", "8787", \
     "--work-dir", "/data/downloads", \
     "--render", \
     "--interactive"]

EXPOSE 8787

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
  CMD curl -fsS http://127.0.0.1:8787/healthz || exit 1
