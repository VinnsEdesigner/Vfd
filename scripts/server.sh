#!/usr/bin/env bash
#
# server.sh — bring the whole stack up in one command.
#
#   ./scripts/server.sh
#
# Builds the console, starts Vyzor, and serves the console from Vyzor's own
# static webroot so the page and the API share one origin.
#
# WHY THIS EXISTS
#
# The console is a static bundle. Serving it from Vyzor (`--webroot`) means the
# page and the API are the same origin: no CORS to configure, and `127.0.0.1` is
# a secure context, so a service worker and an install prompt work without TLS.
# It also means one process and one port to start, stop and reason about.
#
# This replaces the previous Cloudflare Pages deploy. The console is no longer
# published to a CDN; it ships inside the app and runs beside the API. The deploy
# of both to a device is the Vyzorix manifest (`deploy/vyzorix.toml`).
#
# USAGE
#
#   ./scripts/server.sh                  start Vyzor, build + serve the console
#   ./scripts/server.sh up               the same, explicitly
#   ./scripts/server.sh stop             stop Vyzor
#   ./scripts/server.sh status           what is running, and where
#   ./scripts/server.sh --foreground     run Vyzor attached (Ctrl-C to stop)
#   ./scripts/server.sh --no-console     do not build or serve the console
#   ./scripts/server.sh --port 12001     use the other public port
#   ./scripts/server.sh --help           everything

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

step() { printf '%s==>%s %s\n' "$C_BOLD" "$C_RESET" "$1"; }
ok()   { printf '  %s✓%s %s\n' "$C_OK" "$C_RESET" "$1"; }
warn() { printf '  %s!%s %s\n' "$C_WARN" "$C_RESET" "$1"; }
fail() { printf '  %s✗%s %s\n' "$C_ERR" "$C_RESET" "$1"; }
note() { printf '  %s%s%s\n' "$C_DIM" "$1" "$C_RESET"; }
die()  { fail "$1"; exit 1; }

# ------------------------------------------------------------------ #
# Defaults                                                            #
# ------------------------------------------------------------------ #
COMMAND=""
PORT=12000
WORK_DIR=""
FOREGROUND=0
CONSOLE=1
RENDER=1
INTERACTIVE=1
ALLOW_ORIGIN="*"
LOG_FILE="/tmp/vyzor.log"
PID_FILE="/tmp/vyzor.pid"

usage() {
    sed -n '3,28p' "$0" | sed 's/^# \{0,1\}//'
    cat <<'EOF'

Options:
  --port N             the port to listen on (default 12000)
  --work-dir DIR       where downloads land (default <repo>/downloads)
  --foreground         run Vyzor attached rather than in the background
  --no-console         do not build or serve the console (a pure API)
  --no-render          start Vyzor without the page renderer
  --no-interactive     start Vyzor without live page sessions
  --allow-origin O     CORS origin to allow (default *, i.e. any)
  --log FILE           where the background log goes (default /tmp/vyzor.log)
  -h, --help           this message
EOF
}

# Parse. A value-taking flag is consumed with the word after it, so this is a
# single indexed pass rather than a `for arg in "$@"` (which cannot skip).
ARGS=("$@")
i=0
while [ "$i" -lt "${#ARGS[@]}" ]; do
    arg="${ARGS[$i]}"
    case "$arg" in
        up|start|stop|status)
            [ -z "$COMMAND" ] || die "only one command at a time (got '$COMMAND' and '$arg')"
            COMMAND="$arg" ;;
        --port|--work-dir|--allow-origin|--log)
            [ $((i + 1)) -lt "${#ARGS[@]}" ] || die "$arg needs a value"
            val="${ARGS[$((i + 1))]}"
            i=$((i + 1))
            case "$arg" in
                --port) PORT="$val" ;;
                --work-dir) WORK_DIR="$val" ;;
                --allow-origin) ALLOW_ORIGIN="$val" ;;
                --log) LOG_FILE="$val" ;;
            esac ;;
        --foreground) FOREGROUND=1 ;;
        --no-console) CONSOLE=0 ;;
        --no-render) RENDER=0 ;;
        --no-interactive) INTERACTIVE=0 ;;
        -h|--help) usage; exit 0 ;;
        --*) die "unknown option: $arg (try --help)" ;;
        *) die "unknown argument: $arg (try --help)" ;;
    esac
    i=$((i + 1))
done

[ -z "$COMMAND" ] && COMMAND="up"

case "$PORT" in ''|*[!0-9]*) die "--port must be a number (got '$PORT')" ;; esac

# ------------------------------------------------------------------ #
# Locate the repository                                              #
# ------------------------------------------------------------------ #
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(dirname -- "$SCRIPT_DIR")
[ -d "$ROOT/media-ingest-c" ] || die "no media-ingest-c/ under $ROOT — run this from the repository"
[ -d "$ROOT/web" ] || die "no web/ under $ROOT — run this from the repository"

VYZOR_BIN="$ROOT/media-ingest-c/build/vyzor"
CONSOLE_DIR="$ROOT/web/dist"

# The extractors live in a virtualenv inside the workspace, because the sandbox
# wipes everything outside /workspace on a restart. That venv is not on PATH for
# a process started from here, and Vyzor finds the extractors by name — so it is
# put in front of PATH for the child. Without this the console offers no video or
# gallery mode even though the tools are installed.
TOOLCHAIN="${TOOLCHAIN_DIR:-$ROOT/.toolchain}"
if [ -x "$TOOLCHAIN/venv/bin/yt-dlp" ]; then
    PATH="$TOOLCHAIN/venv/bin:$PATH"
    export PATH
fi
[ -z "$WORK_DIR" ] && WORK_DIR="$ROOT/downloads"

# ------------------------------------------------------------------ #
# Vyzor's public address                                             #
#                                                                     #
# The sandbox publishes ports 12000 and 12001 on hosts derived from   #
# RUNTIME_ID (e.g. work-1-<id>.prod-runtime.all-hands.dev). Deriving  #
# it means a restart does not leave the console pointing at a host    #
# that no longer exists. Any other port is local-only.                #
# ------------------------------------------------------------------ #
public_url_for() { # port
    local id="${RUNTIME_ID:-}"
    [ -n "$id" ] || return 1
    case "$1" in
        12000) printf 'https://work-1-%s.prod-runtime.all-hands.dev' "$id" ;;
        12001) printf 'https://work-2-%s.prod-runtime.all-hands.dev' "$id" ;;
        *) return 1 ;;
    esac
}

PUBLIC_URL=$(public_url_for "$PORT" || true)

# ------------------------------------------------------------------ #
# Helpers                                                            #
# ------------------------------------------------------------------ #
running_pid() {
    # Prefer the pidfile; fall back to a pattern that names this binary and
    # port specifically, so an unrelated process is never matched.
    if [ -f "$PID_FILE" ]; then
        local p; p=$(cat "$PID_FILE" 2>/dev/null)
        if [ -n "$p" ] && pid_alive "$p"; then printf '%s' "$p"; return 0; fi
    fi
    local q
    q=$(pgrep -f "build/vyzor serve .*--port $PORT" 2>/dev/null | head -1)
    if [ -n "$q" ] && pid_alive "$q"; then printf '%s' "$q"; return 0; fi
    return 1
}

# Is a pid running, as opposed to merely existing?
#
# `kill -0` is not enough: a child that has exited but not yet been reaped is a
# zombie, and `kill -0` succeeds on a zombie because the pid is still taken.
# That made "stop" report a process as still up (and then SIGKILL it) when it
# had in fact already gone. Checking the state is what distinguishes the two.
pid_alive() {
    local state
    state=$(ps -o stat= -p "$1" 2>/dev/null | tr -d ' ')
    [ -n "$state" ] || return 1
    case "$state" in Z*) return 1 ;; esac
    return 0
}

health_ok() { curl -fsS -m 5 "http://127.0.0.1:$1/healthz" >/dev/null 2>&1; }

wait_for_health() { # port, seconds
    local i
    for i in $(seq 1 "$2"); do
        if health_ok "$1"; then return 0; fi
        sleep 1
    done
    return 1
}

# ------------------------------------------------------------------ #
# stop                                                               #
# ------------------------------------------------------------------ #
do_stop() {
    step "Stopping Vyzor"
    local p
    p=$(running_pid || true)
    if [ -z "$p" ]; then
        ok "not running"
        rm -f "$PID_FILE"
        return 0
    fi
    # SIGTERM first: Vyzor handles it (it sets its stop flag and shuts the
    # listening socket down), and in practice it exits in well under a second.
    kill -TERM "$p" 2>/dev/null || true
    local i
    for i in $(seq 1 20); do
        pid_alive "$p" || break
        sleep 0.25
    done
    if pid_alive "$p"; then
        warn "still up after 5s; sending SIGKILL"
        kill -9 "$p" 2>/dev/null || true
        sleep 0.5
    fi
    rm -f "$PID_FILE"
    ok "stopped (pid $p)"
}

# ------------------------------------------------------------------ #
# status                                                             #
# ------------------------------------------------------------------ #
do_status() {
    step "Vyzor"
    local p
    p=$(running_pid || true)
    if [ -z "$p" ]; then
        warn "not running"
    else
        ok "running (pid $p), listening on port $PORT"
        if health_ok "$PORT"; then
            local caps
            caps=$(curl -fsS -m 5 "http://127.0.0.1:$PORT/api/v1/capabilities" 2>/dev/null)
            if [ -n "$caps" ]; then
                printf '%s' "$caps" | python3 -c "
import json,sys
try: d=json.load(sys.stdin)
except Exception: sys.exit(0)
print('    version:  ', d.get('vyzor_version'))
print('    ffmpeg:   ', d.get('ffmpeg'))
print('    render:   ', (d.get('render') or {}).get('available'))
print('    live page:', (d.get('interactive') or {}).get('available'))
" 2>/dev/null || true
            fi
        else
            warn "process is up but /healthz does not answer"
        fi
    fi

    step "Addresses"
    note "local:  http://127.0.0.1:$PORT"
    if [ -n "$PUBLIC_URL" ]; then
        note "public: $PUBLIC_URL"
        if curl -fsS -m 8 "$PUBLIC_URL/healthz" >/dev/null 2>&1; then
            ok "public address answers"
        else
            warn "public address does not answer yet (the proxy can lag a few seconds)"
        fi
    else
        note "public: none for port $PORT (the sandbox publishes 12000 and 12001)"
    fi

    step "Console"
    if [ -f "$CONSOLE_DIR/index.html" ]; then
        ok "built at web/dist (served same-origin by Vyzor)"
        if curl -fsS -m 5 "http://127.0.0.1:$PORT/" 2>/dev/null | grep -q '<div id="root">'; then
            ok "served at the root"
        elif [ -n "$p" ]; then
            warn "Vyzor is up but the root does not serve the console (was it started with --webroot?)"
        fi
    else
        warn "not built (run: $0, or $0 --no-console to run the API alone)"
    fi
    note "log: $LOG_FILE"
}

# ------------------------------------------------------------------ #
# up                                                                 #
# ------------------------------------------------------------------ #
do_up() {
    [ -x "$VYZOR_BIN" ] || die "Vyzor is not built. Run ./scripts/install-toolchain.sh first."
    mkdir -p "$WORK_DIR"

    # A second instance on the same port would fail to bind, and the failure
    # would be confusing; replace the old one deliberately instead.
    local existing
    existing=$(running_pid || true)
    if [ -n "$existing" ]; then
        warn "Vyzor is already running (pid $existing); restarting it"
        do_stop >/dev/null
    fi

    # ------------------------------------------------------------------
    # The console
    #
    # Built same-origin: no VITE_API_BASE_URL is set, so the bundle targets
    # `location.origin` — the very origin Vyzor serves it from. An empty string
    # would NOT do: the connection store treats it as a set value, not a
    # fallback, and every request would go to an empty base.
    # ------------------------------------------------------------------
    local webroot=""
    if [ "$CONSOLE" -eq 1 ]; then
        step "Building the console"
        if ! command -v npm >/dev/null 2>&1; then
            warn "npm is not installed; skipping (run ./scripts/install-toolchain.sh)"
        elif [ ! -d "$ROOT/web/node_modules" ]; then
            warn "web/node_modules is missing; skipping (run ./scripts/install-toolchain.sh)"
        else
            note "VITE_API_BASE_URL unset -> the bundle targets its own origin"
            if (cd "$ROOT/web" && env -u VITE_API_BASE_URL npm run build >/tmp/vfd-build.log 2>&1); then
                ok "built web/dist"
                webroot="$CONSOLE_DIR"
            else
                fail "the console build failed — see /tmp/vfd-build.log"
                tail -5 /tmp/vfd-build.log | sed 's/^/    /'
                exit 1
            fi
        fi
    else
        step "The console"
        note "skipped (--no-console): Vyzor runs as a pure API"
    fi

    # Build the argument list. Only add feature flags that were asked for, so
    # the command in the log is what was actually run.
    local args=(serve --host 0.0.0.0 --port "$PORT" --work-dir "$WORK_DIR"
                --render-concurrent=1 --interactive-concurrent=1)
    [ "$RENDER" -eq 1 ] && args+=(--render)
    [ "$INTERACTIVE" -eq 1 ] && args+=(--interactive)
    args+=(--allow-origin "$ALLOW_ORIGIN")
    [ -n "$webroot" ] && args+=(--webroot "$webroot")

    step "Starting Vyzor"
    note "$VYZOR_BIN ${args[*]}"

    if [ "$FOREGROUND" -eq 1 ]; then
        note "running attached; Ctrl-C to stop"
        exec "$VYZOR_BIN" "${args[@]}"
    fi

    nohup "$VYZOR_BIN" "${args[@]}" >"$LOG_FILE" 2>&1 &
    local pid=$!
    printf '%s' "$pid" > "$PID_FILE"

    if wait_for_health "$PORT" 15; then
        ok "up (pid $pid) on port $PORT"
    else
        fail "did not answer /healthz within 15s — last lines of $LOG_FILE:"
        tail -5 "$LOG_FILE" | sed 's/^/    /'
        die "Vyzor failed to start"
    fi

    # Say what it can actually do, rather than assuming the flags took effect.
    local caps
    caps=$(curl -fsS -m 5 "http://127.0.0.1:$PORT/api/v1/capabilities" 2>/dev/null || true)
    if [ -n "$caps" ]; then
        printf '%s' "$caps" | python3 -c "
import json,sys
try: d=json.load(sys.stdin)
except Exception: sys.exit(0)
r=(d.get('render') or {}).get('available')
i=(d.get('interactive') or {}).get('available')
f=d.get('ffmpeg')
print('  render:', 'available' if r else 'unavailable',
      '| live page:', 'available' if i else 'unavailable',
      '| ffmpeg:', 'yes' if f else 'no')
" 2>/dev/null || true
    fi

    # ------------------------------------------------------------------
    # Verify the console is served
    # ------------------------------------------------------------------
    if [ -n "$webroot" ]; then
        step "Console"
        if curl -fsS -m 5 "http://127.0.0.1:$PORT/" 2>/dev/null | grep -q '<div id="root">'; then
            ok "served same-origin at http://127.0.0.1:$PORT/"
        else
            warn "the root did not return the console bundle; check $LOG_FILE"
        fi
    fi

    # ------------------------------------------------------------------
    # Report
    # ------------------------------------------------------------------
    printf '\n'
    printf '%sRunning:%s\n' "$C_BOLD" "$C_RESET"
    printf '  Vyzor    http://127.0.0.1:%s\n' "$PORT"
    [ -n "$PUBLIC_URL" ] && printf '           %s\n' "$PUBLIC_URL"
    if [ -n "$webroot" ]; then
        printf '  Console  same origin (served by Vyzor from %s)\n' "$webroot"
    else
        printf '  Console  not served (--no-console)\n'
    fi
    printf '  Log      %s\n' "$LOG_FILE"
    printf '\n'
    printf '%sStop with:%s ./scripts/server.sh stop\n' "$C_DIM" "$C_RESET"
}

# ------------------------------------------------------------------ #
# Dispatch                                                           #
# ------------------------------------------------------------------ #
case "$COMMAND" in
    up|start) do_up ;;
    stop)     do_stop ;;
    status)   do_status ;;
    *) die "unknown command: $COMMAND" ;;
esac
