#include "mi_antibot.h"
#include "mi_util.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

const char *mi_antibot_kind_name(mi_block_kind kind) {
    switch (kind) {
        case MI_BLOCK_NONE: return "none";
        case MI_BLOCK_RATE_LIMITED: return "rate_limited";
        case MI_BLOCK_IP: return "ip_block";
        case MI_BLOCK_TLS: return "tls_fingerprint";
        case MI_BLOCK_JS_CHALLENGE: return "js_challenge";
        case MI_BLOCK_CAPTCHA: return "captcha";
        case MI_BLOCK_LOGIN: return "login_required";
        case MI_BLOCK_GEO: return "geo_block";
        default: return "unknown";
    }
}

const char *mi_antibot_strategy_name(mi_strategy s) {
    switch (s) {
        case MI_STRATEGY_DIRECT: return "direct";
        case MI_STRATEGY_WAIT: return "wait";
        case MI_STRATEGY_IMPERSONATE: return "impersonate";
        case MI_STRATEGY_ROTATE_UA: return "rotate_ua";
        case MI_STRATEGY_ROTATE_PROXY: return "rotate_proxy";
        case MI_STRATEGY_BROWSER: return "browser";
        case MI_STRATEGY_CAPTCHA: return "captcha";
        case MI_STRATEGY_COOKIES: return "cookies";
        default: return "give_up";
    }
}

/* ------------------------------------------------------------------ */
/* Classification                                                      */
/* ------------------------------------------------------------------ */

/* A case-insensitive substring search. */
static int has_ci(const char *hay, const char *needle) {
    if (!hay || !needle) return 0;
    size_t nl = strlen(needle);
    if (nl == 0) return 0;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return 1;
    }
    return 0;
}

/* Whether a header block carries a given header name. */
static int header_present(const char *headers, const char *name) {
    if (!headers) return 0;
    size_t nl = strlen(name);
    for (const char *p = headers; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)name[i]))
            i++;
        if (i == nl && p[nl] == ':') return 1;
    }
    return 0;
}

/*
 * The markers of a *soft* block: a page that answers 200 but is a wall.
 *
 * These are the shapes a site returns when it would rather not say 403: a
 * challenge interstitial, an "access denied" page, an "unusual traffic" notice.
 * Reading one as content is the worst failure a crawler can make — it records
 * nothing and reports success.
 */
mi_block_kind mi_antibot_soft_block(const char *body, size_t body_len) {
    if (!body || body_len == 0) return MI_BLOCK_NONE;

    /* A rendered captcha is the most specific: solve it. */
    if (has_ci(body, "g-recaptcha") || has_ci(body, "grecaptcha") ||
        has_ci(body, "h-captcha") || has_ci(body, "cf-turnstile") ||
        has_ci(body, "px-captcha") || has_ci(body, "captcha-delivery")) {
        return MI_BLOCK_CAPTCHA;
    }
    /* A JS interstitial: Cloudflare, Akamai, DataDome, PerimeterX, Imperva. */
    if (has_ci(body, "cf-chl") || has_ci(body, "challenge-platform") ||
        has_ci(body, "cf_chl_opt") || has_ci(body, "jschl") ||
        has_ci(body, "Just a moment") || has_ci(body, "Checking your browser") ||
        has_ci(body, "cf-please-wait") || has_ci(body, "_Incapsula_Resource") ||
        has_ci(body, "datadome") || has_ci(body, "perimeterx") ||
        has_ci(body, "px-captcha")) {
        return MI_BLOCK_JS_CHALLENGE;
    }
    /* A login wall served with 200. */
    if (has_ci(body, "authwall") || has_ci(body, "login required") ||
        has_ci(body, "please log in") || has_ci(body, "please sign in")) {
        return MI_BLOCK_LOGIN;
    }
    /*
     * An access-denied page. Kept last and narrow: "access denied" appears in
     * legitimate error pages too, so it is only a block when the page is small —
     * a real content page that merely mentions the phrase is not a wall.
     */
    if (body_len < 16384 &&
        (has_ci(body, "access denied") || has_ci(body, "request blocked") ||
         has_ci(body, "you have been blocked") || has_ci(body, "unusual traffic"))) {
        return MI_BLOCK_IP;
    }
    return MI_BLOCK_NONE;
}

mi_block_kind mi_antibot_classify(int status, const char *headers, const char *body,
                                  size_t body_len) {
    /*
     * A 2xx is content *unless* the body is a wall. This is the soft-block case,
     * and it is checked first because the status alone would call it fine.
     */
    if (status >= 200 && status < 300) {
        return mi_antibot_soft_block(body, body_len);
    }

    /* The body tells the challenge walls apart from a plain ban, so it is read
     * before the status is trusted. */
    mi_block_kind soft = mi_antibot_soft_block(body, body_len);
    if (soft != MI_BLOCK_NONE) return soft;

    if (status == 429) return MI_BLOCK_RATE_LIMITED;
    if (status == 503 && header_present(headers, "Retry-After"))
        return MI_BLOCK_RATE_LIMITED;
    if (status == 451) return MI_BLOCK_GEO;

    /* A login wall: 401, or a 403 whose body names a sign-in. */
    if (status == 401) return MI_BLOCK_LOGIN;
    if (body && (has_ci(body, "sign in") || has_ci(body, "log in") ||
                 has_ci(body, "login required") || has_ci(body, "authwall") ||
                 has_ci(body, "please log in"))) {
        return MI_BLOCK_LOGIN;
    }

    if (status == 403) {
        /*
         * A 403 with no challenge body is a firewall ban — of the address or of
         * the client. The two want different moves and cannot always be told
         * apart from here, so it is reported as an IP block: the caller tries a
         * different address first, then a browser fingerprint, which covers both.
         */
        return MI_BLOCK_IP;
    }
    if (status == 503) return MI_BLOCK_UNKNOWN;
    return MI_BLOCK_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/* Per-host state                                                      */
/* ------------------------------------------------------------------ */

#define MAX_HOSTS 256

typedef struct {
    char *host;
    int failures;        /* consecutive blocks */
    mi_block_kind last;  /* the last kind seen */
    time_t cooldown_until;
    /* The rung the ladder had reached, so a host that fails the same way again
     * resumes rather than restarting from the cheapest move. */
    int rung;
    /* The identity that last worked here, or a nulled one. */
    mi_site_identity identity;
} host_state;

/* A proxy, and when it may be used again. */
typedef struct {
    char *url;
    time_t cooldown_until;
} proxy_state;

struct mi_antibot {
    host_state hosts[MAX_HOSTS];
    size_t host_count;
    proxy_state *proxies;
    size_t proxy_count;
    size_t proxy_rr;
    pthread_mutex_t lock;
};

mi_antibot *mi_antibot_new(void) {
    mi_antibot *a = mi_calloc(1, sizeof *a);
    if (!a) return NULL;
    pthread_mutex_init(&a->lock, NULL);
    return a;
}

void mi_antibot_free(mi_antibot *a) {
    if (!a) return;
    for (size_t i = 0; i < a->host_count; i++) {
        free(a->hosts[i].host);
        mi_site_identity_free(&a->hosts[i].identity);
    }
    for (size_t i = 0; i < a->proxy_count; i++) free(a->proxies[i].url);
    free(a->proxies);
    pthread_mutex_destroy(&a->lock);
    free(a);
}

/* Find or create the state for `host`. Caller holds the lock. */
static host_state *state_for(mi_antibot *a, const char *host) {
    if (!host || !*host) return NULL;
    for (size_t i = 0; i < a->host_count; i++) {
        if (!strcmp(a->hosts[i].host, host)) return &a->hosts[i];
    }
    if (a->host_count >= MAX_HOSTS) {
        /* Full: drop the oldest entry. A long crawl sees many hosts and the
         * least-recently-added one is the least useful to remember. */
        free(a->hosts[0].host);
        mi_site_identity_free(&a->hosts[0].identity);
        memmove(&a->hosts[0], &a->hosts[1], (MAX_HOSTS - 1) * sizeof a->hosts[0]);
        a->host_count = MAX_HOSTS - 1;
    }
    host_state *h = &a->hosts[a->host_count++];
    memset(h, 0, sizeof *h);
    h->host = mi_strdup(host);
    return h;
}

/* An escalating cooldown: 2s, then 8s, 30s, 2min, capped at 10min. */
static int cooldown_for(int failures) {
    static const int steps[] = {2, 8, 30, 120, 300, 600};
    int n = (int)(sizeof steps / sizeof steps[0]);
    if (failures <= 0) return 0;
    if (failures > n) failures = n;
    return steps[failures - 1];
}

void mi_antibot_note_block(mi_antibot *a, const char *host, mi_block_kind kind) {
    if (!a || !host) return;
    pthread_mutex_lock(&a->lock);
    host_state *h = state_for(a, host);
    if (h) {
        h->failures++;
        h->last = kind;
        h->cooldown_until = time(NULL) + cooldown_for(h->failures);
        /* The identity that had been working is no longer working. */
        mi_site_identity_free(&h->identity);
    }
    pthread_mutex_unlock(&a->lock);
}

void mi_antibot_note_success(mi_antibot *a, const char *host) {
    if (!a || !host) return;
    pthread_mutex_lock(&a->lock);
    host_state *h = state_for(a, host);
    if (h) {
        h->failures = 0;
        h->last = MI_BLOCK_NONE;
        h->cooldown_until = 0;
        h->rung = 0;
    }
    pthread_mutex_unlock(&a->lock);
}

int mi_antibot_cooldown(const mi_antibot *a, const char *host) {
    if (!a || !host) return 0;
    mi_antibot *m = (mi_antibot *)a;
    pthread_mutex_lock(&m->lock);
    int wait = 0;
    for (size_t i = 0; i < m->host_count; i++) {
        if (!strcmp(m->hosts[i].host, host)) {
            time_t now = time(NULL);
            if (m->hosts[i].cooldown_until > now)
                wait = (int)(m->hosts[i].cooldown_until - now);
            break;
        }
    }
    pthread_mutex_unlock(&m->lock);
    return wait;
}

int mi_antibot_failures(const mi_antibot *a, const char *host) {
    if (!a || !host) return 0;
    mi_antibot *m = (mi_antibot *)a;
    pthread_mutex_lock(&m->lock);
    int f = 0;
    for (size_t i = 0; i < m->host_count; i++) {
        if (!strcmp(m->hosts[i].host, host)) { f = m->hosts[i].failures; break; }
    }
    pthread_mutex_unlock(&m->lock);
    return f;
}

/* ------------------------------------------------------------------ */
/* Identity affinity                                                   */
/* ------------------------------------------------------------------ */

void mi_site_identity_free(mi_site_identity *id) {
    if (!id) return;
    free(id->proxy);
    free(id->user_agent);
    free(id->impersonate);
    memset(id, 0, sizeof *id);
}

void mi_antibot_remember_identity(mi_antibot *a, const char *host,
                                  mi_strategy strategy, const char *proxy,
                                  const char *user_agent, const char *impersonate) {
    if (!a || !host) return;
    pthread_mutex_lock(&a->lock);
    host_state *h = state_for(a, host);
    if (h) {
        mi_site_identity_free(&h->identity);
        h->identity.valid = 1;
        h->identity.strategy = strategy;
        h->identity.proxy = (proxy && *proxy) ? mi_strdup(proxy) : NULL;
        h->identity.user_agent = (user_agent && *user_agent) ? mi_strdup(user_agent) : NULL;
        h->identity.impersonate = (impersonate && *impersonate) ? mi_strdup(impersonate) : NULL;
    }
    pthread_mutex_unlock(&a->lock);
}

int mi_antibot_recall_identity(const mi_antibot *a, const char *host,
                               mi_site_identity *out) {
    if (!a || !host || !out) return 0;
    memset(out, 0, sizeof *out);
    mi_antibot *m = (mi_antibot *)a;
    pthread_mutex_lock(&m->lock);
    int found = 0;
    for (size_t i = 0; i < m->host_count; i++) {
        if (!strcmp(m->hosts[i].host, host) && m->hosts[i].identity.valid) {
            out->valid = 1;
            out->strategy = m->hosts[i].identity.strategy;
            out->proxy = m->hosts[i].identity.proxy
                             ? mi_strdup(m->hosts[i].identity.proxy) : NULL;
            out->user_agent = m->hosts[i].identity.user_agent
                                  ? mi_strdup(m->hosts[i].identity.user_agent) : NULL;
            out->impersonate = m->hosts[i].identity.impersonate
                                   ? mi_strdup(m->hosts[i].identity.impersonate) : NULL;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&m->lock);
    return found;
}

/* ------------------------------------------------------------------ */
/* The ladder                                                          */
/* ------------------------------------------------------------------ */

/*
 * The strategies that can clear a given block, cheapest first.
 *
 * The order encodes the economics: a wait costs nothing, a new User-Agent is
 * free, a browser costs seconds and hundreds of megabytes. A caller walks the
 * list, skipping what the host cannot do and what has already been tried.
 */
static const mi_strategy LADDER_RATE[] = {MI_STRATEGY_WAIT, MI_STRATEGY_GIVE_UP};
static const mi_strategy LADDER_IP[] = {MI_STRATEGY_ROTATE_PROXY,
                                        MI_STRATEGY_IMPERSONATE,
                                        MI_STRATEGY_BROWSER, MI_STRATEGY_GIVE_UP};
static const mi_strategy LADDER_TLS[] = {MI_STRATEGY_IMPERSONATE,
                                         MI_STRATEGY_ROTATE_PROXY,
                                         MI_STRATEGY_GIVE_UP};
static const mi_strategy LADDER_JS[] = {MI_STRATEGY_IMPERSONATE,
                                        MI_STRATEGY_BROWSER,
                                        MI_STRATEGY_ROTATE_PROXY,
                                        MI_STRATEGY_GIVE_UP};
static const mi_strategy LADDER_CAPTCHA[] = {MI_STRATEGY_IMPERSONATE,
                                             MI_STRATEGY_CAPTCHA,
                                             MI_STRATEGY_BROWSER,
                                             MI_STRATEGY_GIVE_UP};
static const mi_strategy LADDER_LOGIN[] = {MI_STRATEGY_COOKIES,
                                           MI_STRATEGY_BROWSER, MI_STRATEGY_GIVE_UP};
static const mi_strategy LADDER_UNKNOWN[] = {MI_STRATEGY_IMPERSONATE,
                                             MI_STRATEGY_ROTATE_UA,
                                             MI_STRATEGY_ROTATE_PROXY,
                                             MI_STRATEGY_BROWSER,
                                             MI_STRATEGY_GIVE_UP};

static const mi_strategy *ladder_for(mi_block_kind kind) {
    switch (kind) {
        case MI_BLOCK_RATE_LIMITED: return LADDER_RATE;
        case MI_BLOCK_IP: return LADDER_IP;
        case MI_BLOCK_TLS: return LADDER_TLS;
        case MI_BLOCK_JS_CHALLENGE: return LADDER_JS;
        case MI_BLOCK_CAPTCHA: return LADDER_CAPTCHA;
        case MI_BLOCK_LOGIN: return LADDER_LOGIN;
        case MI_BLOCK_GEO: return LADDER_IP; /* a different country may work */
        default: return LADDER_UNKNOWN;
    }
}

/* Whether the host can perform a strategy. */
static int can_do(const mi_strategy *s, const mi_antibot_caps *caps) {
    switch (*s) {
        case MI_STRATEGY_WAIT: return 1;
        case MI_STRATEGY_IMPERSONATE: return caps->can_impersonate;
        case MI_STRATEGY_ROTATE_UA: return caps->can_rotate_ua;
        case MI_STRATEGY_ROTATE_PROXY: return caps->proxy_count > 0;
        case MI_STRATEGY_BROWSER: return caps->can_browser;
        case MI_STRATEGY_CAPTCHA: return caps->can_captcha;
        case MI_STRATEGY_COOKIES: return caps->have_cookies;
        default: return 1;
    }
}

mi_strategy mi_antibot_next(mi_antibot *a, const char *host, mi_block_kind kind,
                            const mi_antibot_caps *caps, int *wait_seconds) {
    if (wait_seconds) *wait_seconds = 0;
    if (!a || !caps) return MI_STRATEGY_GIVE_UP;

    const mi_strategy *ladder = ladder_for(kind);

    pthread_mutex_lock(&a->lock);
    host_state *h = state_for(a, host);
    int rung = h ? h->rung : 0;
    pthread_mutex_unlock(&a->lock);

    for (; ladder[rung] != MI_STRATEGY_GIVE_UP; rung++) {
        if (!can_do(&ladder[rung], caps)) continue;
        mi_strategy s = ladder[rung];
        /* Advance the remembered rung so the next call is the next strategy. */
        pthread_mutex_lock(&a->lock);
        if (h) h->rung = rung + 1;
        pthread_mutex_unlock(&a->lock);
        if (s == MI_STRATEGY_WAIT && wait_seconds) {
            int cd = mi_antibot_cooldown(a, host);
            *wait_seconds = cd > 0 ? cd : 2;
        }
        return s;
    }
    return MI_STRATEGY_GIVE_UP;
}

/* ------------------------------------------------------------------ */
/* Proxy pool                                                          */
/* ------------------------------------------------------------------ */

void mi_antibot_set_proxies(mi_antibot *a, const char *proxies) {
    if (!a) return;
    pthread_mutex_lock(&a->lock);
    for (size_t i = 0; i < a->proxy_count; i++) free(a->proxies[i].url);
    free(a->proxies);
    a->proxies = NULL;
    a->proxy_count = 0;
    a->proxy_rr = 0;
    if (proxies && *proxies) {
        size_t cap = 4, n = 0;
        proxy_state *items = mi_calloc(cap, sizeof *items);
        if (items) {
            const char *p = proxies;
            while (*p) {
                while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
                if (!*p) break;
                const char *start = p;
                while (*p && *p != ',' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
                if (n == cap) {
                    size_t want = cap * 2;
                    proxy_state *grown = mi_realloc(items, want * sizeof *grown);
                    if (!grown) break;
                    items = grown;
                    cap = want;
                }
                items[n++].url = mi_strndup(start, (size_t)(p - start));
            }
            a->proxies = items;
            a->proxy_count = n;
        }
    }
    pthread_mutex_unlock(&a->lock);
}

int mi_antibot_proxy_count(const mi_antibot *a) {
    if (!a) return 0;
    mi_antibot *m = (mi_antibot *)a;
    pthread_mutex_lock(&m->lock);
    int n = (int)m->proxy_count;
    pthread_mutex_unlock(&m->lock);
    return n;
}

const char *mi_antibot_next_proxy(mi_antibot *a) {
    if (!a || a->proxy_count == 0) return NULL;
    pthread_mutex_lock(&a->lock);
    time_t now = time(NULL);
    const char *chosen = NULL;
    for (size_t i = 0; i < a->proxy_count; i++) {
        size_t idx = (a->proxy_rr + i) % a->proxy_count;
        if (a->proxies[idx].cooldown_until <= now) {
            chosen = a->proxies[idx].url;
            a->proxy_rr = idx + 1;
            break;
        }
    }
    pthread_mutex_unlock(&a->lock);
    return chosen;
}

void mi_antibot_park_proxy(mi_antibot *a, const char *proxy, int seconds) {
    if (!a || !proxy) return;
    pthread_mutex_lock(&a->lock);
    for (size_t i = 0; i < a->proxy_count; i++) {
        if (!strcmp(a->proxies[i].url, proxy)) {
            a->proxies[i].cooldown_until = time(NULL) + (seconds > 0 ? seconds : 60);
            break;
        }
    }
    pthread_mutex_unlock(&a->lock);
}

/* ------------------------------------------------------------------ */
/* URL helpers                                                         */
/* ------------------------------------------------------------------ */

char *mi_antibot_host_of(const char *url) {
    if (!url) return NULL;
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    /* Skip userinfo, if any. */
    const char *at = strchr(p, '@');
    const char *slash = strchr(p, '/');
    if (at && (!slash || at < slash)) p = at + 1;
    const char *end = p;
    while (*end && *end != '/' && *end != ':' && *end != '?' && *end != '#') end++;
    if (end == p) return NULL;
    return mi_strndup(p, (size_t)(end - p));
}
