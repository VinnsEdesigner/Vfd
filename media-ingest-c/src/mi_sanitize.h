/* mi_sanitize.h - make a rendered page safe to put in a frame.
 *
 * WHAT THIS IS AND IS NOT
 *
 * This is *hygiene*, not a security boundary. The boundary is the frame the
 * console puts this in: `sandbox=""` gives the document an opaque origin with
 * no scripts, no forms and no way to reach the console or Vyzor, and that
 * is enforced by the browser. A hand-written HTML parser is not a thing to trust
 * with a security property, so the guarantee is not placed here.
 *
 * What this does add is a second, independent guarantee and a much smaller
 * payload:
 *
 *   - an allowlist. Only named tags and attributes survive; anything else is
 *     dropped. An allowlist is used deliberately: a denylist is a list of the
 *     attacks someone thought of.
 *   - the *content* of script, style, noscript, template, svg, math and canvas
 *     is removed rather than parsed, because those are the classic vectors and
 *     there is no reason to keep what is inside them.
 *   - attribute values are escaped on the way out, so a value cannot end its
 *     own quotes and become a new attribute.
 *   - `javascript:`, `vbscript:` and `data:` are refused in URL attributes.
 *   - a strict Content-Security-Policy is injected, which is a browser-enforced
 *     rule rather than a parsing one: `default-src 'none'` with narrow
 *     allowances. Even a sanitiser bug cannot make a script run under it.
 *
 * Links are rewritten to route back through the renderer, so clicking a link
 * re-renders rather than navigating the frame away from the app.
 */
#ifndef MI_SANITIZE_H
#define MI_SANITIZE_H

#include <stddef.h>

typedef struct {
    /* Prefix that an in-page link is rewritten to, e.g. "/api/v1/render?url=".
     * NULL leaves links alone. */
    const char *link_prefix;
    /* The URL the document came from, so a relative link can be made absolute
     * before it is handed to the prefix. NULL skips rewriting. */
    const char *base_url;
    /* Extra CSS to allow from, beyond inline styles. Empty means inline only. */
    const char *extra_style_src;
} mi_sanitize_config;

/*
 * Sanitise `html`, returning a new NUL-terminated document. Caller frees.
 *
 * Never returns NULL for a non-NULL input: a document that cannot be sanitised
 * is not a reason to fail a render, and an empty result is the safe answer.
 */
char *mi_sanitize_html(const char *html, size_t len, const mi_sanitize_config *cfg);

/* The policy injected into a sanitised document. Exposed so it can be asserted
 * in a test rather than only observed through a browser. */
const char *mi_sanitize_csp(void);

#endif /* MI_SANITIZE_H */
