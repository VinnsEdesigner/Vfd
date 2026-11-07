/* mi_impersonate.h - fetch a page through a real browser's TLS fingerprint.
 *
 * The engine's own HTTP client is libcurl, which is honest about being libcurl.
 * Some sites fingerprint the TLS handshake (JA3/JA4) and refuse anything that is
 * not a browser, whatever User-Agent it claims — a check no header can pass.
 *
 * `curl_cffi` replays a real browser's handshake, so the request looks like
 * Chrome all the way down. It is Python, so it is run as a small helper program
 * rather than linked in: the engine stays C and libcurl, and the one thing it
 * cannot do is delegated to the tool that can. This module is the C side of that
 * call — find the helper, run it, read the file it wrote.
 */
#ifndef MI_IMPERSONATE_H
#define MI_IMPERSONATE_H

#include <stddef.h>

typedef struct {
    /*
     * The helper program. NULL searches, in order: $MI_IMPERSONATE_HELPER,
     * `fetch-impersonate` on PATH, then the engine's own install location. A
     * path ending in `.py` is run with python3.
     */
    const char *helper_path;
    /* The browser to imitate, e.g. "chrome" or "chrome131". NULL is "chrome". */
    const char *impersonate;
    /* A proxy the helper's request goes through. NULL is direct. */
    const char *proxy;
    /* A User-Agent to send, when the caller wants a specific one. NULL is the
     * browser the fingerprint already implies. */
    const char *user_agent;
    /* Netscape cookies, as text, to send with the request. Written to a 0600
     * temp file for the helper and removed after. NULL sends none. */
    const char *cookies_text;
    /* Seconds the helper may take. 0 uses a default. */
    int timeout_seconds;
} mi_impersonate_config;

/*
 * Whether the helper can be found, so a caller can decide whether to try it.
 * `resolved` (optional) receives the path found, owned by the caller.
 */
int mi_impersonate_available(const mi_impersonate_config *cfg, char **resolved);

/*
 * Fetch `url` and return the body (owned by the caller), or NULL with `err`
 * filled. `max_bytes` 0 means no cap.
 *
 * When `meta_out` is given it receives an owned JSON object describing the
 * response — `{"final_url":…,"headers":["X-Frame-Options: DENY", …]}` — so the
 * caller can apply the same framing rule it applies to its own fetches. It is
 * NULL on failure or when the helper returned no headers.
 */
char *mi_impersonate_fetch(const mi_impersonate_config *cfg, const char *url,
                           size_t max_bytes, char **meta_out, char *err, size_t errcap);

/*
 * The impersonation targets this host can offer, as a JSON array string (owned by
 * the caller), or NULL when the helper is absent or fails.
 *
 * Asked of the helper, which is the one that imports curl_cffi and so knows what
 * the installed version supports. The caller can then offer a list rather than
 * asking a user to type a target and guess.
 */
char *mi_impersonate_list_targets(const mi_impersonate_config *cfg);

#endif
