/* mi_captcha.h - detecting a challenge page, and solving it through a provider.
 *
 * There is no library that *solves* a captcha; solving is either a paid service
 * or a model, and both are external. So this module is a *seam*: it detects that
 * a page is a challenge, extracts what a solver needs (the widget kind and its
 * site key), and hands that to a provider. The provider is pluggable — a stub
 * for tests and development, a real service when a key is configured.
 *
 * Detection is the half that has real value on its own: a crawl that hits a
 * challenge currently reports "no file found", which is wrong and unactionable.
 * Recognising the wall lets the caller say *why*.
 */
#ifndef MI_CAPTCHA_H
#define MI_CAPTCHA_H

#include <stddef.h>

typedef enum {
    MI_CAPTCHA_NONE = 0,
    MI_CAPTCHA_RECAPTCHA_V2,
    MI_CAPTCHA_RECAPTCHA_V3,
    MI_CAPTCHA_HCAPTCHA,
    MI_CAPTCHA_TURNSTILE,
} mi_captcha_kind;

typedef struct {
    mi_captcha_kind kind;
    /* The widget's site key, which is what a solver is asked about. */
    char *sitekey;
    /* reCAPTCHA v3 needs the action name; NULL otherwise. */
    char *action;
} mi_captcha_challenge;

/*
 * Look at a page's HTML and decide whether it is a challenge.
 *
 * Returns 1 and fills `out` when a challenge is recognised, 0 otherwise. `out`
 * must be released with `mi_captcha_challenge_free`.
 */
int mi_captcha_detect(const char *html, size_t len, mi_captcha_challenge *out);

void mi_captcha_challenge_free(mi_captcha_challenge *ch);

/* A stable name for the kind, for logs and JSON. */
const char *mi_captcha_kind_name(mi_captcha_kind kind);

typedef struct {
    /*
     * The provider to solve through: "stub", "2captcha", "capsolver", or NULL.
     *
     * NULL or "stub" uses the built-in stub, which returns a deterministic token
     * and talks to nothing — the default so nothing is ever sent to a third
     * party by accident. A real provider needs `api_key`.
     */
    const char *provider;
    const char *api_key;
    /* Seconds to wait for a real provider. 0 uses a default. */
    int timeout_seconds;
} mi_captcha_config;

/*
 * Ask the provider for a token for `challenge` on `page_url`.
 *
 * Returns an owned token string, or NULL on failure with `err` filled. The stub
 * never fails and never leaves the process.
 */
char *mi_captcha_solve(const mi_captcha_config *cfg, const mi_captcha_challenge *challenge,
                       const char *page_url, char *err, size_t errcap);

/* Whether `provider` names a real (network) provider rather than the stub. */
int mi_captcha_provider_is_remote(const char *provider);

#endif
