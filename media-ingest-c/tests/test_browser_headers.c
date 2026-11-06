/* test_browser_headers.c - the header set a browser sends, and its coherence.
 *
 * The property that matters is coherence: the `sec-ch-ua` version must match the
 * User-Agent's, and a family that does not send client hints must not send them.
 * A site that checks this blocks a request whose UA claims Chrome but whose
 * headers do not, however good the TLS fingerprint is.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mi_browser_headers.h"

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

/* Join a header set into one string, for substring checks. */
static char *join(char **h, size_t n) {
    size_t len = 1;
    for (size_t i = 0; i < n; i++) len += strlen(h[i]) + 1;
    char *s = malloc(len);
    s[0] = 0;
    for (size_t i = 0; i < n; i++) { strcat(s, h[i]); strcat(s, "\n"); }
    return s;
}

static void test_chrome_coherence(void) {
    char *out[16];
    const char *ua = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                     "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
    size_t n = mi_browser_headers(MI_BROWSER_CHROME, NULL, ua, out, 16);
    char **h = out;
    CHECK(n > 0, "chrome headers are produced");
    char *s = join(h, n);
    CHECK(strstr(s, "sec-ch-ua:") != NULL, "chrome sends sec-ch-ua");
    /* The version in sec-ch-ua must match the UA's major version. */
    CHECK(strstr(s, "Google Chrome\";v=\"131\"") != NULL,
          "sec-ch-ua carries the UA's own version");
    CHECK(strstr(s, "sec-ch-ua-platform: \"Windows\"") != NULL,
          "the platform hint is present");
    CHECK(strstr(s, "Sec-Fetch-Mode: navigate") != NULL, "fetch metadata is present");
    CHECK(strstr(s, "Accept-Language: en-US") != NULL, "accept-language is present");
    CHECK(strstr(s, "Priority: u=0, i") != NULL, "priority is present for chrome");
    free(s);
    mi_browser_headers_free(h, n);
}

static void test_firefox_sends_no_client_hints(void) {
    char *out[16];
    const char *ua = "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0";
    size_t n = mi_browser_headers(MI_BROWSER_FIREFOX, NULL, ua, out, 16);
    char *s = join(out, n);
    /* Sending client hints from Firefox is as much a tell as omitting them
     * from Chrome. */
    CHECK(strstr(s, "sec-ch-ua") == NULL, "firefox sends no sec-ch-ua");
    CHECK(strstr(s, "Sec-Fetch-Mode: navigate") != NULL,
          "firefox still sends fetch metadata");
    free(s);
    mi_browser_headers_free(out, n);
}

static void test_family_of(void) {
    CHECK(mi_browser_family_of("chrome131") == MI_BROWSER_CHROME, "chrome family");
    CHECK(mi_browser_family_of("firefox") == MI_BROWSER_FIREFOX, "firefox family");
    CHECK(mi_browser_family_of("safari17") == MI_BROWSER_SAFARI, "safari family");
    CHECK(mi_browser_family_of(NULL) == MI_BROWSER_CHROME, "default is chrome");
}

int main(void) {
    test_chrome_coherence();
    test_firefox_sends_no_client_hints();
    test_family_of();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
