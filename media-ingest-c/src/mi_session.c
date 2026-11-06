#define _GNU_SOURCE
#include "mi_session.h"
#include "mi_form.h"
#include "mi_html.h"
#include "mi_util.h"

#include <stdlib.h>
#include <string.h>

const char *mi_session_status_name(mi_login_status s) {
    switch (s) {
        case MI_LOGIN_OK: return "ok";
        case MI_LOGIN_NO_FORM: return "no_form";
        case MI_LOGIN_FETCH_FAILED: return "fetch_failed";
        case MI_LOGIN_POST_FAILED: return "post_failed";
        case MI_LOGIN_REJECTED: return "rejected";
    }
    return "unknown";
}

void mi_login_result_free(mi_login_result *r) {
    if (!r) return;
    free(r->detail);
    free(r->final_url);
    memset(r, 0, sizeof *r);
}

/* The form to use: the login heuristic, unless the caller named the fields, in
 * which case any form with a password field will do. */
static const mi_form *choose_form(const mi_forms *forms, const mi_login_options *opts) {
    if (opts && opts->username_field && opts->password_field) {
        for (size_t i = 0; i < forms->count; i++) {
            const mi_form *fm = &forms->forms[i];
            int has_user = 0, has_pass = 0;
            for (size_t j = 0; j < fm->field_count; j++) {
                if (!strcmp(fm->fields[j].name, opts->username_field)) has_user = 1;
                if (!strcmp(fm->fields[j].name, opts->password_field)) has_pass = 1;
            }
            if (has_user && has_pass) return fm;
        }
    }
    return mi_forms_find_login(forms);
}

/* The username and password field names to use. */
static void choose_fields(const mi_form *form, const mi_login_options *opts,
                          const char **user_out, const char **pass_out) {
    *user_out = opts ? opts->username_field : NULL;
    *pass_out = opts ? opts->password_field : NULL;
    if (*pass_out && *user_out) return;
    for (size_t i = 0; i < form->field_count; i++) {
        const mi_form_field *f = &form->fields[i];
        if (!*pass_out && f->kind == MI_FIELD_PASSWORD) *pass_out = f->name;
        if (!*user_out &&
            (f->kind == MI_FIELD_TEXT || f->kind == MI_FIELD_EMAIL)) {
            /* The first text/email field before the password is the username. */
            *user_out = f->name;
        }
    }
}

mi_login_result mi_session_login(mi_cookiejar *jar, const mi_download_config *cfg,
                                 const char *login_url, const char *username,
                                 const char *password, const mi_login_options *opts) {
    mi_login_result res;
    memset(&res, 0, sizeof res);

    if (!jar || !cfg || !login_url || !username || !password) {
        res.status = MI_LOGIN_FETCH_FAILED;
        res.detail = mi_strdup("missing jar, config, url or credentials");
        return res;
    }

    /* The caller's static cookies seed the jar, so a site that needs one to show
     * the form works. */
    if (cfg->cookies_text && *cfg->cookies_text)
        mi_cookiejar_load_netscape(jar, cfg->cookies_text);

    mi_download_config c = *cfg;
    c.jar = jar;

    /* 1. The login page: sets the pre-login cookies and carries the token. */
    mi_fetch_info info;
    char *page = mi_download_fetch_document(&c, login_url, 4 * 1024 * 1024, &info);
    if (!page) {
        mi_fetch_info_free(&info);
        res.status = MI_LOGIN_FETCH_FAILED;
        res.detail = mi_asprintf("could not fetch the login page at %s", login_url);
        return res;
    }
    char *pre_final = info.final_url ? mi_strdup(info.final_url) : mi_strdup(login_url);
    mi_fetch_info_free(&info);

    /* 2. The form, the fields, and the anti-CSRF token the page set. */
    mi_html_node *doc = mi_html_parse(page, strlen(page));
    mi_forms forms;
    mi_forms_extract(doc, pre_final, &forms);
    const mi_form *form = choose_form(&forms, opts);
    if (!form) {
        mi_forms_free(&forms);
        mi_html_free(doc);
        free(page);
        free(pre_final);
        res.status = MI_LOGIN_NO_FORM;
        res.detail = mi_strdup("the page has no login form");
        return res;
    }

    const char *user_field = NULL, *pass_field = NULL;
    choose_fields(form, opts, &user_field, &pass_field);
    /* Own copies: the names point into the form, which is freed below. */
    char *user_field_owned = user_field ? mi_strdup(user_field) : NULL;
    char *pass_field_owned = pass_field ? mi_strdup(pass_field) : NULL;
    if (!user_field || !pass_field) {
        mi_forms_free(&forms);
        mi_html_free(doc);
        free(page);
        free(pre_final);
        res.status = MI_LOGIN_NO_FORM;
        res.detail = mi_strdup("the login form has no username or password field");
        return res;
    }

    /* The overrides: the credentials, plus any the caller added. The form's own
     * fields — including the CSRF token — keep the page's values. */
    size_t extra_n = opts ? opts->extra_count : 0;
    mi_form_override *ov = mi_calloc(2 + extra_n, sizeof *ov);
    ov[0].name = user_field;
    ov[0].value = username;
    ov[1].name = pass_field;
    ov[1].value = password;
    for (size_t i = 0; i < extra_n; i++) ov[2 + i] = opts->extra[i];
    char *body = mi_form_encode(form, ov, 2 + extra_n);
    free(ov);

    /* The action, and the method. A login is almost always a POST, but a site
     * may use GET; both are honoured. */
    char *action = mi_strdup(form->action);
    int is_post = strcasecmp(form->method, "post") == 0;
    char *enctype = form->enctype ? mi_strdup(form->enctype) : NULL;

    /* Whether the page had a CSRF token, for the detail line. */
    char *csrf_name = NULL;
    const char *csrf = mi_form_csrf_token(form, &csrf_name);

    mi_forms_free(&forms);
    mi_html_free(doc);

    /* 3. Submit. */
    char *result_page = NULL;
    if (is_post) {
        const char *ct = enctype && strstr(enctype, "multipart")
                             ? "multipart/form-data"
                             : "application/x-www-form-urlencoded";
        result_page = mi_download_post(&c, action, body, ct, 4 * 1024 * 1024, &info);
    } else {
        char *sep = strchr(action, '?') ? "&" : "?";
        char *full = mi_asprintf("%s%s%s", action, sep, body);
        result_page = mi_download_fetch_document(&c, full, 4 * 1024 * 1024, &info);
        free(full);
    }
    free(body);
    free(enctype);

    if (!result_page) {
        mi_fetch_info_free(&info);
        res.status = MI_LOGIN_POST_FAILED;
        res.detail = mi_asprintf("could not POST the credentials to %s", action);
        free(action);
        free(pre_final);
        free(page);
        return res;
    }
    char *post_final = info.final_url ? mi_strdup(info.final_url) : mi_strdup(action);
    mi_fetch_info_free(&info);

    /*
     * Did it work? A failed login usually re-renders the login form, and a
     * successful one either redirects or shows different content. Two signals:
     * the site redirected away from the login page, or the result is no longer a
     * login form. Either is a success.
     */
    int redirected = post_final && pre_final && strcmp(post_final, pre_final) != 0;
    mi_html_node *rdoc = mi_html_parse(result_page, strlen(result_page));
    mi_forms rforms;
    mi_forms_extract(rdoc, post_final, &rforms);
    const mi_form *still_login = choose_form(&rforms, opts);
    mi_forms_free(&rforms);
    mi_html_free(rdoc);

    int changed = !still_login;
    int require_change = !opts || opts->require_change;
    int ok = redirected || (changed && require_change) ||
             (!require_change && !still_login);

    if (ok) {
        res.status = MI_LOGIN_OK;
        res.final_url = post_final;
        post_final = NULL;
        res.detail = mi_asprintf(
            "logged in (username field '%s'%s)", user_field_owned ? user_field_owned : "?",
            csrf ? ", CSRF token carried" : ", no CSRF token on the form");
    } else {
        res.status = MI_LOGIN_REJECTED;
        res.detail = mi_strdup("the site re-rendered the login form; "
                               "the credentials were refused or a second step is needed");
    }

    free(post_final);
    free(result_page);
    free(action);
    free(pre_final);
    free(page);
    free(user_field_owned);
    free(pass_field_owned);
    return res;
}

int mi_session_is_login_page(const mi_download_config *cfg, const char *url) {
    mi_download_config c = *cfg;
    char *page = mi_download_fetch_document(&c, url, 2 * 1024 * 1024, NULL);
    if (!page) return 0;
    mi_html_node *doc = mi_html_parse(page, strlen(page));
    mi_forms forms;
    mi_forms_extract(doc, url, &forms);
    int is_login = mi_forms_find_login(&forms) != NULL;
    mi_forms_free(&forms);
    mi_html_free(doc);
    free(page);
    return is_login;
}
