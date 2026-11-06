/* mi_session.h - log in to a site, and keep the session alive.
 *
 * Static cookies get a crawler onto a site it already has a session on. A
 * *login* is different: the crawler posts credentials to a form, the site
 * answers with a session cookie, and every later request carries it. That is a
 * three-step dance, and this is the module that performs it:
 *
 *   1. GET the login page — which sets the pre-login cookies and carries the
 *      anti-CSRF token;
 *   2. POST the form, with the caller's credentials and *the token the page
 *      set*, to the form's own action;
 *   3. keep the response's cookies in a jar the fetcher then uses.
 *
 * The credentials come from the caller (the settings endpoint or the
 * environment), are never logged, and are never written to disk by this module.
 * A login is reported as a yes/no with a reason, so an operator can tell a wrong
 * password from a site that changed its form.
 *
 * This is deliberately *not* a browser: it posts a form and follows the cookie.
 * A site whose login is built by JavaScript, or that needs a captcha, is the
 * case the interactive browser session is for; this covers the form that is
 * still a form.
 */
#ifndef MI_SESSION_H
#define MI_SESSION_H

#include <stddef.h>
#include "mi_cookiejar.h"
#include "mi_download.h"
#include "mi_form.h"

typedef enum {
    MI_LOGIN_OK = 0,
    MI_LOGIN_NO_FORM,      /* the page had no login form */
    MI_LOGIN_FETCH_FAILED, /* the login page could not be fetched */
    MI_LOGIN_POST_FAILED,  /* the credentials could not be posted */
    MI_LOGIN_REJECTED,     /* the site answered, and did not accept the login */
} mi_login_status;

const char *mi_session_status_name(mi_login_status s);

typedef struct {
    /* The username field's name, when the caller knows it. NULL uses the
     * heuristic (a field whose name looks like a username). */
    const char *username_field;
    /* The password field's name. NULL uses the first password field. */
    const char *password_field;
    /* Extra fields to send, e.g. a "remember me" checkbox or a second factor. */
    const struct mi_form_override *extra;
    size_t extra_count;
    /*
     * Whether the login is accepted only when the response *differs* from the
     * login page — the common shape, where a failed login re-renders the form.
     * On by default; a site that redirects on success and 200s on failure is
     * covered by the redirect check below.
     */
    int require_change;
} mi_login_options;

typedef struct {
    mi_login_status status;
    /* A sentence for the log; never contains the credentials. */
    char *detail;
    /* The URL the site ended on, when it redirected. */
    char *final_url;
} mi_login_result;

void mi_login_result_free(mi_login_result *r);

/*
 * Log in to `login_url` with `username`/`password`, keeping the session cookies
 * in `jar`.
 *
 * `cfg` is the fetch configuration (proxy, impersonation, cookies, user agent).
 * The caller's static cookies are seeded into `jar` first when the config has
 * them, so a site that needs one to even show the form works.
 *
 * On success the jar holds the session and the caller's fetches use it.
 */
mi_login_result mi_session_login(mi_cookiejar *jar, const mi_download_config *cfg,
                                 const char *login_url, const char *username,
                                 const char *password, const mi_login_options *opts);

/*
 * Whether a page is still the login form — i.e. the session has lapsed.
 *
 * A session that expires leaves the crawler on a login page, which would be
 * crawled as content. This is the check that catches it.
 */
int mi_session_is_login_page(const mi_download_config *cfg, const char *url);

#endif
