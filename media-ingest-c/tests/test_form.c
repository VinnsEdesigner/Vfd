/* test_form.c - reading a page's forms, finding the login, and the CSRF token.
 *
 * The CSRF token is the part a naive login gets wrong: it is a hidden field the
 * page set, and it must be carried back unchanged. These fixtures are the shapes
 * real login forms take.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mi_form.h"
#include "mi_html.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);   \
        }                                                                       \
    } while (0)

static mi_forms parse(const char *html, const char *url) {
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    mi_forms f;
    mi_forms_extract(doc, url, &f);
    mi_html_free(doc);
    return f;
}

static void test_basic_form(void) {
    const char *html =
        "<form action=\"/login\" method=\"post\">"
        "<input type=\"text\" name=\"user\">"
        "<input type=\"password\" name=\"pass\">"
        "<input type=\"submit\" value=\"Go\">"
        "</form>";
    mi_forms f = parse(html, "https://example.com/page");
    CHECK(f.count == 1, "one form");
    CHECK(!strcmp(f.forms[0].action, "https://example.com/login"), "the action is resolved");
    CHECK(!strcmp(f.forms[0].method, "post"), "the method");
    /* The submit input has no name, so it is not a submittable field. */
    CHECK(f.forms[0].field_count == 2, "the two named fields");
    mi_forms_free(&f);
}

static void test_find_login(void) {
    const char *html =
        "<form action=\"/search\" method=\"get\"><input name=\"q\"></form>"
        "<form action=\"/login\" method=\"post\">"
        "<input name=\"username\"><input type=\"password\" name=\"pw\">"
        "<input type=\"hidden\" name=\"csrf_token\" value=\"tok123\">"
        "</form>";
    mi_forms f = parse(html, "https://example.com/");
    CHECK(f.count == 2, "two forms");
    const mi_form *login = mi_forms_find_login(&f);
    CHECK(login != NULL, "the login is found");
    CHECK(login && !strcmp(login->action, "https://example.com/login"),
          "and it is the login, not the search");
    mi_forms_free(&f);
}

static void test_csrf(void) {
    const char *html =
        "<form action=\"/login\" method=\"post\">"
        "<input name=\"user\"><input type=\"password\" name=\"pw\">"
        "<input type=\"hidden\" name=\"csrf_token\" value=\"tok123\">"
        "</form>";
    mi_forms f = parse(html, "https://example.com/");
    const mi_form *login = mi_forms_find_login(&f);
    char *name = NULL;
    const char *tok = mi_form_csrf_token(login, &name);
    CHECK(tok && !strcmp(tok, "tok123"), "the CSRF token is read from the page");
    CHECK(name && !strcmp(name, "csrf_token"), "and its name");
    mi_forms_free(&f);
}

/* The other names sites use. */
static void test_csrf_names(void) {
    const char *names[] = {"_token", "authenticity_token",
                           "__RequestVerificationToken", "_csrf", NULL};
    for (size_t i = 0; names[i]; i++) {
        char html[512];
        snprintf(html, sizeof html,
                 "<form action=\"/login\" method=\"post\">"
                 "<input name=\"u\"><input type=\"password\" name=\"p\">"
                 "<input type=\"hidden\" name=\"%s\" value=\"T\">"
                 "</form>", names[i]);
        mi_forms f = parse(html, "https://example.com/");
        const mi_form *login = mi_forms_find_login(&f);
        const char *tok = mi_form_csrf_token(login, NULL);
        CHECK(tok && !strcmp(tok, "T"), names[i]);
        mi_forms_free(&f);
    }
}

static void test_encode(void) {
    const char *html =
        "<form action=\"/login\" method=\"post\">"
        "<input name=\"user\" value=\"\">"
        "<input type=\"password\" name=\"pw\">"
        "<input type=\"hidden\" name=\"csrf_token\" value=\"tok123\">"
        "<input type=\"checkbox\" name=\"remember\" value=\"1\">" /* unchecked */
        "</form>";
    mi_forms f = parse(html, "https://example.com/");
    const mi_form *login = mi_forms_find_login(&f);
    mi_form_override ov[] = {{"user", "alice@example.com"}, {"pw", "s3cret"}};
    char *body = mi_form_encode(login, ov, 2);
    /* The username and password are the caller's, the token keeps the page's
     * value, and the unchecked box is not sent. */
    CHECK(body && strstr(body, "user=alice%40example.com") != NULL, "the username is encoded");
    CHECK(body && strstr(body, "pw=s3cret") != NULL, "the password");
    CHECK(body && strstr(body, "csrf_token=tok123") != NULL, "the CSRF token is carried");
    CHECK(body && strstr(body, "remember") == NULL, "an unchecked box is not sent");
    free(body);
    mi_forms_free(&f);
}

/* A GET search form: the same shape, submitted as a query. */
static void test_search_form(void) {
    const char *html =
        "<form action=\"/search\" method=\"get\">"
        "<input type=\"text\" name=\"q\"><input type=\"submit\" value=\"Search\">"
        "</form>";
    mi_forms f = parse(html, "https://example.com/");
    CHECK(f.count == 1 && !strcmp(f.forms[0].method, "get"), "a GET form");
    mi_form_override ov[] = {{"q", "hello world"}};
    char *q = mi_form_encode(&f.forms[0], ov, 1);
    CHECK(q && !strcmp(q, "q=hello+world"), "a space becomes '+' in a query");
    free(q);
    mi_forms_free(&f);
}

int main(void) {
    test_basic_form();
    test_find_login();
    test_csrf();
    test_csrf_names();
    test_encode();
    test_search_form();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
