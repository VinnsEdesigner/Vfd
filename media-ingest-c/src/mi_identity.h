/* mi_identity.h - who we present ourselves as, and how a retry changes it.
 *
 * A site that refuses a request is often refusing the *identity*: a bot
 * User-Agent, a datacenter address, or a TLS fingerprint that is plainly
 * libcurl. Retrying the same way is retrying the same refusal, so a retry is
 * given a different identity — a different browser User-Agent, and a different
 * proxy when the operator supplied more than one.
 *
 * The first attempt is the caller's own configuration; the pool only changes
 * what a *retry* looks like. That honours the operator's explicit choice on the
 * happy path and makes only the fallback look like an ordinary browser.
 *
 * The proxy list is a plain string so the setting stays a string: several
 * proxies, separated by commas or whitespace, are rotated. One proxy is the
 * common case and behaves exactly as before.
 */
#ifndef MI_IDENTITY_H
#define MI_IDENTITY_H

#include <stddef.h>

/* One way of presenting ourselves to a site. Pointers are owned by the pool
 * that produced the value; they are valid until that pool is freed. */
typedef struct {
    /* NULL means "use the caller's own default", which is what attempt 0 does
     * when no User-Agent was configured. */
    const char *user_agent;
    /* NULL means direct. */
    const char *proxy;
    /* A yt-dlp / curl_cffi impersonation target, e.g. "chrome"; NULL is off. */
    const char *impersonate;
} mi_identity;

typedef struct mi_identity_pool mi_identity_pool;

/*
 * Build a pool from the configured values.
 *
 * `proxy` may name one proxy or several (comma- or whitespace-separated);
 * several are rotated across retries. `rotate` 0 pins every attempt to the base
 * identity, which is the old behaviour and the right choice when the operator
 * wants a fixed identity. Returns NULL only on allocation failure.
 */
mi_identity_pool *mi_identity_pool_new(const char *user_agent, const char *proxy,
                                       const char *impersonate, int rotate);

void mi_identity_pool_free(mi_identity_pool *pool);

/*
 * The identity for attempt N, where 0 is the first try.
 *
 * Attempt 0 is the base identity. Later attempts rotate the User-Agent and, when
 * there is more than one, the proxy. The returned pointers belong to the pool.
 */
mi_identity mi_identity_for_attempt(const mi_identity_pool *pool, int attempt);

/* How many browser User-Agents the pool can rotate through (always > 0). */
size_t mi_identity_ua_count(const mi_identity_pool *pool);
/* How many proxies were configured (0 means direct). */
size_t mi_identity_proxy_count(const mi_identity_pool *pool);

#endif
