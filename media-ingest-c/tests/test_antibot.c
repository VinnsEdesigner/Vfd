/* test_antibot.c - naming a block, and choosing the next move.
 *
 * The classification is what tells a solvable challenge from a plain ban, and
 * the ladder is what decides whether a retry is worth making. Both are pure
 * logic, so they are tested here with no network. The per-host memory is tested
 * for the two properties a scale crawl depends on: a failing host is backed off,
 * and a proxy that was refused is parked rather than reused.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mi_antibot.h"
#include "mi_util.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);   \
        }                                                                       \
    } while (0)

static void test_classify(void) {
    /* A 2xx is never a block, whatever the body says. */
    CHECK(mi_antibot_classify(200, NULL, "sign in to continue", 20) == MI_BLOCK_NONE,
          "a 200 is not a block");

    CHECK(mi_antibot_classify(429, NULL, NULL, 0) == MI_BLOCK_RATE_LIMITED,
          "429 is a rate limit");
    CHECK(mi_antibot_classify(503, "Retry-After: 30\r\n", NULL, 0) ==
              MI_BLOCK_RATE_LIMITED,
          "503 with Retry-After is a rate limit");

    CHECK(mi_antibot_classify(403, NULL, "<html>Just a moment...</html>", 30) ==
              MI_BLOCK_JS_CHALLENGE,
          "a Cloudflare interstitial is a JS challenge");
    CHECK(mi_antibot_classify(403, NULL,
              "<div class=\"g-recaptcha\" data-sitekey=\"k\"></div>", 46) ==
              MI_BLOCK_CAPTCHA,
          "a rendered captcha is a captcha block");

    CHECK(mi_antibot_classify(403, "Server: cloudflare\r\n", "<html>nope</html>", 17) ==
              MI_BLOCK_IP,
          "a bare 403 is an address ban");
    CHECK(mi_antibot_classify(401, NULL, NULL, 0) == MI_BLOCK_LOGIN,
          "401 is a login wall");
    CHECK(mi_antibot_classify(451, NULL, NULL, 0) == MI_BLOCK_GEO,
          "451 is a geo block");

    /* A challenge body beats the status code: a captcha served with 503 is still
     * a captcha, and solving it is the move. */
    CHECK(mi_antibot_classify(503, NULL, "<div id=\"h-captcha\"></div>", 27) ==
              MI_BLOCK_CAPTCHA,
          "a captcha on a 503 is still a captcha");
}

/* The ladder must fit the block: the right first move, and a give-up. */
static void test_ladder(void) {
    mi_antibot_caps full = {.can_impersonate = 1, .can_rotate_ua = 1,
                            .proxy_count = 2, .can_browser = 1, .can_captcha = 1,
                            .have_cookies = 1};
    mi_antibot *a = mi_antibot_new();
    int wait = 0;

    /* A rate limit's only sensible move is to wait. */
    CHECK(mi_antibot_next(a, "h1", MI_BLOCK_RATE_LIMITED, &full, &wait) ==
              MI_STRATEGY_WAIT,
          "a rate limit waits");
    CHECK(wait > 0, "the wait has a duration");
    CHECK(mi_antibot_next(a, "h1", MI_BLOCK_RATE_LIMITED, &full, &wait) ==
              MI_STRATEGY_GIVE_UP,
          "a rate limit has nothing after waiting");

    /* A TLS block goes to impersonation first — a new User-Agent would not help. */
    CHECK(mi_antibot_next(a, "h2", MI_BLOCK_TLS, &full, &wait) ==
              MI_STRATEGY_IMPERSONATE,
          "a TLS block impersonates first");
    CHECK(mi_antibot_next(a, "h2", MI_BLOCK_TLS, &full, &wait) ==
              MI_STRATEGY_ROTATE_PROXY,
          "a TLS block falls to a new address next");

    /* A login wall goes to cookies. */
    CHECK(mi_antibot_next(a, "h3", MI_BLOCK_LOGIN, &full, &wait) ==
              MI_STRATEGY_COOKIES,
          "a login wall sends cookies first");

    /* A captcha block reaches the solver. */
    CHECK(mi_antibot_next(a, "h4", MI_BLOCK_CAPTCHA, &full, &wait) ==
              MI_STRATEGY_IMPERSONATE, "a captcha block impersonates first");
    CHECK(mi_antibot_next(a, "h4", MI_BLOCK_CAPTCHA, &full, &wait) ==
              MI_STRATEGY_CAPTCHA, "then solves");

    mi_antibot_free(a);
}

/* A host with no tools must be given up on, not sent a strategy it cannot do. */
static void test_ladder_without_tools(void) {
    mi_antibot_caps bare = {0};
    mi_antibot *a = mi_antibot_new();
    int wait = 0;
    CHECK(mi_antibot_next(a, "h", MI_BLOCK_TLS, &bare, &wait) == MI_STRATEGY_GIVE_UP,
          "a TLS block with no impersonation gives up");
    CHECK(mi_antibot_next(a, "h", MI_BLOCK_CAPTCHA, &bare, &wait) == MI_STRATEGY_GIVE_UP,
          "a captcha with no solver gives up");
    mi_antibot_free(a);
}

/* A failing host is backed off, and a success clears it. */
static void test_host_memory(void) {
    mi_antibot *a = mi_antibot_new();
    CHECK(mi_antibot_cooldown(a, "x.com") == 0, "a fresh host has no cooldown");
    mi_antibot_note_block(a, "x.com", MI_BLOCK_IP);
    CHECK(mi_antibot_failures(a, "x.com") == 1, "a block is counted");
    CHECK(mi_antibot_cooldown(a, "x.com") > 0, "a blocked host is in cooldown");
    mi_antibot_note_block(a, "x.com", MI_BLOCK_IP);
    int cd2 = mi_antibot_cooldown(a, "x.com");
    CHECK(cd2 > 0, "the cooldown grows with repeated failures");
    mi_antibot_note_success(a, "x.com");
    CHECK(mi_antibot_failures(a, "x.com") == 0, "success clears the failures");
    CHECK(mi_antibot_cooldown(a, "x.com") == 0, "success clears the cooldown");
    mi_antibot_free(a);
}

/* A refused proxy is parked; the pool skips it. */
static void test_proxy_pool(void) {
    mi_antibot *a = mi_antibot_new();
    mi_antibot_set_proxies(a, "http://p1:1, http://p2:2");
    CHECK(mi_antibot_proxy_count(a) == 2, "two proxies parsed");
    const char *p1 = mi_antibot_next_proxy(a);
    CHECK(p1 != NULL, "a proxy is offered");
    char *first = mi_strdup(p1);
    mi_antibot_park_proxy(a, first, 3600);
    /* With p1 parked, the next must be the other one. */
    const char *p2 = mi_antibot_next_proxy(a);
    CHECK(p2 != NULL && strcmp(p2, first) != 0, "a parked proxy is skipped");
    free(first);
    mi_antibot_free(a);
}

static void test_host_of(void) {
    char *h = mi_antibot_host_of("https://user:pass@www.example.com:8443/a?b=1");
    CHECK(h && !strcmp(h, "www.example.com"), "the host is extracted");
    free(h);
    h = mi_antibot_host_of("http://example.com/x");
    CHECK(h && !strcmp(h, "example.com"), "a bare host works");
    free(h);
    CHECK(mi_antibot_host_of(NULL) == NULL, "a NULL url has no host");
}

/* A soft block is a 200 that is really a wall. */
static void test_soft_block(void) {
    /* A 200 whose body is a Cloudflare interstitial is a block, not content. */
    const char *cf = "<html><title>Just a moment...</title><div id=\"cf-chl\"></div></html>";
    CHECK(mi_antibot_soft_block(cf, strlen(cf)) == MI_BLOCK_JS_CHALLENGE,
          "a Cloudflare interstitial in a 200 is a soft block");
    CHECK(mi_antibot_classify(200, NULL, cf, strlen(cf)) == MI_BLOCK_JS_CHALLENGE,
          "a 200 carrying a challenge classifies as the challenge");
    const char *cap = "<div class=\"g-recaptcha\"></div>";
    CHECK(mi_antibot_soft_block(cap, strlen(cap)) == MI_BLOCK_CAPTCHA,
          "a captcha in a 200 is a soft block");
    const char *denied = "<html><body>Access Denied</body></html>";
    CHECK(mi_antibot_soft_block(denied, strlen(denied)) == MI_BLOCK_IP,
          "an access-denied page in a 200 is a soft block");
    /* Real content is not a block. */
    const char *real = "<html><body><h1>A real page</h1><p>lots of words here</p></body></html>";
    CHECK(mi_antibot_soft_block(real, strlen(real)) == MI_BLOCK_NONE,
          "real content is not a soft block");
    CHECK(mi_antibot_classify(200, NULL, real, strlen(real)) == MI_BLOCK_NONE,
          "a 200 of real content is not a block");
    /* A large page that merely mentions the phrase is not a wall. */
    CHECK(mi_antibot_soft_block("access denied", 13) == MI_BLOCK_IP,
          "a small access-denied page is a wall");
    char big[20000];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    CHECK(mi_antibot_soft_block(big, sizeof big - 1) == MI_BLOCK_NONE,
          "a large page is not treated as a wall just for its length");
}

/* The identity that worked is remembered and reused; a block forgets it. */
static void test_identity_affinity(void) {
    mi_antibot *a = mi_antibot_new();
    mi_site_identity id;
    CHECK(mi_antibot_recall_identity(a, "shop.example", &id) == 0,
          "no identity is known before one works");

    mi_antibot_remember_identity(a, "shop.example", MI_STRATEGY_IMPERSONATE,
                                 "http://p1:1", "UA/1", "chrome");
    CHECK(mi_antibot_recall_identity(a, "shop.example", &id) == 1,
          "the working identity is recalled");
    CHECK(id.strategy == MI_STRATEGY_IMPERSONATE, "the strategy is remembered");
    CHECK(id.proxy && !strcmp(id.proxy, "http://p1:1"), "the proxy is remembered");
    CHECK(id.impersonate && !strcmp(id.impersonate, "chrome"),
          "the impersonation is remembered");
    mi_site_identity_free(&id);

    /* A different host has no identity of its own. */
    CHECK(mi_antibot_recall_identity(a, "other.example", &id) == 0,
          "identity is per host");

    /* A block forgets the identity, because it is no longer working. */
    mi_antibot_note_block(a, "shop.example", MI_BLOCK_IP);
    CHECK(mi_antibot_recall_identity(a, "shop.example", &id) == 0,
          "a block forgets the stale identity");
    mi_antibot_free(a);
}

int main(void) {
    test_classify();
    test_soft_block();
    test_identity_affinity();
    test_ladder();
    test_ladder_without_tools();
    test_host_memory();
    test_proxy_pool();
    test_host_of();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
