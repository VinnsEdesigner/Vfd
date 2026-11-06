/* mi_antibot.h - deciding why a site refused us, and what to do about it.
 *
 * Vyzor can already rotate an identity, wear a browser's TLS fingerprint, use a
 * proxy, carry cookies and solve a captcha. What it could not do is decide
 * *which* of those a given refusal calls for, or remember the answer. Retrying a
 * TLS-fingerprint block with a new User-Agent is wasted work; rotating a proxy
 * for a rate limit makes it worse. The tools were there; the judgement was not.
 *
 * This is the brain, and it is three things:
 *
 *   classify   read a response and name the block — rate limit, IP ban, TLS
 *              fingerprint, JS challenge, captcha, login wall, geo block — and
 *              tell a *soft* block (HTTP 200 carrying a wall) from real content.
 *   plan       turn the block and the tools available into the next move on an
 *              escalating ladder, skipping what the host cannot do.
 *   remember   keep per-host state: what was refused, how long to back off, and
 *              the identity that finally worked — so a host is solved once, not
 *              rediscovered on every request.
 *
 * The memory is what makes it work at scale. A crawler that re-learns every site
 * on every pass is not a product; one that remembers "for this host, impersonate
 * + this proxy" is.
 */
#ifndef MI_ANTIBOT_H
#define MI_ANTIBOT_H

#include <stddef.h>

/* Why a request was refused. MI_BLOCK_NONE means it was not. */
typedef enum {
    MI_BLOCK_NONE = 0,
    MI_BLOCK_RATE_LIMITED,   /* 429 / 503 with a Retry-After: slow down */
    MI_BLOCK_IP,             /* 403 from a firewall: this address is banned */
    MI_BLOCK_TLS,            /* the fingerprint was rejected before HTTP */
    MI_BLOCK_JS_CHALLENGE,   /* a Cloudflare/Akamai-style interstitial */
    MI_BLOCK_CAPTCHA,        /* a solvable challenge is rendered */
    MI_BLOCK_LOGIN,          /* a sign-in is required */
    MI_BLOCK_GEO,            /* 451 / region-blocked */
    MI_BLOCK_UNKNOWN,        /* refused, but not in a way we recognise */
} mi_block_kind;

const char *mi_antibot_kind_name(mi_block_kind kind);

/*
 * Name the block from what came back.
 *
 * `headers` is the raw response header block (may be NULL), `body`/`body_len`
 * the response body (may be NULL/0). A *soft* block — HTTP 200 whose body is a
 * challenge wall or an "access denied" page rather than the content — is
 * classified by its body even though the status is fine, because treating a wall
 * as content is how a crawler records nothing and reports success.
 */
mi_block_kind mi_antibot_classify(int status, const char *headers, const char *body,
                                  size_t body_len);

/*
 * Whether a 2xx body is a wall rather than content.
 *
 * Separate from `classify` so a caller that already has a 200 can ask the narrow
 * question without re-deriving the status. Returns the block kind, or
 * MI_BLOCK_NONE when the body looks like real content.
 */
mi_block_kind mi_antibot_soft_block(const char *body, size_t body_len);

/* What a move looks like. */
typedef enum {
    MI_STRATEGY_DIRECT = 0,      /* the request was fine; nothing to change */
    MI_STRATEGY_WAIT,            /* pause, then retry the same way */
    MI_STRATEGY_IMPERSONATE,     /* retry wearing a browser's TLS fingerprint */
    MI_STRATEGY_ROTATE_UA,       /* retry with a different browser User-Agent */
    MI_STRATEGY_ROTATE_PROXY,    /* retry from a different address */
    MI_STRATEGY_BROWSER,         /* clear a JS challenge in a real browser */
    MI_STRATEGY_CAPTCHA,         /* solve the rendered captcha */
    MI_STRATEGY_COOKIES,         /* send the operator's sign-in cookies */
    MI_STRATEGY_GIVE_UP,         /* nothing left that could work */
} mi_strategy;

const char *mi_antibot_strategy_name(mi_strategy s);

/* The tools this Vyzor has. A strategy the host cannot perform is skipped. */
typedef struct {
    int can_impersonate;   /* the curl_cffi helper is installed */
    int can_rotate_ua;     /* there is more than one User-Agent to try */
    int proxy_count;       /* how many proxies are configured (0 = direct only) */
    int can_browser;       /* the interactive pool is available */
    int can_captcha;       /* a captcha provider is configured */
    int have_cookies;      /* the operator supplied sign-in cookies */
} mi_antibot_caps;

typedef struct mi_antibot mi_antibot;

mi_antibot *mi_antibot_new(void);
void mi_antibot_free(mi_antibot *a);

/*
 * The next move for `host` after a `kind` block, given what is available.
 *
 * Escalates: the first call after a block returns the cheapest strategy that
 * fits the block and the tools; a later call for the same host returns the next
 * rung, so a caller can loop. Returns MI_STRATEGY_GIVE_UP when the ladder is
 * exhausted. `wait_seconds` (optional) is set for MI_STRATEGY_WAIT.
 *
 * `host` is the site's host, e.g. "example.com"; state is kept per host.
 */
mi_strategy mi_antibot_next(mi_antibot *a, const char *host, mi_block_kind kind,
                            const mi_antibot_caps *caps, int *wait_seconds);

/*
 * How many seconds until `host` should be touched again (0 means now).
 *
 * A host that has refused us repeatedly is backed off; this is what stops a
 * crawl from turning a soft block into a hard one.
 */
int mi_antibot_cooldown(const mi_antibot *a, const char *host);

/* A request to `host` succeeded: clear its penalty and remember the identity. */
void mi_antibot_note_success(mi_antibot *a, const char *host);

/* A request to `host` was refused: record the kind. */
void mi_antibot_note_block(mi_antibot *a, const char *host, mi_block_kind kind);

/* Consecutive blocks recorded for `host` (0 if it is behaving). */
int mi_antibot_failures(const mi_antibot *a, const char *host);

/* ------------------------------------------------------------------ */
/* Identity affinity: the winning identity for a host, remembered       */
/* ------------------------------------------------------------------ */

/*
 * The identity that worked for a host, so the next request reuses it.
 *
 * Rotating on every request is itself a bot signal: a real visitor keeps one
 * address and one browser for a site. Once an identity is seen to work it is
 * pinned to that host, and only a *classified* block rotates it. This is the
 * "sticky session" that turns per-request rotation into per-host identity.
 *
 * Strings are owned by the caller; pass NULL for fields that did not matter.
 */
typedef struct {
    int valid;
    mi_strategy strategy;   /* the move that worked */
    char *proxy;            /* NULL for a direct connection */
    char *user_agent;       /* NULL to keep the default */
    char *impersonate;      /* NULL when no impersonation was needed */
} mi_site_identity;

/*
 * Record that `strategy` (with these concrete settings) worked for `host`.
 * Replaces any previous record for that host.
 */
void mi_antibot_remember_identity(mi_antibot *a, const char *host,
                                  mi_strategy strategy, const char *proxy,
                                  const char *user_agent, const char *impersonate);

/*
 * The remembered identity for `host`, or 0 when none is known.
 *
 * On success `out` is filled with owned strings the caller frees with
 * `mi_site_identity_free`.
 */
int mi_antibot_recall_identity(const mi_antibot *a, const char *host,
                               mi_site_identity *out);

void mi_site_identity_free(mi_site_identity *id);

/*
 * The proxy pool, with a cooldown per proxy.
 *
 * A proxy that just got blocked is parked rather than reused immediately, which
 * is the difference between rotating and *thrashing*. `proxies` is the same
 * comma/whitespace-separated list the identity pool takes.
 */
void mi_antibot_set_proxies(mi_antibot *a, const char *proxies);
/* The next proxy to try, or NULL when none is usable right now. */
const char *mi_antibot_next_proxy(mi_antibot *a);
/* Park the proxy just used for `seconds`, because it was refused. */
void mi_antibot_park_proxy(mi_antibot *a, const char *proxy, int seconds);
/* How many proxies are configured. */
int mi_antibot_proxy_count(const mi_antibot *a);

/* The host part of a URL, for keying state. Owned by the caller. */
char *mi_antibot_host_of(const char *url);

#endif
