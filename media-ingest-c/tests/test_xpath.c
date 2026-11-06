/* test_xpath.c - the XPath subset, over the DOM.
 *
 * The expressions here are the ones extraction rules are actually written in:
 * a descendant search, a child step, an attribute predicate, a position, and the
 * two value selectors (`/@attr`, `/text()`).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mi_html.h"
#include "mi_xpath.h"

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

static size_t count(const char *html, const char *expr) {
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    char err[128];
    mi_xpath *x = mi_xpath_compile(expr, err, sizeof err);
    if (!x) { fprintf(stderr, "  compile failed: %s (%s)\n", expr, err); mi_html_free(doc); return (size_t)-1; }
    mi_html_node **out = NULL;
    size_t n = 0;
    mi_xpath_select(x, doc, &out, &n);
    free(out);
    mi_xpath_free(x);
    mi_html_free(doc);
    return n;
}

static void test_paths(void) {
    const char *html = "<html><body><div><p>a</p><p>b</p></div><p>c</p></body></html>";
    CHECK(count(html, "//p") == 3, "descendant search finds all p");
    CHECK(count(html, "/html/body/p") == 1, "absolute path, child steps");
    CHECK(count(html, "//div/p") == 2, "a child step under a descendant");
    CHECK(count(html, "//*") == 6, "wildcard counts every element");
}

static void test_predicates(void) {
    const char *html =
        "<div class=\"box\"><a href=\"/1\" class=\"x\">1</a>"
        "<a href=\"/2\">2</a><a href=\"/3\">3</a></div>";
    CHECK(count(html, "//a[@href]") == 3, "has-attribute predicate");
    CHECK(count(html, "//a[@class='x']") == 1, "attribute-equals predicate");
    CHECK(count(html, "//a[contains(@href,'2')]") == 1, "contains()");
    CHECK(count(html, "//a[starts-with(@href,'/')]") == 3, "starts-with()");
    CHECK(count(html, "//a[1]") == 1, "position predicate");
    CHECK(count(html, "//a[last()]") == 1, "last() predicate");
}

static void test_value_selectors(void) {
    const char *html = "<div><a href=\"/x\">the link</a></div>";
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    char err[128];

    mi_xpath *x = mi_xpath_compile("//a/@href", err, sizeof err);
    CHECK(x != NULL, "an attribute selector compiles");
    CHECK(mi_xpath_is_attribute(x), "it is flagged as an attribute selector");
    CHECK(!strcmp(mi_xpath_result_attribute(x), "href"), "the attribute name is kept");
    mi_html_node **out = NULL;
    size_t n = 0;
    mi_xpath_select(x, doc, &out, &n);
    CHECK(n == 1, "it yields the containing node");
    CHECK(n == 1 && !strcmp(mi_html_attr(out[0], "href"), "/x"), "whose attr is readable");
    free(out);
    mi_xpath_free(x);

    x = mi_xpath_compile("//a/text()", err, sizeof err);
    CHECK(x && mi_xpath_is_text(x), "a text() selector compiles and is flagged");
    mi_xpath_free(x);

    mi_html_free(doc);
}

static void test_bad(void) {
    char err[128];
    CHECK(mi_xpath_compile("", err, sizeof err) == NULL, "empty is refused");
    CHECK(mi_xpath_compile("//", err, sizeof err) == NULL, "no step is refused");
    CHECK(mi_xpath_compile("//a[", err, sizeof err) == NULL, "an unclosed predicate is refused");
    CHECK(mi_xpath_compile("//a[bogus()]", err, sizeof err) == NULL, "an unknown function is refused");
    CHECK(mi_xpath_compile("//a//b", err, sizeof err) == NULL, "a mid-path // is refused");
}

int main(void) {
    test_paths();
    test_predicates();
    test_value_selectors();
    test_bad();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
