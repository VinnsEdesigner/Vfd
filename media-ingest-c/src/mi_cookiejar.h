/* mi_cookiejar.h - a live cookie store, the thing a session actually needs.
 *
 * The engine's cookies are a static Netscape blob the operator pastes in: one
 * export, fixed until replaced. That is enough for "a site I already have a
 * session on", and useless for a *login* — a login sets cookies in the response,
 * the site expects them back on the next request, and it rotates them as it
 * goes. This is the store that makes that work.
 *
 * It is a small RFC 6265 jar, the parts that matter:
 *
 *   - `Set-Cookie` is parsed (name, value, Domain, Path, Expires, Max-Age,
 *     Secure, HttpOnly) and stored;
 *   - the `Cookie` header for a request is built from the cookies whose domain
 *     and path match, most-specific path first, as the spec orders them;
 *   - a cookie is replaced when the same name/domain/path is set again — which
 *     is how a login *rotates* a session id;
 *   - an expired cookie is removed.
 *
 * A jar can be seeded from a Netscape file (the operator's export) and exported
 * back to one, so the static-cookie path and the live-session path share a shape.
 */
#ifndef MI_COOKIEJAR_H
#define MI_COOKIEJAR_H

#include <stddef.h>

typedef struct {
    char *name;
    char *value;
    char *domain; /* without a leading dot; matched as a suffix */
    char *path;
    long expires; /* unix seconds, or 0 for a session cookie */
    int secure;
    int http_only;
} mi_cookie;

typedef struct mi_cookiejar mi_cookiejar;

mi_cookiejar *mi_cookiejar_new(void);
void mi_cookiejar_free(mi_cookiejar *jar);

/*
 * Store one `Set-Cookie` header value for `request_host` and `request_path`.
 *
 * `host` is the host of the request that carried the header; it is what a
 * host-only cookie (no Domain attribute) is scoped to. A cookie whose Domain
 * does not cover `host` is refused, as a browser refuses it.
 */
void mi_cookiejar_set(mi_cookiejar *jar, const char *set_cookie,
                      const char *request_host, const char *request_path);

/*
 * Parse a whole response header block and store every Set-Cookie it carries.
 * `headers` is the raw block (lines separated by CRLF).
 */
void mi_cookiejar_set_from_headers(mi_cookiejar *jar, const char *headers,
                                   const char *request_host, const char *request_path);

/*
 * The `Cookie` header value for a request to `url`, or NULL when none applies.
 * Owned by the caller.
 */
char *mi_cookiejar_header_for_url(mi_cookiejar *jar, const char *url);

/* How many cookies are held (for reporting). */
size_t mi_cookiejar_count(const mi_cookiejar *jar);

/* Whether a cookie of this name is held for a host. */
int mi_cookiejar_has(const mi_cookiejar *jar, const char *host, const char *name);

/* Load a Netscape cookie file's text (the operator's export). */
void mi_cookiejar_load_netscape(mi_cookiejar *jar, const char *text);

/* Export the jar as Netscape text, for reporting or reuse. Owned by the caller. */
char *mi_cookiejar_to_netscape(const mi_cookiejar *jar);

#endif
