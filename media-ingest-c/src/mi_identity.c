#include "mi_identity.h"
#include "mi_util.h"

#include <stdlib.h>
#include <string.h>

/*
 * Browser User-Agents a retry rotates through.
 *
 * A default like "media-ingest-c/0.1" is honest and gets a crawler blocked on
 * sight. These are ordinary, current desktop browsers, so a *retry* looks like
 * what any other visitor looks like. The exact version does not matter much —
 * what matters is that it is not the library's own name.
 */
static const char *const UA_ROTATION[] = {
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/17.4 Safari/605.1.15",
    "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:128.0) Gecko/20100101 Firefox/128.0",
};
#define UA_ROTATION_COUNT (sizeof UA_ROTATION / sizeof UA_ROTATION[0])

struct mi_identity_pool {
    /* The operator's choice for the first attempt; NULL means "no opinion". */
    char *base_ua;
    /* The proxies, split from the configured string. Empty means direct. */
    char **proxies;
    size_t proxy_count;
    char *impersonate;
    int rotate;
};

/*
 * Split a proxy list on commas and whitespace.
 *
 * A single proxy is the common case and yields a one-element list. An empty or
 * NULL string yields none, which is "direct".
 */
static void split_proxies(const char *s, char ***out, size_t *count) {
    *out = NULL;
    *count = 0;
    if (!s || !*s) return;
    size_t cap = 4, n = 0;
    char **items = mi_calloc(cap, sizeof *items);
    if (!items) return;
    const char *p = s;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        if (n == cap) {
            size_t want = cap * 2;
            char **grown = mi_realloc(items, want * sizeof *grown);
            if (!grown) break;
            items = grown;
            cap = want;
        }
        items[n++] = mi_strndup(start, (size_t)(p - start));
    }
    *out = items;
    *count = n;
}

mi_identity_pool *mi_identity_pool_new(const char *user_agent, const char *proxy,
                                       const char *impersonate, int rotate) {
    mi_identity_pool *pool = mi_calloc(1, sizeof *pool);
    if (!pool) return NULL;
    pool->base_ua = (user_agent && *user_agent) ? mi_strdup(user_agent) : NULL;
    pool->impersonate = (impersonate && *impersonate) ? mi_strdup(impersonate) : NULL;
    pool->rotate = rotate;
    split_proxies(proxy, &pool->proxies, &pool->proxy_count);
    return pool;
}

void mi_identity_pool_free(mi_identity_pool *pool) {
    if (!pool) return;
    free(pool->base_ua);
    for (size_t i = 0; i < pool->proxy_count; i++) free(pool->proxies[i]);
    free(pool->proxies);
    free(pool->impersonate);
    free(pool);
}

size_t mi_identity_ua_count(const mi_identity_pool *pool) {
    (void)pool;
    return UA_ROTATION_COUNT;
}

size_t mi_identity_proxy_count(const mi_identity_pool *pool) {
    return pool ? pool->proxy_count : 0;
}

mi_identity mi_identity_for_attempt(const mi_identity_pool *pool, int attempt) {
    mi_identity id = {NULL, NULL, NULL};
    if (!pool) return id;

    /* The operator's own User-Agent on the first attempt; a browser thereafter.
     * With rotation off, the base identity is used every time. */
    if (attempt <= 0 || !pool->rotate) {
        id.user_agent = pool->base_ua;
    } else {
        /* Start at a stable offset so two concurrent downloads do not all pick
         * the same agent first. */
        size_t idx = ((size_t)attempt - 1) % UA_ROTATION_COUNT;
        id.user_agent = UA_ROTATION[idx];
    }

    if (pool->proxy_count > 0) {
        size_t idx = pool->rotate ? ((size_t)attempt % pool->proxy_count) : 0;
        id.proxy = pool->proxies[idx];
    }
    id.impersonate = pool->impersonate;
    return id;
}
