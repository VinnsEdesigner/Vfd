#include "mi_captcha.h"
#include "mi_json.h"
#include "mi_util.h"

#include <ctype.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
    if (*p == '"' || *p == '\'') {
        char quote = *p++;
        const char *start = p;
        while (*p && *p != quote) p++;
        if (*p != quote) return NULL;
        return mi_strndup(start, (size_t)(p - start));
    }
    /* An unquoted attribute runs to the next space or the tag's end. Real
     * challenge markup quotes these, but some templates do not, and a missing
     * site key is a solve that cannot start. */
    const char *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '>' && *p != '/') p++;
    if (p == start) return NULL;
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

/* ------------------------------------------------------------------ */
/* Providers                                                           */
/* ------------------------------------------------------------------ */

static const char *const PROVIDERS[] = {
    "stub", "2captcha", "anti-captcha", "capsolver", NULL,
};

const char *const *mi_captcha_providers(void) { return PROVIDERS; }

int mi_captcha_provider_is_remote(const char *provider) {
    if (!provider || !*provider) return 0;
    if (!strcmp(provider, "stub")) return 0;
    return 1;
}

int mi_captcha_provider_known(const char *provider) {
    if (!provider || !*provider) return 1; /* empty means the stub */
    for (size_t i = 0; PROVIDERS[i]; i++) {
        if (!strcmp(provider, PROVIDERS[i])) return 1;
    }
    return 0;
}

/* The provider's default endpoint, when the caller named none. */
static const char *provider_base(const mi_captcha_config *cfg) {
    if (cfg && cfg->base_url && *cfg->base_url) return cfg->base_url;
    const char *p = cfg ? cfg->provider : NULL;
    if (p && !strcmp(p, "2captcha")) return "https://2captcha.com";
    if (p && !strcmp(p, "anti-captcha")) return "https://api.anti-captcha.com";
    if (p && !strcmp(p, "capsolver")) return "https://api.capsolver.com";
    return NULL;
}

/* ------------------------------------------------------------------ */
/* A small HTTP client for the provider calls                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char *data;
    size_t len;
} http_buf;

static size_t http_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
    http_buf *b = userdata;
    size_t n = size * nmemb;
    char *grown = mi_realloc(b->data, b->len + n + 1);
    if (!grown) return 0;
    b->data = grown;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

/*
 * POST `body` to `url` and return the response text, or NULL with `err` filled.
 * `content_type` is "application/json" or "application/x-www-form-urlencoded".
 */
static char *http_post(const char *url, const char *body, const char *content_type,
                       int timeout_seconds, char *err, size_t errcap) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        if (err && errcap) snprintf(err, errcap, "curl init failed");
        return NULL;
    }
    struct curl_slist *headers = NULL;
    char *ct = mi_asprintf("Content-Type: %s", content_type);
    if (ct) headers = curl_slist_append(headers, ct);

    http_buf b = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)(timeout_seconds > 0 ? timeout_seconds : 30));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(ct);
    if (rc != CURLE_OK) {
        if (err && errcap) snprintf(err, errcap, "request failed: %s", curl_easy_strerror(rc));
        free(b.data);
        return NULL;
    }
    if (code < 200 || code >= 300) {
        if (err && errcap) snprintf(err, errcap, "provider returned HTTP %ld", code);
        free(b.data);
        return NULL;
    }
    return b.data ? b.data : mi_strdup("");
}

static char *http_get(const char *url, int timeout_seconds, char *err, size_t errcap) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        if (err && errcap) snprintf(err, errcap, "curl init failed");
        return NULL;
    }
    http_buf b = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)(timeout_seconds > 0 ? timeout_seconds : 30));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || code < 200 || code >= 300) {
        if (err && errcap) snprintf(err, errcap, "provider request failed");
        free(b.data);
        return NULL;
    }
    return b.data ? b.data : mi_strdup("");
}

/* ------------------------------------------------------------------ */
/* The stub                                                            */
/* ------------------------------------------------------------------ */

/*
 * Returns a fixed, obviously-fake token and contacts nothing. That is the point:
 * the whole path can be exercised without an account, and nothing is ever sent
 * to a third party unless the operator configured a real provider.
 */
static char *solve_stub(const mi_captcha_challenge *ch) {
    return mi_asprintf("stub-token-for-%s-%s", mi_captcha_kind_name(ch->kind),
                       ch->sitekey ? ch->sitekey : "no-key");
}

/* ------------------------------------------------------------------ */
/* 2captcha (and compatible resellers)                                 */
/* ------------------------------------------------------------------ */

/* 2captcha's method name for a challenge kind. */
static const char *twocaptcha_method(mi_captcha_kind kind) {
    switch (kind) {
        case MI_CAPTCHA_RECAPTCHA_V2:
        case MI_CAPTCHA_RECAPTCHA_V3: return "userrecaptcha";
        case MI_CAPTCHA_HCAPTCHA: return "hcaptcha";
        case MI_CAPTCHA_TURNSTILE: return "turnstile";
        default: return NULL;
    }
}

static char *solve_2captcha(const mi_captcha_config *cfg, const mi_captcha_challenge *ch,
                            const char *page_url, char *err, size_t errcap) {
    const char *base = provider_base(cfg);
    const char *method = twocaptcha_method(ch->kind);
    if (!method) {
        if (err && errcap) snprintf(err, errcap, "2captcha cannot solve that kind");
        return NULL;
    }
    char *key = mi_url_encode(cfg->api_key ? cfg->api_key : "");
    char *page = mi_url_encode(page_url ? page_url : "");
    char *site = mi_url_encode(ch->sitekey ? ch->sitekey : "");

    char *body = mi_asprintf("key=%s&method=%s&sitekey=%s&pageurl=%s",
                             key, method, site, page);
    if (ch->kind == MI_CAPTCHA_RECAPTCHA_V3) {
        char *act = mi_url_encode(ch->action ? ch->action : "verify");
        char *b2 = mi_asprintf("%s&version=v3&action=%s&min_score=0.5", body, act);
        free(body);
        body = b2;
        free(act);
    }
    free(key); free(page); free(site);

    char *url = mi_asprintf("%s/in.php", base);
    char *resp = http_post(url, body, "application/x-www-form-urlencoded", 30, err, errcap);
    free(url);
    free(body);
    if (!resp) return NULL;
    if (strncmp(resp, "OK|", 3) != 0) {
        if (err && errcap) snprintf(err, errcap, "2captcha rejected the task: %.120s", resp);
        free(resp);
        return NULL;
    }
    char *task_id = mi_strdup(resp + 3);
    free(resp);

    /* Poll until the token is ready or the deadline passes. */
    int timeout = (cfg->timeout_seconds > 0) ? cfg->timeout_seconds : 120;
    time_t deadline = time(NULL) + timeout;
    char *token = NULL;
    while (time(NULL) < deadline) {
        sleep(5);
        char *k = mi_url_encode(cfg->api_key ? cfg->api_key : "");
        char *u = mi_asprintf("%s/res.php?key=%s&action=get&id=%s", base, k, task_id);
        free(k);
        char *r = http_get(u, 30, err, errcap);
        free(u);
        if (!r) break;
        if (!strncmp(r, "OK|", 3)) {
            token = mi_strdup(r + 3);
            free(r);
            break;
        }
        if (!strncmp(r, "CAPCHA_NOT_READY", 16)) {
            free(r);
            continue;
        }
        if (err && errcap) snprintf(err, errcap, "2captcha error: %.120s", r);
        free(r);
        break;
    }
    free(task_id);
    if (!token && err && errcap && !*err) snprintf(err, errcap, "2captcha timed out");
    return token;
}

/* ------------------------------------------------------------------ */
/* anti-captcha and capsolver (the same JSON task protocol)            */
/* ------------------------------------------------------------------ */

static int is_capsolver(const mi_captcha_config *cfg) {
    return cfg->provider && !strcmp(cfg->provider, "capsolver");
}

/* The task type name for a kind, per provider. */
static const char *task_type(const mi_captcha_config *cfg, mi_captcha_kind kind) {
    int cap = is_capsolver(cfg);
    switch (kind) {
        case MI_CAPTCHA_RECAPTCHA_V2:
            return cap ? "ReCaptchaV2TaskProxyLess" : "RecaptchaV2TaskProxyless";
        case MI_CAPTCHA_RECAPTCHA_V3:
            return cap ? "ReCaptchaV3TaskProxyLess" : "RecaptchaV3TaskProxyless";
        case MI_CAPTCHA_HCAPTCHA:
            return cap ? "HCaptchaTaskProxyLess" : "HCaptchaTaskProxyless";
        case MI_CAPTCHA_TURNSTILE:
            return cap ? "AntiTurnstileTaskProxyLess" : "TurnstileTaskProxyless";
        default: return NULL;
    }
}

/* The field name the task uses for the site key: capsolver's Turnstile uses
 * `websiteKey`, anti-captcha's uses `websiteKey` too; both share the shape. */
static char *json_escape_str(const char *s) {
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_string(&b, s ? s : "");
    char *out = mi_strdup(mi_json_data(&b));
    mi_json_free(&b);
    return out;
}

static char *solve_task_provider(const mi_captcha_config *cfg,
                                 const mi_captcha_challenge *ch, const char *page_url,
                                 char *err, size_t errcap) {
    const char *base = provider_base(cfg);
    const char *type = task_type(cfg, ch->kind);
    if (!type) {
        if (err && errcap) snprintf(err, errcap, "provider cannot solve that kind");
        return NULL;
    }
    int cap = is_capsolver(cfg);
    /* capsolver names the key field `websiteKey` and the token field the same;
     * anti-captcha matches. Both take a clientKey/appKey. */
    const char *key_field = cap ? "clientKey" : "clientKey";

    char *key = json_escape_str(cfg->api_key);
    char *urlj = json_escape_str(page_url);
    char *sitej = json_escape_str(ch->sitekey);
    char *body = mi_asprintf(
        "{\"%s\":%s,\"task\":{\"type\":\"%s\",\"websiteURL\":%s,\"websiteKey\":%s}}",
        key_field, key, type, urlj, sitej);
    free(key); free(urlj); free(sitej);

    char *url = mi_asprintf("%s/createTask", base);
    char *resp = http_post(url, body, "application/json", 30, err, errcap);
    free(url);
    free(body);
    if (!resp) return NULL;

    mi_json *j = mi_json_parse(resp);
    free(resp);
    if (!j) {
        if (err && errcap) snprintf(err, errcap, "provider returned unparseable JSON");
        return NULL;
    }
    const char *e = mi_json_get_string(j, "errorId", NULL);
    if (e && strcmp(e, "0") != 0) {
        const char *d = mi_json_get_string(j, "errorDescription", "rejected");
        if (err && errcap) snprintf(err, errcap, "provider rejected the task: %s", d);
        mi_json_value_free(j);
        return NULL;
    }
    char *task_id = mi_strdup(mi_json_get_string(j, "taskId", ""));
    mi_json_value_free(j);
    if (!task_id || !*task_id) {
        free(task_id);
        if (err && errcap) snprintf(err, errcap, "provider returned no task id");
        return NULL;
    }

    int timeout = (cfg->timeout_seconds > 0) ? cfg->timeout_seconds : 120;
    time_t deadline = time(NULL) + timeout;
    char *token = NULL;
    while (time(NULL) < deadline) {
        sleep(5);
        char *k = json_escape_str(cfg->api_key);
        char *id = json_escape_str(task_id);
        char *b = mi_asprintf("{\"%s\":%s,\"taskId\":%s}",
                              cap ? "clientKey" : "clientKey", k, id);
        free(k); free(id);
        char *u = mi_asprintf("%s/getTaskResult", base);
        char *r = http_post(u, b, "application/json", 30, err, errcap);
        free(u); free(b);
        if (!r) break;
        mi_json *rj = mi_json_parse(r);
        free(r);
        if (!rj) break;
        const char *st = mi_json_get_string(rj, "status", NULL);
        if (st && !strcmp(st, "ready")) {
            const mi_json *sol = mi_json_get(rj, "solution");
            const char *t = mi_json_get_string(sol, "gRecaptchaResponse", NULL);
            if (!t) t = mi_json_get_string(sol, "token", NULL);
            if (!t) t = mi_json_get_string(sol, "text", NULL);
            if (t) token = mi_strdup(t);
            mi_json_value_free(rj);
            break;
        }
        mi_json_value_free(rj);
    }
    free(task_id);
    if (!token && err && errcap && !*err) snprintf(err, errcap, "provider timed out");
    return token;
}

/* ------------------------------------------------------------------ */
/* Solve                                                               */
/* ------------------------------------------------------------------ */

char *mi_captcha_solve(const mi_captcha_config *cfg, const mi_captcha_challenge *challenge,
                       const char *page_url, char *err, size_t errcap) {
    if (err && errcap) err[0] = '\0';
    if (!challenge || challenge->kind == MI_CAPTCHA_NONE) {
        if (err && errcap) snprintf(err, errcap, "no challenge to solve");
        return NULL;
    }
    const char *provider = cfg ? cfg->provider : NULL;

    if (!mi_captcha_provider_is_remote(provider)) return solve_stub(challenge);

    if (!cfg->api_key || !*cfg->api_key) {
        if (err && errcap) snprintf(err, errcap, "the '%s' provider needs an API key", provider);
        return NULL;
    }
    if (!mi_captcha_provider_known(provider)) {
        if (err && errcap) snprintf(err, errcap, "unknown captcha provider '%s'", provider);
        return NULL;
    }
    if (!strcmp(provider, "2captcha")) {
        return solve_2captcha(cfg, challenge, page_url, err, errcap);
    }
    if (!strcmp(provider, "anti-captcha") || !strcmp(provider, "capsolver")) {
        return solve_task_provider(cfg, challenge, page_url, err, errcap);
    }
    if (err && errcap) snprintf(err, errcap, "the '%s' provider is not implemented", provider);
    return NULL;
}
