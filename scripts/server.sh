#!/usr/bin/env bash
#
# server.sh — bring the whole stack up in one command.
#
#   ./scripts/server.sh
#
# Starts Vyzor, builds the console pointed at Vyzor's public address, deploys
# the console to Cloudflare Pages, and verifies both ends answer.
#
# WHY THIS EXISTS
#
# The console is a static bundle with Vyzor's URL baked in at build time, and
# this sandbox's public host is per-sandbox. So after a restart the old bundle
# points at a host that no longer exists, and Vyzor is not running. This does
# the three steps that have to happen together — start, point, publish — so
# they cannot drift apart.
#
# USAGE
#
#   ./scripts/server.sh                  start Vyzor, build + deploy the console
#   ./scripts/server.sh up               the same, explicitly
#   ./scripts/server.sh stop             stop Vyzor
#   ./scripts/server.sh status           what is running, and where
#   ./scripts/server.sh --foreground     run Vyzor attached (Ctrl-C to stop)
#   ./scripts/server.sh --no-deploy      build the console, do not publish it
#   ./scripts/server.sh --local          build the console for localhost:PORT
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
DEPLOY=1
LOCAL_URL=0
RENDER=1
INTERACTIVE=1
ALLOW_ORIGIN="*"
PAGES_PROJECT="${PAGES_PROJECT:-vfd}"
LOG_FILE="/tmp/vyzor.log"
PID_FILE="/tmp/vyzor.pid"

usage() {
    sed -n '3,27p' "$0" | sed 's/^# \{0,1\}//'
    cat <<'EOF'

Options:
  --port N             the port to listen on (default 12000)
  --work-dir DIR       where downloads land (default <repo>/downloads)
  --foreground         run Vyzor attached rather than in the background
  --no-deploy          build the console but do not publish it
  --local              build the console for http://localhost:PORT
  --no-render          start Vyzor without the page renderer
  --no-interactive     start Vyzor without live page sessions
  --allow-origin O     CORS origin to allow (default *, i.e. any)
  --log FILE           where the background log goes (default /tmp/vyzor.log)
  -h, --help           this message

Environment:
  PAGES_PROJECT        Cloudflare Pages project (default vfd)
  CLOUDFLARE_ACCOUNT_ID, CLOUDFLARE_API_TOKEN   needed only to deploy
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
        --no-deploy) DEPLOY=0 ;;
        --local) LOCAL_URL=1 ;;
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

# The console's public URL is the Pages project's subdomain, which is NOT
# always the project name: a Pages subdomain is assigned at creation and does
# not move when the project is renamed. So it is looked up, never assumed —
# `vfd.pages.dev` is a different site entirely, and reporting it would send
# someone to a stranger's page.
pages_subdomain() { # account, project
    [ -n "$1" ] || return 1
    curl -fsS -m 10 \
        -H "Authorization: Bearer ${CLOUDFLARE_API_KEY:-${CLOUDFLARE_API_TOKEN:-}}" \
        "https://api.cloudflare.com/client/v4/accounts/$1/pages/projects/$2" 2>/dev/null \
        | python3 -c "
import json,sys
try: d=json.load(sys.stdin)
except Exception: sys.exit(1)
r=d.get('result') or {}
s=r.get('subdomain') or ''
print(s)
sys.exit(0 if s else 1)
" 2>/dev/null
}

# The Cloudflare account id, discovered from the token when it is not set.
cloudflare_account() {
    [ -n "${CLOUDFLARE_ACCOUNT_ID:-}" ] && { printf '%s' "$CLOUDFLARE_ACCOUNT_ID"; return 0; }
    curl -fsS -m 10 \
        -H "Authorization: Bearer ${CLOUDFLARE_API_KEY:-${CLOUDFLARE_API_TOKEN:-}}" \
        "https://api.cloudflare.com/client/v4/accounts" 2>/dev/null \
        | python3 -c "
import json,sys
try: d=json.load(sys.stdin)
except Exception: sys.exit(1)
r=d.get('result') or []
print(r[0]['id'] if r else '')
" 2>/dev/null
}

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
    if [ -f "$ROOT/web/dist/index.html" ]; then
        local baked
        # Look specifically for the sandbox's public hosts, not just any URL in
        # the bundle — a React comment contains reactjs.org and matched first.
        baked=$(grep -ho 'https://work-[12]-[A-Za-z0-9]*\.prod-runtime\.all-hands\.dev\|http://localhost:[0-9]*' \
                "$ROOT/web/dist/assets/"index-*.js 2>/dev/null | head -1)
        if [ -n "$baked" ]; then
            ok "built (points at $baked)"
        else
            ok "built (no baked API URL; the console will ask in Settings)"
        fi
    else
        warn "not built (run: $0 --no-deploy)"
    fi
    if [ -n "${CLOUDFLARE_API_KEY:-}${CLOUDFLARE_API_TOKEN:-}" ]; then
        local acct sub
        acct=$(cloudflare_account)
        sub=$(pages_subdomain "$acct" "$PAGES_PROJECT" || true)
        [ -n "$sub" ] && note "published at https://$sub"
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

    # Build the argument list. Only add feature flags that were asked for, so
    # the command in the log is what was actually run.
    local args=(serve --host 0.0.0.0 --port "$PORT" --work-dir "$WORK_DIR"
                --render-concurrent=1 --interactive-concurrent=1)
    [ "$RENDER" -eq 1 ] && args+=(--render)
    [ "$INTERACTIVE" -eq 1 ] && args+=(--interactive)
    args+=(--allow-origin "$ALLOW_ORIGIN")

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
    # The console
    # ------------------------------------------------------------------
    local base_url="$PUBLIC_URL"
    if [ "$LOCAL_URL" -eq 1 ]; then base_url="http://localhost:$PORT"; fi
    if [ -z "$base_url" ]; then
        warn "no public address for port $PORT; building the console for http://localhost:$PORT"
        base_url="http://localhost:$PORT"
    fi

    step "Building the console"
    if ! command -v npm >/dev/null 2>&1; then
        warn "npm is not installed; skipping (run ./scripts/install-toolchain.sh)"
    elif [ ! -d "$ROOT/web/node_modules" ]; then
        warn "web/node_modules is missing; skipping (run ./scripts/install-toolchain.sh)"
    else
        note "VITE_API_BASE_URL=$base_url"
        if (cd "$ROOT/web" && VITE_API_BASE_URL="$base_url" npm run build >/tmp/vfd-build.log 2>&1); then
            ok "built web/dist"
        else
            fail "the console build failed — see /tmp/vfd-build.log"
            tail -5 /tmp/vfd-build.log | sed 's/^/    /'
            exit 1
        fi
    fi

    # ------------------------------------------------------------------
    # Publish
    # ------------------------------------------------------------------
    if [ "$DEPLOY" -eq 0 ]; then
        step "Publishing"
        note "skipped (--no-deploy)"
    elif [ ! -f "$ROOT/web/dist/index.html" ]; then
        step "Publishing"
        warn "nothing built to publish"
    elif [ -z "${CLOUDFLARE_API_KEY:-}" ] && [ -z "${CLOUDFLARE_API_TOKEN:-}" ]; then
        step "Publishing"
        warn "no CLOUDFLARE_API_KEY or CLOUDFLARE_API_TOKEN set; skipping"
        note "the console is built at web/dist — serve it yourself, or set a token"
    else
        step "Publishing to Cloudflare Pages"
        local acct
        acct=$(cloudflare_account)
        if [ -z "$acct" ]; then
            warn "could not determine the Cloudflare account id; skipping"
            note "set CLOUDFLARE_ACCOUNT_ID to deploy"
        else
            note "project: $PAGES_PROJECT (account $acct)"
            local out
            if out=$(cd "$ROOT/web" && CLOUDFLARE_ACCOUNT_ID="$acct" \
                     CLOUDFLARE_API_TOKEN="${CLOUDFLARE_API_KEY:-$CLOUDFLARE_API_TOKEN}" \
                     npx --yes wrangler@3.90.0 pages deploy dist \
                     --project-name="$PAGES_PROJECT" --branch=main 2>&1); then
                ok "published"
            else
                fail "the deploy failed:"
                printf '%s\n' "$out" | tail -6 | sed 's/^/    /'
                exit 1
            fi
        fi
    fi

    # ------------------------------------------------------------------
    # Report
    # ------------------------------------------------------------------
    local console_url=""
    if [ -n "${CLOUDFLARE_API_KEY:-}${CLOUDFLARE_API_TOKEN:-}" ]; then
        local acct2
        acct2=$(cloudflare_account)
        console_url=$(pages_subdomain "$acct2" "$PAGES_PROJECT" || true)
    fi

    printf '\n'
    printf '%sRunning:%s\n' "$C_BOLD" "$C_RESET"
    printf '  Vyzor    http://127.0.0.1:%s\n' "$PORT"
    [ -n "$PUBLIC_URL" ] && printf '           %s\n' "$PUBLIC_URL"
    if [ -n "$console_url" ]; then
        printf '  Console  https://%s\n' "$console_url"
    elif [ "$DEPLOY" -eq 1 ]; then
        printf '  Console  deployed to the "%s" Pages project\n' "$PAGES_PROJECT"
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
