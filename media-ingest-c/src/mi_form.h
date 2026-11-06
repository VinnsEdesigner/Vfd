/* mi_form.h - find a page's forms, and fill and submit one.
 *
 * A login is a form: a page with a `<form>` that posts a username and a
 * password, plus a hidden anti-CSRF token the server set. Submitting it by hand
 * means reading the form's action, method and every field it expects — including
 * the hidden ones, which is exactly the part a naive login gets wrong.
 *
 * This reads a parsed page and returns its forms with every field, so a caller
 * can fill the ones it knows and leave the rest as the page set them. That is
 * what makes the CSRF token work: it is read from the page, not guessed.
 *
 * It also handles the search-interface case: the same shape, a form with a
 * single text field, submitted by GET.
 */
#ifndef MI_FORM_H
#define MI_FORM_H

#include <stddef.h>
#include "mi_html.h"

/* A field value the caller supplies, overriding what the form had. */
typedef struct mi_form_override {
    const char *name;
    const char *value;
} mi_form_override;

typedef enum {
    MI_FIELD_TEXT = 0,
    MI_FIELD_PASSWORD,
    MI_FIELD_HIDDEN,
    MI_FIELD_EMAIL,
    MI_FIELD_SUBMIT,
    MI_FIELD_CHECKBOX,
    MI_FIELD_FILE,
    MI_FIELD_OTHER,
} mi_form_field_kind;

typedef struct {
    char *name;
    char *value;      /* as the page set it; "" for an empty field */
    mi_form_field_kind kind;
    /* 1 when a checkbox/radio is checked, so a caller knows whether to send it. */
    int checked;
} mi_form_field;

typedef struct {
    /* The form's action, resolved against the page URL. Never NULL; an action
     * of "" resolves to the page itself. */
    char *action;
    char *method; /* "get" or "post", lowercased */
    /* The form's enctype, or NULL. */
    char *enctype;
    mi_form_field *fields;
    size_t field_count;
    /* The form's own id/name, for telling one form from another. */
    char *id;
    char *name;
} mi_form;

typedef struct {
    mi_form *forms;
    size_t count;
} mi_forms;

void mi_forms_free(mi_forms *f);

/*
 * Every form on the page, with its fields. `page_url` resolves the action.
 * Returns 0 and fills `out`.
 */
int mi_forms_extract(const mi_html_node *doc, const char *page_url, mi_forms *out);

/*
 * The form most likely to be a login: it has a password field, and preferably a
 * field whose name looks like a username. Returns NULL when there is none.
 *
 * This is a heuristic on purpose — a login form has no machine-readable marker,
 * and every site spells its fields differently (`user`, `username`, `email`,
 * `login`). A caller that knows the site should name the fields itself; this is
 * the default that works on the common shapes.
 */
const mi_form *mi_forms_find_login(const mi_forms *forms);

/*
 * The value of a form's anti-CSRF token, or NULL.
 *
 * A CSRF token is a hidden field whose name the site chose — `csrf_token`,
 * `_token`, `authenticity_token`, `__RequestVerificationToken`, `_csrf`, and
 * many more. The reliable way to find it is not by name but by looking for a
 * hidden field that the page *just set*; short of that, the common names are
 * checked. `name_out` (optional) receives the field's name.
 */
const char *mi_form_csrf_token(const mi_form *form, char **name_out);

/*
 * URL-encode a form's fields into a request body, with `overrides` applied: a
 * field whose name matches an override takes the override's value, and a field
 * the form did not have is appended. This is how a caller supplies the username
 * and password while every hidden field keeps the page's value.
 *
 * `overrides` is an array of {name, value} pairs. Owned by the caller.
 */
char *mi_form_encode(const mi_form *form, const mi_form_override *overrides,
                     size_t override_count);

#endif
