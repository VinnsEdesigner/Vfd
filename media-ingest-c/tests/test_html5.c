/* test_html5.c - the HTML5 tree-construction algorithm.
 *
 * The cases here are the ones the ad-hoc parser gets wrong and the spec gets
 * right: implicit head/body, foster parenting around a table, an unclosed
 * formatting element spanning a block, and the RCDATA/RAWTEXT elements.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mi_html.h"
#include "mi_html5.h"

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

static mi_html_node *find(mi_html_node *root, const char *tag) {
    for (mi_html_node *n = mi_html_next(root, root); n; n = mi_html_next(root, n))
        if (mi_html_is(n, tag)) return n;
    return NULL;
}

/* An implicit html/head/body appear even when the page omits them. */
static void test_implied_structure(void) {
    const char *html = "<title>T</title><p>Hi</p>";
    mi_html_node *doc = mi_html5_parse(html, strlen(html), NULL);
    CHECK(doc != NULL, "parses");
    CHECK(find(doc, "html") != NULL, "an html element is implied");
    CHECK(find(doc, "head") != NULL, "a head is implied");
    CHECK(find(doc, "body") != NULL, "a body is implied");
    mi_html_node *title = find(doc, "title");
    CHECK(title != NULL, "the title is in the head");
    CHECK(title && title->parent && mi_html_is(title->parent, "head"),
          "the title is a child of head");
    mi_html_node *p = find(doc, "p");
    CHECK(p && p->parent && mi_html_is(p->parent, "body"), "the p is in the body");
    mi_html_free(doc);
}

/* A <tr> without a <tbody> gets one, as a browser does. */
static void test_implied_tbody(void) {
    const char *html = "<table><tr><td>a</td></tr></table>";
    mi_html_node *doc = mi_html5_parse(html, strlen(html), NULL);
    mi_html_node *table = find(doc, "table");
    CHECK(table != NULL, "the table parses");
    mi_html_node *tbody = find(doc, "tbody");
    CHECK(tbody != NULL, "a tbody is implied");
    CHECK(tbody && tbody->parent == table, "the tbody is a child of the table");
    mi_html_node *tr = find(doc, "tr");
    CHECK(tr && tr->parent == tbody, "the tr is a child of the tbody");
    mi_html_free(doc);
}

/* Text that is not table content is foster-parented before the table. */
static void test_foster_parenting(void) {
    const char *html = "<div><table>oops<tr><td>a</td></tr></table></div>";
    mi_html_node *doc = mi_html5_parse(html, strlen(html), NULL);
    mi_html_node *div = find(doc, "div");
    mi_html_node *table = find(doc, "table");
    CHECK(div && table, "the div and table parse");
    CHECK(table && table->parent == div, "the table is in the div");
    /* "oops" must be before the table, not inside it. */
    CHECK(div && div->first_child && div->first_child->type == MI_HTML_TEXT,
          "the stray text is foster-parented before the table");
    CHECK(div && div->first_child && div->first_child->next_sibling == table,
          "and it sits immediately before the table");
    mi_html_free(doc);
}

/*
 * The adoption agency, on the canonical mis-nested case. `<p>1<b>2<i>3</b>4</i>`
 * has the <b> closed while the <i> is still open inside it; the spec splits them
 * so the result nests as a browser nests it, rather than leaving <i> inside a
 * closed <b>.
 */
static void test_adoption_agency(void) {
    const char *html = "<p>1<b>2<i>3</b>4</i>5</p>";
    mi_html_node *doc = mi_html5_parse(html, strlen(html), NULL);
    mi_html_node *p = find(doc, "p");
    mi_html_node *b = find(doc, "b");
    CHECK(p && b, "the p and b parse");
    /*
     * The spec's output is <p>1<b>2<i>3</i></b><i>4</i>5</p>: the <b> is closed
     * at its end tag, and the <i> that was still open is *reopened* as a sibling
     * holding "4" — that reopening is what the adoption agency is for.
     */
    int i_count = 0;
    mi_html_node *second_i = NULL;
    for (mi_html_node *n = mi_html_next(doc, doc); n; n = mi_html_next(doc, n)) {
        if (mi_html_is(n, "i")) { i_count++; if (i_count == 2) second_i = n; }
    }
    CHECK(i_count == 2, "the <i> was closed and reopened, so there are two");
    CHECK(second_i && second_i->parent && !mi_html_is(second_i->parent, "b"),
          "the reopened <i> is a sibling of the <b>, not inside it");
    char *t = mi_html_text(p);
    CHECK(t && !strcmp(t, "12345"), "the text is intact and in order");
    free(t);
    mi_html_free(doc);
}

/* A table inside a table closes the outer one. */
static void test_nested_table(void) {
    const char *html = "<table><tr><td>a<table><tr><td>b</td></tr></table></td></tr></table>";
    mi_html_node *doc = mi_html5_parse(html, strlen(html), NULL);
    int tables = 0;
    for (mi_html_node *n = mi_html_next(doc, doc); n; n = mi_html_next(doc, n))
        if (mi_html_is(n, "table")) tables++;
    CHECK(tables == 2, "both tables are present");
    mi_html_free(doc);
}

/* A comment and a doctype are kept. */
static void test_doctype_comment(void) {
    const char *html = "<!DOCTYPE html><!-- hi --><p>x</p>";
    mi_html_node *doc = mi_html5_parse(html, strlen(html), NULL);
    int doctype = 0, comment = 0;
    for (mi_html_node *c = doc->first_child; c; c = c->next_sibling) {
        if (c->type == MI_HTML_DOCTYPE) doctype = 1;
        if (c->type == MI_HTML_COMMENT) comment = 1;
    }
    CHECK(doctype, "the doctype is a document child");
    CHECK(comment, "the comment is kept");
    mi_html_free(doc);
}

/* A <script> with '<' inside does not break the tree. */
static void test_rawtext(void) {
    const char *html = "<body><script>if (a < b) x = '</div>';</script><p>after</p></body>";
    mi_html_node *doc = mi_html5_parse(html, strlen(html), NULL);
    mi_html_node *script = find(doc, "script");
    CHECK(script != NULL, "the script parses");
    char *st = mi_html_text(script);
    CHECK(st && strstr(st, "a < b") != NULL, "the script body is raw text");
    free(st);
    mi_html_node *p = find(doc, "p");
    CHECK(p != NULL, "the p after the script is found");
    mi_html_free(doc);
}

/* Attributes are lowercased; duplicate attributes keep the first. */
static void test_attributes(void) {
    const char *html = "<div ID=\"a\" Class=\"B\" class=\"C\" data-x='1'>x</div>";
    mi_html_node *doc = mi_html5_parse(html, strlen(html), NULL);
    mi_html_node *div = find(doc, "div");
    CHECK(div != NULL, "the div parses");
    CHECK(div && !strcmp(mi_html_attr(div, "id"), "a"), "an attribute name is lowercased");
    CHECK(div && !strcmp(mi_html_attr(div, "class"), "B"),
          "the first of a duplicate attribute wins");
    CHECK(div && !strcmp(mi_html_attr(div, "data-x"), "1"), "a single-quoted value");
    mi_html_free(doc);
}

/* An unclosed list item nests as a sibling, as the spec says. */
static void test_li(void) {
    const char *html = "<ul><li>one<li>two</ul>";
    mi_html_node *doc = mi_html5_parse(html, strlen(html), NULL);
    mi_html_node *ul = find(doc, "ul");
    int lis = 0;
    for (mi_html_node *c = ul->first_child; c; c = c->next_sibling)
        if (mi_html_is(c, "li")) lis++;
    CHECK(lis == 2, "two li are siblings");
    mi_html_free(doc);
}

/* A stray </br> becomes a <br>, as the spec says. */
static void test_br(void) {
    const char *html = "<body><p>a</br>b</p></body>";
    mi_html_node *doc = mi_html5_parse(html, strlen(html), NULL);
    int brs = 0;
    for (mi_html_node *n = mi_html_next(doc, doc); n; n = mi_html_next(doc, n))
        if (mi_html_is(n, "br")) brs++;
    CHECK(brs == 1, "a stray </br> makes a br");
    mi_html_free(doc);
}

int main(void) {
    test_implied_structure();
    test_implied_tbody();
    test_foster_parenting();
    test_adoption_agency();
    test_nested_table();
    test_doctype_comment();
    test_rawtext();
    test_attributes();
    test_li();
    test_br();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
