/* test_css.c - the CSS selector engine over the DOM.
 *
 * Each selector here is one a real extraction rule would use. The subtle cases —
 * descendant vs child, a class as a whole word, nth-child with `2n+1`, `:not`,
 * a group — are the ones a hand-rolled matcher gets wrong.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mi_css.h"
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

/* Count matches of `sel` in `html`. */
static size_t count_matches(const char *html, const char *sel) {
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    char err[128];
    mi_css_selector *s = mi_css_compile(sel, err, sizeof err);
    if (!s) { fprintf(stderr, "  compile failed: %s (%s)\n", sel, err); mi_html_free(doc); return (size_t)-1; }
    mi_html_node **out = NULL;
    size_t n = 0;
    mi_css_select(s, doc, &out, &n);
    free(out);
    mi_css_free(s);
    mi_html_free(doc);
    return n;
}

static void test_tag_class_id(void) {
    const char *html = "<div class=\"box\"><p id=\"a\" class=\"x y\">1</p>"
                       "<p class=\"y\">2</p><span class=\"x\">3</span></div>";
    CHECK(count_matches(html, "p") == 2, "tag selector");
    CHECK(count_matches(html, ".x") == 2, "class selector");
    CHECK(count_matches(html, "#a") == 1, "id selector");
    CHECK(count_matches(html, "p.x") == 1, "tag + class");
    CHECK(count_matches(html, ".x.y") == 1, "two classes on one element");
    /* 7, not 4: the HTML5 parser implies <html>, <head> and <body>, so the
     * fragment's four elements are joined by three the page did not write. */
    CHECK(count_matches(html, "*") == 7, "universal matches every element");
}

/* A class is a whole word: `x` must not match `xx`. */
static void test_class_word(void) {
    const char *html = "<div class=\"xx xy\"></div><div class=\"x\"></div>";
    CHECK(count_matches(html, ".x") == 1, "a class matches as a whole word only");
    CHECK(count_matches(html, ".xx") == 1, "a longer class matches its own element");
}

static void test_combinators(void) {
    const char *html = "<div class=\"a\"><section><p>deep</p></section></div>"
                       "<p>shallow</p>";
    CHECK(count_matches(html, "div p") == 1, "descendant finds the nested p");
    CHECK(count_matches(html, "div > p") == 0, "child does not match a grandchild");
    CHECK(count_matches(html, "div > section > p") == 1, "chained child");
    /* The outer p follows the div, so they are adjacent siblings. */
    CHECK(count_matches(html, "div + p") == 1, "adjacent sibling");
    CHECK(count_matches(html, "section + p") == 0,
          "section and the outer p are not siblings");
    CHECK(count_matches(html, "div ~ p") == 1, "general sibling");
}

static void test_attributes(void) {
    const char *html = "<a href=\"http://x/a.pdf\" data-kind=\"file\">1</a>"
                       "<a href=\"http://x/b.html\">2</a>"
                       "<a title=\"hi there\">3</a>";
    CHECK(count_matches(html, "[href]") == 2, "has attribute");
    CHECK(count_matches(html, "[href$=\".pdf\"]") == 1, "suffix match");
    CHECK(count_matches(html, "[href^=\"http\"]") == 2, "prefix match");
    CHECK(count_matches(html, "[href*=\"x/\"]") == 2, "contains match");
    CHECK(count_matches(html, "[data-kind=\"file\"]") == 1, "equals match");
    CHECK(count_matches(html, "a[title]") == 1, "tag + attribute");
}

static void test_pseudos(void) {
    const char *html = "<ul><li>1</li><li>2</li><li>3</li><li>4</li></ul>";
    CHECK(count_matches(html, "li:first-child") == 1, "first-child");
    CHECK(count_matches(html, "li:last-child") == 1, "last-child");
    CHECK(count_matches(html, "li:nth-child(2)") == 1, "nth-child(2)");
    CHECK(count_matches(html, "li:nth-child(odd)") == 2, "nth-child(odd)");
    CHECK(count_matches(html, "li:nth-child(even)") == 2, "nth-child(even)");
    CHECK(count_matches(html, "li:nth-child(2n+1)") == 2, "nth-child(2n+1)");
    CHECK(count_matches(html, "li:nth-child(n+3)") == 2, "nth-child(n+3)");
}

static void test_not(void) {
    const char *html = "<p class=\"a\">1</p><p class=\"b\">2</p><p>3</p>";
    CHECK(count_matches(html, "p:not(.a)") == 2, ":not excludes a class");
    CHECK(count_matches(html, "p:not(.a):not(.b)") == 1, "chained :not");
}

static void test_group(void) {
    const char *html = "<h1>t</h1><h2>s</h2><p>b</p>";
    CHECK(count_matches(html, "h1, h2") == 2, "a group matches any of its chains");
    CHECK(count_matches(html, "h1, h2, p") == 3, "a three-way group");
}

static void test_bad_selector(void) {
    char err[128];
    CHECK(mi_css_compile("", err, sizeof err) == NULL, "empty selector is refused");
    CHECK(mi_css_compile("div >", err, sizeof err) == NULL, "a dangling combinator is refused");
    CHECK(mi_css_compile("p:madeup", err, sizeof err) == NULL, "an unknown pseudo is refused");
    CHECK(mi_css_compile("[unclosed", err, sizeof err) == NULL, "an unclosed attribute is refused");
    /* A refusal names the problem. */
    mi_css_compile("p:madeup", err, sizeof err);
    CHECK(err[0] != 0, "the refusal has a reason");
}

int main(void) {
    test_tag_class_id();
    test_class_word();
    test_combinators();
    test_attributes();
    test_pseudos();
    test_not();
    test_group();
    test_bad_selector();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
