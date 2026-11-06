/* test_identity.c - the identity pool: what a retry presents.
 *
 * The property that matters: attempt 0 is the caller's own configuration, and a
 * later attempt is a *different* browser identity — never the library's own
 * name, which is what a site blocks on sight. A pool with rotation off must not
 * change anything.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mi_identity.h"

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

/* A rotation never repeats the library's own name, and never a bot-looking UA. */
static void test_rotation_uses_browsers(void) {
    mi_identity_pool *p = mi_identity_pool_new(NULL, NULL, NULL, 1);
    CHECK(p != NULL, "pool builds");
    CHECK(mi_identity_ua_count(p) >= 2, "there is more than one UA to rotate");

    /* Attempt 0 with no configured UA is NULL on purpose: it means "use the
     * caller's own default", which is the library's name. Only a *retry* must
     * wear a browser identity. */
    mi_identity id0 = mi_identity_for_attempt(p, 0);
    CHECK(id0.user_agent == NULL, "attempt 0 with no configured UA defers to the caller");

    for (int attempt = 1; attempt < 8; attempt++) {
        mi_identity id = mi_identity_for_attempt(p, attempt);
        CHECK(id.user_agent != NULL, "a retry has a UA");
        if (id.user_agent) {
            CHECK(strstr(id.user_agent, "Mozilla/5.0") != NULL,
                  "a rotated UA is a browser, not a bot");
            CHECK(strstr(id.user_agent, "media-ingest") == NULL,
                  "a rotated UA is never the library's own name");
        }
    }
    mi_identity_pool_free(p);
}

/* Attempt 0 is the operator's own choice; the pool only changes retries. */
static void test_attempt_zero_is_the_caller(void) {
    mi_identity_pool *p = mi_identity_pool_new("MyAgent/1.0", "http://a:1,http://b:2",
                                                "chrome", 1);
    mi_identity id0 = mi_identity_for_attempt(p, 0);
    CHECK(id0.user_agent && !strcmp(id0.user_agent, "MyAgent/1.0"),
          "attempt 0 is the caller's own UA");
    CHECK(id0.proxy && !strcmp(id0.proxy, "http://a:1"),
          "attempt 0 is the first proxy");
    CHECK(id0.impersonate && !strcmp(id0.impersonate, "chrome"),
          "the impersonation target is carried through");

    mi_identity id1 = mi_identity_for_attempt(p, 1);
    CHECK(id1.user_agent && strcmp(id1.user_agent, "MyAgent/1.0") != 0,
          "attempt 1 is a different UA");
    CHECK(id1.proxy && !strcmp(id1.proxy, "http://b:2"),
          "attempt 1 rotates to the next proxy");

    mi_identity id2 = mi_identity_for_attempt(p, 2);
    CHECK(id2.proxy && !strcmp(id2.proxy, "http://a:1"),
          "proxies wrap around");

    CHECK(mi_identity_proxy_count(p) == 2, "two proxies were parsed");
    mi_identity_pool_free(p);
}

/* With rotation off, every attempt is the base identity — the old behaviour. */
static void test_rotation_off_is_stable(void) {
    mi_identity_pool *p = mi_identity_pool_new("MyAgent/1.0", "http://a:1", NULL, 0);
    for (int attempt = 0; attempt < 4; attempt++) {
        mi_identity id = mi_identity_for_attempt(p, attempt);
        CHECK(id.user_agent && !strcmp(id.user_agent, "MyAgent/1.0"),
              "rotation off keeps the caller's UA");
        CHECK(id.proxy && !strcmp(id.proxy, "http://a:1"),
              "rotation off keeps the one proxy");
    }
    mi_identity_pool_free(p);
}

/* No proxy configured means direct, on every attempt. */
static void test_no_proxy_is_direct(void) {
    mi_identity_pool *p = mi_identity_pool_new(NULL, "", NULL, 1);
    CHECK(mi_identity_proxy_count(p) == 0, "an empty proxy list has no entries");
    for (int attempt = 0; attempt < 3; attempt++) {
        mi_identity id = mi_identity_for_attempt(p, attempt);
        CHECK(id.proxy == NULL, "no proxy means direct");
    }
    mi_identity_pool_free(p);
}

/* A proxy list separated by commas, whitespace, or both is split the same. */
static void test_proxy_list_splitting(void) {
    mi_identity_pool *p = mi_identity_pool_new(NULL, " http://a:1 ,\nhttp://b:2\thttp://c:3 ",
                                                NULL, 1);
    CHECK(mi_identity_proxy_count(p) == 3, "three proxies split from mixed separators");
    mi_identity_pool_free(p);
}

int main(void) {
    test_rotation_uses_browsers();
    test_attempt_zero_is_the_caller();
    test_rotation_off_is_stable();
    test_no_proxy_is_direct();
    test_proxy_list_splitting();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
