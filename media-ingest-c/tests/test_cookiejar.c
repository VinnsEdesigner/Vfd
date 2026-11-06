/* test_cookiejar.c - the live cookie store a session needs.
 *
 * The properties a login depends on: a Set-Cookie is stored and sent back;
 * a login *rotates* a session id (same name replaces the value); a domain and
 * path scope a cookie; a Secure cookie is only sent over https; an expired
 * cookie is dropped; and a Netscape export loads and re-exports.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mi_cookiejar.h"

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

static int header_has(const char *h, const char *needle) {
    return h && strstr(h, needle) != NULL;
}

static void test_basic_roundtrip(void) {
    mi_cookiejar *j = mi_cookiejar_new();
    mi_cookiejar_set(j, "sessionid=abc123; Path=/", "example.com", "/login");
    char *h = mi_cookiejar_header_for_url(j, "https://example.com/anything");
    CHECK(header_has(h, "sessionid=abc123"), "a set cookie is sent back");
    free(h);
    /* It is not sent to another host. */
    h = mi_cookiejar_header_for_url(j, "https://other.com/");
    CHECK(h == NULL, "a cookie is not sent to another host");
    free(h);
    mi_cookiejar_free(j);
}

/* A login sets the same cookie name again with a new value. */
static void test_rotation(void) {
    mi_cookiejar *j = mi_cookiejar_new();
    mi_cookiejar_set(j, "sid=old; Path=/", "example.com", "/");
    mi_cookiejar_set(j, "sid=new; Path=/", "example.com", "/");
    CHECK(mi_cookiejar_count(j) == 1, "the same cookie is replaced, not duplicated");
    char *h = mi_cookiejar_header_for_url(j, "https://example.com/");
    CHECK(header_has(h, "sid=new") && !header_has(h, "sid=old"),
          "the new value is what is sent");
    free(h);
    mi_cookiejar_free(j);
}

static void test_domain_scope(void) {
    mi_cookiejar *j = mi_cookiejar_new();
    mi_cookiejar_set(j, "wide=1; Domain=example.com; Path=/", "example.com", "/");
    /* A subdomain gets it. */
    char *h = mi_cookiejar_header_for_url(j, "https://www.example.com/");
    CHECK(header_has(h, "wide=1"), "a Domain cookie reaches a subdomain");
    free(h);
    /* A look-alike host does not. */
    h = mi_cookiejar_header_for_url(j, "https://notexample.com/");
    CHECK(h == NULL, "a look-alike host does not get the cookie");
    free(h);
    /* A site cannot set a cookie for a domain it is not under. */
    mi_cookiejar_set(j, "evil=1; Domain=other.com; Path=/", "example.com", "/");
    h = mi_cookiejar_header_for_url(j, "https://other.com/");
    CHECK(h == NULL, "a cookie for a foreign domain is refused");
    free(h);
    mi_cookiejar_free(j);
}

static void test_path_scope(void) {
    mi_cookiejar *j = mi_cookiejar_new();
    mi_cookiejar_set(j, "a=1; Path=/admin", "example.com", "/admin/login");
    char *h = mi_cookiejar_header_for_url(j, "https://example.com/admin/x");
    CHECK(header_has(h, "a=1"), "a Path cookie is sent under its path");
    free(h);
    h = mi_cookiejar_header_for_url(j, "https://example.com/public");
    CHECK(h == NULL, "and not outside it");
    free(h);
    mi_cookiejar_free(j);
}

static void test_secure(void) {
    mi_cookiejar *j = mi_cookiejar_new();
    mi_cookiejar_set(j, "s=1; Secure; Path=/", "example.com", "/");
    char *h = mi_cookiejar_header_for_url(j, "https://example.com/");
    CHECK(header_has(h, "s=1"), "a Secure cookie is sent over https");
    free(h);
    h = mi_cookiejar_header_for_url(j, "http://example.com/");
    CHECK(h == NULL, "and not over http");
    free(h);
    mi_cookiejar_free(j);
}

static void test_expiry(void) {
    mi_cookiejar *j = mi_cookiejar_new();
    mi_cookiejar_set(j, "gone=1; Path=/", "example.com", "/");
    /* A past Expires deletes it. */
    mi_cookiejar_set(j, "gone=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT",
                     "example.com", "/");
    char *h = mi_cookiejar_header_for_url(j, "https://example.com/");
    CHECK(h == NULL, "an expired cookie is removed");
    free(h);
    mi_cookiejar_free(j);
}

static void test_from_headers(void) {
    mi_cookiejar *j = mi_cookiejar_new();
    const char *hdrs =
        "HTTP/1.1 200 OK\r\n"
        "Set-Cookie: a=1; Path=/\r\n"
        "Set-Cookie: b=2; Path=/; HttpOnly\r\n"
        "Content-Type: text/html\r\n";
    mi_cookiejar_set_from_headers(j, hdrs, "example.com", "/");
    CHECK(mi_cookiejar_count(j) == 2, "both Set-Cookie headers are stored");
    char *h = mi_cookiejar_header_for_url(j, "https://example.com/");
    CHECK(header_has(h, "a=1") && header_has(h, "b=2"), "both are sent");
    free(h);
    mi_cookiejar_free(j);
}

static void test_netscape(void) {
    mi_cookiejar *j = mi_cookiejar_new();
    const char *export_text =
        "# Netscape HTTP Cookie File\n"
        ".example.com\tTRUE\t/\tFALSE\t0\tfromfile\tvalue1\n";
    mi_cookiejar_load_netscape(j, export_text);
    CHECK(mi_cookiejar_count(j) == 1, "a Netscape line loads");
    char *h = mi_cookiejar_header_for_url(j, "https://www.example.com/");
    CHECK(header_has(h, "fromfile=value1"), "and is sent to the subdomain");
    free(h);
    /* It re-exports. */
    char *out = mi_cookiejar_to_netscape(j);
    CHECK(out && strstr(out, "fromfile") != NULL, "the jar exports to Netscape");
    free(out);
    mi_cookiejar_free(j);
}

int main(void) {
    test_basic_roundtrip();
    test_rotation();
    test_domain_scope();
    test_path_scope();
    test_secure();
    test_expiry();
    test_from_headers();
    test_netscape();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
