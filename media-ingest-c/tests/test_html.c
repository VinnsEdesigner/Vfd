/* test_html.c - the tolerant HTML DOM.
 *
 * Real pages are malformed, and the parser has to nest them the way a browser
 * does or a selector finds the wrong node. Each case here is a shape that
 * actually appears on the web.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* The first element under root with this tag, or NULL. */
static mi_html_node *find(mi_html_node *root, const char *tag) {
    for (mi_html_node *n = mi_html_next(root, root); n; n = mi_html_next(root, n)) {
        if (mi_html_is(n, tag)) return n;
    }
    return NULL;
}

static void test_basic_tree(void) {
    const char *html = "<html><head><title>Hi</title></head>"
                       "<body><div id=\"a\" class=\"box big\"><p>Hello <b>world</b></p></div></body></html>";
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    CHECK(doc != NULL, "parses");
    mi_html_node *div = find(doc, "div");
    CHECK(div != NULL, "finds the div");
    CHECK(div && !strcmp(mi_html_attr(div, "id"), "a"), "id attribute");
    CHECK(div && !strcmp(mi_html_attr(div, "class"), "box big"), "class attribute");
    char *t = mi_html_text(div);
    CHECK(t && !strcmp(t, "Hello world"), "descendant text is concatenated");
    free(t);
    mi_html_free(doc);
}

/* A void element never becomes a parent. */
static void test_void(void) {
    const char *html = "<div><img src=\"x.png\"><br><span>after</span></div>";
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    mi_html_node *div = find(doc, "div");
    CHECK(div != NULL, "finds div");
    /* br and img are siblings of span, all children of div. */
    int kids = 0;
    for (mi_html_node *c = div->first_child; c; c = c->next_sibling)
        if (c->type == MI_HTML_ELEMENT) kids++;
    CHECK(kids == 3, "img, br and span are all children of div");
    mi_html_node *span = find(doc, "span");
    CHECK(span && mi_html_is(span->parent, "div"), "span did not nest inside img");
    mi_html_free(doc);
}

/* Unclosed <li> elements nest as siblings, not as a chain. */
static void test_implicit_close_li(void) {
    const char *html = "<ul><li>one<li>two<li>three</ul>";
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    mi_html_node *ul = find(doc, "ul");
    int lis = 0;
    for (mi_html_node *c = ul->first_child; c; c = c->next_sibling)
        if (mi_html_is(c, "li")) lis++;
    CHECK(lis == 3, "three li are siblings");
    mi_html_node *first = ul->first_child;
    CHECK(mi_html_is(first, "li") && !mi_html_is(first->first_child, "li"),
          "the second li is not a child of the first");
    mi_html_free(doc);
}

/* A <p> is closed by a following block element. */
static void test_implicit_close_p(void) {
    const char *html = "<div><p>para<div>block</div></div>";
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    mi_html_node *outer = find(doc, "div");
    int divs = 0;
    for (mi_html_node *n = mi_html_next(doc, doc); n; n = mi_html_next(doc, n))
        if (mi_html_is(n, "div")) divs++;
    CHECK(divs == 2, "the inner div is a sibling of the p, not inside it");
    mi_html_node *inner = outer->first_child;
    while (inner && !mi_html_is(inner, "div")) inner = inner->next_sibling;
    CHECK(inner != NULL, "the inner div is a child of the outer");
    mi_html_free(doc);
}

/* Table cells close each other even with no end tags. */
static void test_table_cells(void) {
    const char *html = "<table><tr><td>A<td>B</tr><tr><td>C</tr></table>";
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    mi_html_node *tr = find(doc, "tr");
    int tds = 0;
    for (mi_html_node *c = tr->first_child; c; c = c->next_sibling)
        if (mi_html_is(c, "td")) tds++;
    CHECK(tds == 2, "two td are siblings in the row");
    char *a = mi_html_text(tr->first_child);
    CHECK(a && !strcmp(a, "A"), "the first cell holds only A");
    free(a);
    mi_html_free(doc);
}

/* A '<' inside a script is not a tag. */
static void test_raw_text_script(void) {
    const char *html = "<div><script>if (a < b) { x = \"<div>\"; }</script><p>after</p></div>";
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    mi_html_node *p = find(doc, "p");
    CHECK(p != NULL, "the <p> after the script is found");
    char *t = mi_html_text(p);
    CHECK(t && !strcmp(t, "after"), "the script content did not leak out");
    free(t);
    mi_html_node *script = find(doc, "script");
    char *st = mi_html_text(script);
    CHECK(st && strstr(st, "a < b") != NULL, "the script body is kept as text");
    free(st);
    mi_html_free(doc);
}

static void test_entities(void) {
    const char *html = "<p>Tom &amp; Jerry &lt;3 &#233;t&#xe9; &nbsp;end</p>";
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    mi_html_node *p = find(doc, "p");
    char *t = mi_html_text(p);
    CHECK(t && strstr(t, "Tom & Jerry <3") != NULL, "named entities decode");
    CHECK(t && strstr(t, "\xc3\xa9") != NULL, "numeric entities decode to UTF-8");
    free(t);
    mi_html_free(doc);
}

/* Own text is the direct text only, not descendants. */
static void test_own_text(void) {
    const char *html = "<td>Price: <span>$9</span></td>";
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    mi_html_node *td = find(doc, "td");
    char *own = mi_html_own_text(td);
    char *all = mi_html_text(td);
    CHECK(own && !strcmp(own, "Price:"), "own text excludes the nested span");
    CHECK(all && !strcmp(all, "Price: $9"), "all text includes it");
    free(own); free(all);
    mi_html_free(doc);
}

static void test_comments_and_doctype(void) {
    const char *html = "<!DOCTYPE html><!-- a note --><p>x</p>";
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    int doctype = 0, comment = 0;
    for (mi_html_node *c = doc->first_child; c; c = c->next_sibling) {
        if (c->type == MI_HTML_DOCTYPE) doctype = 1;
        if (c->type == MI_HTML_COMMENT) comment = 1;
    }
    CHECK(doctype, "the doctype is kept");
    CHECK(comment, "the comment is kept");
    mi_html_free(doc);
}

static void test_empty_and_null(void) {
    mi_html_node *doc = mi_html_parse("", 0);
    CHECK(doc != NULL && doc->first_child == NULL, "empty input yields an empty document");
    mi_html_free(doc);
}

int main(void) {
    test_basic_tree();
    test_void();
    test_implicit_close_li();
    test_implicit_close_p();
    test_table_cells();
    test_raw_text_script();
    test_entities();
    test_own_text();
    test_comments_and_doctype();
    test_empty_and_null();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
