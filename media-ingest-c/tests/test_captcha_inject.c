/* test_captcha_inject.c - the JavaScript that plants a solved token.
 *
 * A token alone is not enough: each widget only reports success when its own
 * callback runs. The JS is generated here and run by the browser, so the JS is
 * what is tested — with no browser, because the shape is the part that goes
 * wrong.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mi_captcha_inject.h"
#include "mi_html.h"

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

static char *js(const char *html, mi_captcha_kind kind, const char *token) {
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    char *s = mi_captcha_inject_js(doc, kind, token);
    mi_html_free(doc);
    return s;
}

static void test_fields(void) {
    CHECK(!strcmp(mi_captcha_response_field(MI_CAPTCHA_RECAPTCHA_V2),
                  "g-recaptcha-response"), "reCAPTCHA v2 field");
    CHECK(!strcmp(mi_captcha_response_field(MI_CAPTCHA_RECAPTCHA_V3),
                  "g-recaptcha-response"), "reCAPTCHA v3 uses the same field");
    CHECK(!strcmp(mi_captcha_response_field(MI_CAPTCHA_HCAPTCHA),
                  "h-captcha-response"), "hCaptcha field");
    CHECK(!strcmp(mi_captcha_response_field(MI_CAPTCHA_TURNSTILE),
                  "cf-turnstile-response"), "Turnstile field");
}

static void test_recaptcha(void) {
    const char *html =
        "<div class=\"g-recaptcha\" data-sitekey=\"K\" data-callback=\"onDone\"></div>";
    char *s = js(html, MI_CAPTCHA_RECAPTCHA_V2, "TOKEN123");
    CHECK(s != NULL, "a reCAPTCHA page yields JS");
    CHECK(s && strstr(s, "g-recaptcha-response") != NULL, "the response field is filled");
    CHECK(s && strstr(s, "TOKEN123") != NULL, "the token is planted");
    CHECK(s && strstr(s, "onDone") != NULL, "the page's own callback is called");
    free(s);
}

static void test_turnstile(void) {
    const char *html = "<div class=\"cf-turnstile\" data-sitekey=\"K\"></div>";
    char *s = js(html, MI_CAPTCHA_TURNSTILE, "T");
    CHECK(s && strstr(s, "cf-turnstile-response") != NULL, "the Turnstile field is filled");
    free(s);
    /* A page with no Turnstile widget yields nothing. */
    char *none = js("<div class=\"other\"></div>", MI_CAPTCHA_TURNSTILE, "T");
    CHECK(none == NULL, "no widget means no JS");
    free(none);
}

static void test_token_escaping(void) {
    /* A token with a quote or a tag must not break out of the literal. */
    const char *html = "<div class=\"g-recaptcha\" data-sitekey=\"K\"></div>";
    char *s = js(html, MI_CAPTCHA_RECAPTCHA_V2, "a'b\\c</script>");
    CHECK(s != NULL, "a hostile token still yields JS");
    /* The quote is escaped, and the '<' is escaped so it cannot close a tag. */
    CHECK(s && strstr(s, "\\'") != NULL, "a single quote is escaped");
    CHECK(s && strstr(s, "\\x3c") != NULL, "a '<' is escaped");
    CHECK(s && strstr(s, "</script>") == NULL, "the literal cannot close a script tag");
    free(s);
}

static void test_none(void) {
    CHECK(js("<div></div>", MI_CAPTCHA_NONE, "t") == NULL, "kind none yields nothing");
    CHECK(mi_captcha_inject_js(NULL, MI_CAPTCHA_RECAPTCHA_V2, "t") == NULL,
          "a NULL document yields nothing");
    mi_html_node *doc = mi_html_parse("<div class=g-recaptcha></div>", 28);
    CHECK(mi_captcha_inject_js(doc, MI_CAPTCHA_RECAPTCHA_V2, NULL) == NULL,
          "a NULL token yields nothing");
    mi_html_free(doc);
}

int main(void) {
    test_fields();
    test_recaptcha();
    test_turnstile();
    test_token_escaping();
    test_none();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
