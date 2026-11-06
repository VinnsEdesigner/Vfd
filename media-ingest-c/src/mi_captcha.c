#include "mi_captcha.h"
#include "mi_util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Case-insensitive search for `needle` in a length-bounded haystack. */
static const char *find_ci(const char *hay, size_t n, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0 || n < nl) return NULL;
    for (size_t i = 0; i + nl <= n; i++) {
        size_t j = 0;
        while (j < nl && tolower((unsigned char)hay[i + j]) ==
                             tolower((unsigned char)needle[j]))
            j++;
        if (j == nl) return hay + i;
    }
    return NULL;
}

/*
 * Pull the value of an attribute from a tag.
 *
 * Good enough for the challenge markup, which is machine-generated and regular.
 * `attr` is matched case-insensitively; the value may be single- or
 * double-quoted. Returns an owned string or NULL.
 */
static char *attr_value(const char *tag, const char *attr) {
    const char *p = find_ci(tag, strlen(tag), attr);
    if (!p) return NULL;
    p += strlen(attr);
    while (*p && (*p == ' ' || *p == '\t')) p++;
    if (*p != '=') return NULL;
    p++;
    while (*p && (*p == ' ' || *p == '\t')) p++;
    if (*p != '"' && *p != '\'') return NULL;
    char quote = *p++;
    const char *start = p;
    while (*p && *p != quote) p++;
    if (*p != quote) return NULL;
    return mi_strndup(start, (size_t)(p - start));
}

int mi_captcha_detect(const char *html, size_t len, mi_captcha_challenge *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!html || len == 0) return 0;

    /*
     * The markers, most specific first.
     *
     * A page can carry several; the one that is actually rendered is the first
     * of these to appear, which is why the order matters.
     */
    if (find_ci(html, len, "challenges.cloudflare.com/turnstile") ||
        find_ci(html, len, "cf-turnstile")) {
        out->kind = MI_CAPTCHA_TURNSTILE;
    } else if (find_ci(html, len, "hcaptcha.com/1/api.js") ||
               find_ci(html, len, "h-captcha")) {
        out->kind = MI_CAPTCHA_HCAPTCHA;
    } else if (find_ci(html, len, "recaptcha/api.js") ||
               find_ci(html, len, "g-recaptcha")) {
        /* v3 is the invisible, score-based variant; it names an action. */
        if (find_ci(html, len, "grecaptcha.execute") ||
            find_ci(html, len, "recaptcha/api.js?render=")) {
            out->kind = MI_CAPTCHA_RECAPTCHA_V3;
        } else {
            out->kind = MI_CAPTCHA_RECAPTCHA_V2;
        }
    } else {
        return 0;
    }

    /* The site key lives on the widget's tag. */
    const char *tag = NULL;
    switch (out->kind) {
        case MI_CAPTCHA_TURNSTILE:
            tag = find_ci(html, len, "cf-turnstile");
            if (!tag) tag = find_ci(html, len, "data-sitekey");
            break;
        case MI_CAPTCHA_HCAPTCHA:
            tag = find_ci(html, len, "h-captcha");
            if (!tag) tag = find_ci(html, len, "data-sitekey");
            break;
        default:
            tag = find_ci(html, len, "g-recaptcha");
            if (!tag) tag = find_ci(html, len, "data-sitekey");
            break;
    }
    if (tag) {
        out->sitekey = attr_value(tag, "data-sitekey");
        if (!out->sitekey) out->sitekey = attr_value(tag, "sitekey");
        out->action = attr_value(tag, "data-action");
    }
    return 1;
}

void mi_captcha_challenge_free(mi_captcha_challenge *ch) {
    if (!ch) return;
    free(ch->sitekey);
    free(ch->action);
    memset(ch, 0, sizeof *ch);
}

const char *mi_captcha_kind_name(mi_captcha_kind kind) {
    switch (kind) {
        case MI_CAPTCHA_RECAPTCHA_V2: return "recaptcha_v2";
        case MI_CAPTCHA_RECAPTCHA_V3: return "recaptcha_v3";
        case MI_CAPTCHA_HCAPTCHA: return "hcaptcha";
        case MI_CAPTCHA_TURNSTILE: return "turnstile";
        default: return "none";
    }
}

int mi_captcha_provider_is_remote(const char *provider) {
    if (!provider || !*provider) return 0;
    if (!strcmp(provider, "stub")) return 0;
    return 1;
}

/*
 * The stub.
 *
 * It returns a fixed, obviously-fake token and contacts nothing. That is the
 * point: the whole path can be exercised — detection, the provider call, the
 * token being used — without an account or a network, and nothing is ever sent
 * to a third party unless the operator configured a real provider.
 */
static char *solve_stub(const mi_captcha_challenge *ch) {
    return mi_asprintf("stub-token-for-%s-%s", mi_captcha_kind_name(ch->kind),
                       ch->sitekey ? ch->sitekey : "no-key");
}

char *mi_captcha_solve(const mi_captcha_config *cfg, const mi_captcha_challenge *challenge,
                       const char *page_url, char *err, size_t errcap) {
    (void)page_url;
    if (err && errcap) err[0] = '\0';
    if (!challenge || challenge->kind == MI_CAPTCHA_NONE) {
        if (err && errcap) snprintf(err, errcap, "no challenge to solve");
        return NULL;
    }
    const char *provider = cfg ? cfg->provider : NULL;

    if (!mi_captcha_provider_is_remote(provider)) return solve_stub(challenge);

    /*
     * A real provider is named but not yet implemented. Refusing is the correct
     * behaviour: silently returning a stub token would let a caller believe a
     * challenge was solved when nothing was sent, which is worse than a clear
     * failure. The detection half is still useful without this.
     */
    if (err && errcap) {
        snprintf(err, errcap,
                 "the '%s' captcha provider is not implemented yet; use 'stub'",
                 provider);
    }
    return NULL;
}
