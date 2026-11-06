/* mi_browser_headers.h - the header set a real browser sends, made coherent.
 *
 * A site that checks header *coherence* — a UA that claims Chrome but no
 * `sec-ch-ua`, or a `sec-ch-ua` that disagrees with the UA — blocks the request
 * however good the TLS fingerprint is. The impersonation helper gets this for
 * free from curl_cffi; the engine's own libcurl fetch sends almost none of it,
 * which is a bot signature by itself.
 *
 * This is that header set in one place, so a libcurl fetch can wear the same
 * coherent set a browser would, and so the two paths cannot drift apart. The
 * headers are derived from the browser family, so they always agree with each
 * other and with the UA.
 */
#ifndef MI_BROWSER_HEADERS_H
#define MI_BROWSER_HEADERS_H

#include <stddef.h>

/*
 * The browser family a header set is for. Only Chromium sends `sec-ch-ua` and
 * its friends, so the family decides whether they are present at all — sending
 * them from Firefox would be as much a tell as omitting them from Chrome.
 */
typedef enum {
    MI_BROWSER_CHROME = 0,
    MI_BROWSER_FIREFOX,
    MI_BROWSER_SAFARI,
} mi_browser_family;

/* The family named by an impersonation target ("chrome131", "firefox", …). */
mi_browser_family mi_browser_family_of(const char *impersonate);

/* The major version to advertise, e.g. "131". NULL uses a recent default. */
const char *mi_browser_default_version(void);

/*
 * Fill `out` with the ordered "Name: value" header lines a browser of `family`
 * at `version` would send for a top-level navigation.
 *
 * `user_agent` is the UA the caller will send, so `sec-ch-ua` can be built from
 * the same major version and the two cannot disagree. `out`/`out_cap` is a
 * caller array of pointers; the strings are owned by the caller and freed with
 * `mi_browser_headers_free`. Returns the number written.
 *
 * The order matters — real browsers send a stable order and some sites check it.
 */
size_t mi_browser_headers(mi_browser_family family, const char *version,
                          const char *user_agent, char **out, size_t out_cap);

/* Free a header set produced above (up to `n` entries). */
void mi_browser_headers_free(char **headers, size_t n);

#endif
