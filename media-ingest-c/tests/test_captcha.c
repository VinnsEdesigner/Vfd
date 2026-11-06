/* test_captcha.c - challenge detection and the solver seam.
 *
 * Detection is the half that stands on its own: a crawl that hits a wall must be
 * able to say so rather than report "not found". The solver seam is tested with
 * the stub, which must never contact anything, and with a named remote provider,
 * which must refuse rather than pretend.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mi_captcha.h"

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

static void test_detect_turnstile(void) {
    const char *html =
        "<html><body><div class=\"cf-turnstile\" data-sitekey=\"0x4AAA\"></div>"
        "<script src=\"https://challenges.cloudflare.com/turnstile/v0/api.js\"></script>"
        "</body></html>";
    mi_captcha_challenge ch;
    CHECK(mi_captcha_detect(html, strlen(html), &ch) == 1, "turnstile detected");
    CHECK(ch.kind == MI_CAPTCHA_TURNSTILE, "kind is turnstile");
    CHECK(ch.sitekey && !strcmp(ch.sitekey, "0x4AAA"), "sitekey extracted");
    mi_captcha_challenge_free(&ch);
}

static void test_detect_recaptcha_v2(void) {
    const char *html =
        "<div class='g-recaptcha' data-sitekey='6Lc-abc'></div>"
        "<script src=\"https://www.google.com/recaptcha/api.js\"></script>";
    mi_captcha_challenge ch;
    CHECK(mi_captcha_detect(html, strlen(html), &ch) == 1, "recaptcha detected");
    CHECK(ch.kind == MI_CAPTCHA_RECAPTCHA_V2, "kind is recaptcha v2");
    CHECK(ch.sitekey && !strcmp(ch.sitekey, "6Lc-abc"), "single-quoted sitekey parsed");
    mi_captcha_challenge_free(&ch);
}

static void test_detect_recaptcha_v3(void) {
    const char *html =
        "<script src=\"https://www.google.com/recaptcha/api.js?render=6Lc-v3\"></script>"
        "<script>grecaptcha.execute('6Lc-v3', {action: 'login'})</script>";
    mi_captcha_challenge ch;
    CHECK(mi_captcha_detect(html, strlen(html), &ch) == 1, "recaptcha v3 detected");
    CHECK(ch.kind == MI_CAPTCHA_RECAPTCHA_V3, "kind is recaptcha v3");
    mi_captcha_challenge_free(&ch);
}

static void test_detect_hcaptcha(void) {
    const char *html = "<div class=\"h-captcha\" data-sitekey=\"10000000-ffff\"></div>";
    mi_captcha_challenge ch;
    CHECK(mi_captcha_detect(html, strlen(html), &ch) == 1, "hcaptcha detected");
    CHECK(ch.kind == MI_CAPTCHA_HCAPTCHA, "kind is hcaptcha");
    CHECK(ch.sitekey && !strcmp(ch.sitekey, "10000000-ffff"), "sitekey extracted");
    mi_captcha_challenge_free(&ch);
}

static void test_plain_page_is_not_a_challenge(void) {
    const char *html = "<html><body><h1>Hello</h1><a href=\"/file.pdf\">file</a></body></html>";
    mi_captcha_challenge ch;
    CHECK(mi_captcha_detect(html, strlen(html), &ch) == 0, "a plain page is not a challenge");
    CHECK(mi_captcha_detect(NULL, 0, &ch) == 0, "NULL is not a challenge");
}

static void test_stub_solver(void) {
    mi_captcha_challenge ch = {.kind = MI_CAPTCHA_TURNSTILE, .sitekey = strdup("KEY1")};
    mi_captcha_config cfg = {.provider = "stub"};
    char err[128];
    char *tok = mi_captcha_solve(&cfg, &ch, "https://site.example/", err, sizeof err);
    CHECK(tok != NULL, "the stub solves");
    CHECK(tok && strstr(tok, "stub-token") != NULL, "the stub token is marked as a stub");
    free(tok);
    mi_captcha_challenge_free(&ch);
}

/* A real provider needs a key; without one it refuses rather than pretending. */
static void test_remote_provider_needs_a_key(void) {
    mi_captcha_challenge ch = {.kind = MI_CAPTCHA_RECAPTCHA_V2, .sitekey = strdup("KEY2")};
    mi_captcha_config cfg = {.provider = "2captcha"};
    char err[128];
    char *tok = mi_captcha_solve(&cfg, &ch, "https://site.example/", err, sizeof err);
    CHECK(tok == NULL, "a real provider without a key refuses");
    CHECK(strstr(err, "API key") != NULL, "the refusal names the missing key");
    mi_captcha_challenge_free(&ch);
}

/* An unknown provider is refused, so a typo does not silently do nothing. */
static void test_unknown_provider_refused(void) {
    mi_captcha_challenge ch = {.kind = MI_CAPTCHA_RECAPTCHA_V2, .sitekey = strdup("KEY3")};
    mi_captcha_config cfg = {.provider = "nope", .api_key = "x"};
    char err[128];
    char *tok = mi_captcha_solve(&cfg, &ch, "https://site.example/", err, sizeof err);
    CHECK(tok == NULL, "an unknown provider refuses");
    mi_captcha_challenge_free(&ch);
}

/* The provider list is what the UI offers, so it must name the real ones. */
static void test_provider_list(void) {
    const char *const *p = mi_captcha_providers();
    int has_stub = 0, has_2c = 0, has_ac = 0, has_cap = 0;
    for (size_t i = 0; p[i]; i++) {
        if (!strcmp(p[i], "stub")) has_stub = 1;
        if (!strcmp(p[i], "2captcha")) has_2c = 1;
        if (!strcmp(p[i], "anti-captcha")) has_ac = 1;
        if (!strcmp(p[i], "capsolver")) has_cap = 1;
    }
    CHECK(has_stub && has_2c && has_ac && has_cap, "the provider list names them all");
    CHECK(mi_captcha_provider_known("capsolver") == 1, "capsolver is known");
    CHECK(mi_captcha_provider_known("nope") == 0, "an unknown name is not known");
    CHECK(mi_captcha_provider_known("") == 1, "an empty name means the stub, which is known");
}

int main(void) {
    test_detect_turnstile();
    test_detect_recaptcha_v2();
    test_detect_recaptcha_v3();
    test_detect_hcaptcha();
    test_plain_page_is_not_a_challenge();
    test_stub_solver();
    test_remote_provider_needs_a_key();
    test_unknown_provider_refused();
    test_provider_list();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
