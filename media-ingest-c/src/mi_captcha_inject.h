/* mi_captcha_inject.h - put a solved captcha token into the live page.
 *
 * A solver returns a token string. That token is not enough on its own: each
 * captcha widget keeps its own state and only reports success when its *own*
 * callback runs or its hidden response field is filled. Dropping the string into
 * the DOM and hoping is the mistake a naive integration makes — the page still
 * sees an unsolved captcha and the form refuses to submit.
 *
 * This builds the JavaScript that plants a token the way the widget expects:
 *
 *   reCAPTCHA v2   fill `g-recaptcha-response` (and its textarea), then call the
 *                  `data-callback` the widget was given.
 *   reCAPTCHA v3   the same response field, no widget to click.
 *   hCaptcha       fill `h-captcha-response`, call `data-callback`.
 *   Turnstile      fill `cf-turnstile-response`, call `data-callback`.
 *
 * The page's own callback name is read from the markup — it is per-site, so
 * guessing it is how an injection silently does nothing.
 *
 * The JS is generated here and run by the browser session (`mi_interactive_eval`
 * on the interactive pool); this module is pure so the JS can be tested without
 * a browser.
 */
#ifndef MI_CAPTCHA_INJECT_H
#define MI_CAPTCHA_INJECT_H

#include <stddef.h>
#include "mi_captcha.h"
#include "mi_html.h"

/*
 * JavaScript that plants `token` for the challenge found on `doc`.
 *
 * Returns an owned string, or NULL when the page holds no challenge of that
 * kind. The token is escaped for a JS string literal.
 */
char *mi_captcha_inject_js(const mi_html_node *doc, mi_captcha_kind kind,
                           const char *token);

/*
 * The response-field name a kind uses, e.g. "g-recaptcha-response". Never NULL.
 * Exposed so a caller can check the field is present before solving.
 */
const char *mi_captcha_response_field(mi_captcha_kind kind);

#endif
