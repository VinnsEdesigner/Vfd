#define _GNU_SOURCE
#include "mi_captcha_inject.h"
#include "mi_json.h"
#include "mi_util.h"

#include <stdlib.h>
#include <string.h>

const char *mi_captcha_response_field(mi_captcha_kind kind) {
    switch (kind) {
        case MI_CAPTCHA_RECAPTCHA_V2:
        case MI_CAPTCHA_RECAPTCHA_V3: return "g-recaptcha-response";
        case MI_CAPTCHA_HCAPTCHA: return "h-captcha-response";
        case MI_CAPTCHA_TURNSTILE: return "cf-turnstile-response";
        default: return "";
    }
}

/* The class/attribute that marks a widget of this kind. */
static const char *widget_marker(mi_captcha_kind kind) {
    switch (kind) {
        case MI_CAPTCHA_RECAPTCHA_V2:
        case MI_CAPTCHA_RECAPTCHA_V3: return "g-recaptcha";
        case MI_CAPTCHA_HCAPTCHA: return "h-captcha";
        case MI_CAPTCHA_TURNSTILE: return "cf-turnstile";
        default: return NULL;
    }
}

/* Escape a string for a single-quoted JS literal. */
static void js_escape(mi_json_buf *b, const char *s) {
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
            case '\'': mi_json_raw(b, "\\'"); break;
            case '\\': mi_json_raw(b, "\\\\"); break;
            case '\n': mi_json_raw(b, "\\n"); break;
            case '\r': mi_json_raw(b, "\\r"); break;
            case '<': mi_json_raw(b, "\\x3c"); break; /* never close a script tag */
            default: {
                char ch[2] = {(char)*p, 0};
                mi_json_raw(b, ch);
            }
        }
    }
}

/* The `data-callback` name on the widget, or NULL. */
static char *callback_of(const mi_html_node *doc, const char *marker) {
    for (const mi_html_node *n = mi_html_next(doc, doc); n; n = mi_html_next(doc, n)) {
        if (n->type != MI_HTML_ELEMENT) continue;
        const char *cls = mi_html_attr(n, "class");
        if (!cls || !strstr(cls, marker)) continue;
        const char *cb = mi_html_attr(n, "data-callback");
        if (cb && *cb) return mi_strdup(cb);
    }
    return NULL;
}

/* Whether the page carries a widget of this kind. */
static int has_widget(const mi_html_node *doc, const char *marker) {
    for (const mi_html_node *n = mi_html_next(doc, doc); n; n = mi_html_next(doc, n)) {
        if (n->type != MI_HTML_ELEMENT) continue;
        const char *cls = mi_html_attr(n, "class");
        if (cls && strstr(cls, marker)) return 1;
        /* A Turnstile/hCaptcha may be a plain div with the attribute. */
        if (mi_html_attr(n, "data-sitekey") && mi_html_is(n, "div")) return 1;
    }
    return 0;
}

char *mi_captcha_inject_js(const mi_html_node *doc, mi_captcha_kind kind,
                           const char *token) {
    if (!doc || !token || kind == MI_CAPTCHA_NONE) return NULL;
    const char *marker = widget_marker(kind);
    if (!marker) return NULL;
    if (!has_widget(doc, marker)) return NULL;
    const char *field = mi_captcha_response_field(kind);
    char *callback = callback_of(doc, marker);

    mi_json_buf b;
    mi_json_init(&b);
    /*
     * The script, as one IIFE. It fills every element that carries the response
     * field name — the widget creates a hidden input *and* a textarea, and a
     * site may read either — then runs the page's own callback, which is what
     * tells the widget the challenge passed.
     */
    mi_json_raw(&b, "(function(){var t='");
    js_escape(&b, token);
    mi_json_raw(&b, "';var f=document.querySelectorAll('[name=\"");
    mi_json_raw(&b, field);
    mi_json_raw(&b, "\"]');for(var i=0;i<f.length;i++){f[i].value=t;}"
        "if(window.___grecaptcha_cfg&&window.grecaptcha&&grecaptcha.getResponse){try{grecaptcha.getResponse();}catch(e){}}");
    if (callback) {
        /* Call the page's own callback by name, so the widget registers success. */
        mi_json_raw(&b, ";try{if(typeof window['");
        js_escape(&b, callback);
        mi_json_raw(&b, "']==='function'){window['");
        js_escape(&b, callback);
        mi_json_raw(&b, "'](t);}}catch(e){}");
    }
    mi_json_raw(&b, ";return f.length>0;})()");
    free(callback);
    char *out = mi_strdup(mi_json_data(&b));
    mi_json_free(&b);
    return out;
}
