# Configuration

Vyzor is configured by flags on `vyzor serve` and by `MI_*` environment
variables. A flag wins where both are given. The environment is how a container
is configured; the flags are how a shell is.

## Where to listen, and where files go

| Flag | Env | Default | Meaning |
|---|---|---|---|
| `--host H` | `MI_HOST` | `127.0.0.1` | bind address |
| `--port N` | `MI_PORT` | `8787` | bind port |
| `--work-dir D` | `MI_WORK_DIR` | `./downloads` | downloads and the render cache |
| `--auth-token T` | `MI_AUTH_TOKEN` | none | bearer token required on every route but `/healthz` |
| `--allow-origin O` | `MI_ALLOW_ORIGINS` | none (same-origin) | comma-separated CORS origins |
| `--user-agent UA` | `MI_USER_AGENT` | Vyzor's own | the User-Agent to send |

`--allow-origin` defaults to **same-origin only**. A server that answers every
origin lets any page a user visits read their library and start downloads, so
allowing an origin is something an operator opts into. Use `*` only on a host you
would expose anyway.

## Egress and identity

| Flag | Env | Meaning |
|---|---|---|
| `--proxy URL` | `MI_PROXY` | egress proxy, e.g. `http://host:port` or `socks5://host:port` |
| `--impersonate CLIENT` | `MI_IMPERSONATE` | curl_cffi impersonation target, e.g. `chrome` |
| `--identity-retries N` | `MI_IDENTITY_RETRIES` | how many identities to try |
| — | `MI_IMPERSONATE_HELPER` | helper binary for impersonation |
| — | `MI_COOKIES_FILE` | a path to a Netscape cookie file (a path, not the contents) |

`MI_COOKIES_FILE` is a path so the credential lives in a file the operator
controls the permissions on, rather than in the process environment where `ps e`
or a core dump can expose it.

## Crawl and politeness

| Env | Default | Meaning |
|---|---|---|
| `MI_OBEY_ROBOTS` | `1` | honour robots.txt (a crawl does; an explicit fetch does not) |
| `MI_CRAWL_INTERVAL` | `1.0` | seconds between requests to one host |
| `MI_FRONTIER_DB` | none | local SQLite file for the shared crawl frontier |
| `MI_FRONTIER_DB_URL` | none | Turso URL for a shared frontier |
| `MI_FRONTIER_TOKEN` | none | Turso token |

## Render

| Flag | Env | Default | Meaning |
|---|---|---|---|
| `--render` | `MI_RENDER` | off | enable page rendering |
| `--render-timeout S` | | 20 | per-render wall clock |
| `--render-concurrent N` | | 1 | render slots |
| `--render-cache DIR` | | under the work dir | cache location |
| `--render-cache-ttl S` | | built-in | cache entry TTL |
| `--no-render-cache` | | — | turn the cache off |

`MI_RENDER` enables the feature by its **presence** — any value, including `0`,
turns it on. Use the flag, or omit the variable, to leave it off.

## Live pages

| Flag | Env | Default | Meaning |
|---|---|---|---|
| `--interactive` | `MI_INTERACTIVE` | off | enable live page sessions |
| `--interactive-concurrent N` | | 1 | session slots |
| `--interactive-idle S` | | 300 | close a session nobody watches |
| `--interactive-keepalive S` | | built-in | how often to ping an idle socket |
| `--interactive-silence S` | | built-in | drop a client silent for this long |

`MI_INTERACTIVE` enables the feature by its presence, like `MI_RENDER`.

## Live video

| Flag | Env | Default | Meaning |
|---|---|---|---|
| `--rtc` | `MI_RTC` | off | enable live video over WebRTC |
| `--rtc-concurrent N` | | 1 | session slots |
| `--rtc-fps N` | | 25 | capture frame rate |
| `--rtc-bitrate KBPS` | | 2500 | encoder bitrate |
| `--rtc-stun URL` | | none | STUN server for ICE |

`MI_RTC` enables the feature by its presence, like `MI_RENDER`. Without a STUN
server only host candidates are produced, which works on a LAN and not across a
NAT.

## Captcha

| Env | Meaning |
|---|---|
| `MI_CAPTCHA_PROVIDER` | `stub`, `2captcha`, `anti-captcha` or `capsolver` |
| `MI_CAPTCHA_KEY` | the provider's API key |
| `MI_CAPTCHA_BASE_URL` | override the provider's base URL |

`stub` is for testing and solves nothing.

## Storage and CDN

| Env | Meaning |
|---|---|
| `MI_S3_ACCESS_KEY` | S3 access key id |
| `MI_S3_SECRET_KEY` | S3 secret access key |
| `MI_S3_SESSION_TOKEN` | optional STS token |
| `MI_S3_ENDPOINT` | endpoint URL |
| `MI_S3_REGION` | region (`auto` for R2) |
| `MI_S3_BUCKET` | target bucket |
| `MI_CF_API_TOKEN` | Cloudflare API token, for the cache purge |
| `MI_CF_ZONE_ID` | Cloudflare zone id |

Storage credentials can also be supplied per request as headers; that is how the
console does it, so a credential is never written to a config file. See
[media-pipeline.md](media-pipeline.md).

## Extractors

Vyzor finds `yt-dlp` (video) and `gallery-dl` (images) **by name on `PATH`**;
there is no flag for the path. Whichever binary is first on `PATH` is the one
used, which is why a run started by `server.sh` puts the workspace venv in front
of `PATH` before exec'ing Vyzor.

This matters more than it looks. A distribution's copy can lag a site's changes
by months, and this is the one component whose whole job is to keep up with them.
A stale extractor is the single most common cause of a "page could not be
resolved" failure — and it fails fast (exit 2, no output) rather than timing out,
which is the tell.

## A worked example

```bash
MI_WORK_DIR=/data \
MI_AUTH_TOKEN=… \
MI_ALLOW_ORIGINS=https://console.example \
MI_PROXY=http://proxy.internal:3128 \
MI_FRONTIER_DB=/data/frontier.db \
./build/vyzor serve --host 0.0.0.0 --port 8787 --render --interactive
```

`server.sh` sets the ones that matter for the sandbox and starts Vyzor for you;
see [operations.md](operations.md).
