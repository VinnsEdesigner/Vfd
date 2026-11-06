/* test_structured.c - reading the data a page publishes about itself.
 *
 * One fixture per format, because each has its own markup shape, plus the two
 * properties that make the output usable: a nested value is dotted, and a
 * repeated name yields several pairs (a product with several offers).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mi_html.h"
#include "mi_structured.h"

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

static void test_json_ld(void) {
    const char *html =
        "<html><head><script type=\"application/ld+json\">"
        "{\"@context\":\"https://schema.org\",\"@type\":\"Product\","
        "\"name\":\"Widget\",\"offers\":{\"@type\":\"Offer\",\"price\":\"9.99\","
        "\"priceCurrency\":\"USD\"},\"brand\":{\"name\":\"Acme\"}}"
        "</script></head><body></body></html>";
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    mi_props p;
    mi_structured_extract(doc, &p);
    CHECK(mi_props_get(&p, "name") && !strcmp(mi_props_get(&p, "name"), "Widget"),
          "a top-level JSON-LD string");
    CHECK(mi_props_get(&p, "@type") && !strcmp(mi_props_get(&p, "@type"), "Product"),
          "the type");
    CHECK(mi_props_get(&p, "offers.price") &&
              !strcmp(mi_props_get(&p, "offers.price"), "9.99"),
          "a nested object is dotted, and a number is text");
    CHECK(mi_props_get(&p, "brand.name") &&
              !strcmp(mi_props_get(&p, "brand.name"), "Acme"),
          "a second nested object");
    mi_props_free(&p);
    mi_html_free(doc);
}

static void test_json_ld_array(void) {
    const char *html =
        "<script type=\"application/ld+json\">"
        "{\"@type\":\"Product\",\"name\":\"W\",\"offers\":["
        "{\"price\":\"1\"},{\"price\":\"2\"}]}</script>";
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    mi_props p;
    mi_structured_extract(doc, &p);
    CHECK(mi_props_count(&p, "offers.price") == 2, "an array yields two pairs");
    CHECK(!strcmp(mi_props_get_n(&p, "offers.price", 0), "1"), "the first");
    CHECK(!strcmp(mi_props_get_n(&p, "offers.price", 1), "2"), "the second");
    mi_props_free(&p);
    mi_html_free(doc);
}

static void test_opengraph(void) {
    const char *html =
        "<html><head>"
        "<meta property=\"og:title\" content=\"My Page\">"
        "<meta property=\"og:image\" content=\"https://x/i.png\">"
        "<meta name=\"twitter:card\" content=\"summary\">"
        "<meta name=\"description\" content=\"not structured\">"
        "</head><body></body></html>";
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    mi_props p;
    mi_structured_extract(doc, &p);
    CHECK(mi_props_get(&p, "og:title") && !strcmp(mi_props_get(&p, "og:title"), "My Page"),
          "an og: property");
    CHECK(mi_props_get(&p, "og:image") != NULL, "og:image");
    CHECK(mi_props_get(&p, "twitter:card") != NULL, "a twitter: name");
    CHECK(mi_props_get(&p, "description") == NULL,
          "a plain meta description is not structured data");
    mi_props_free(&p);
    mi_html_free(doc);
}

static void test_microdata(void) {
    const char *html =
        "<div itemscope itemtype=\"https://schema.org/Product\">"
        "<span itemprop=\"name\">Widget</span>"
        "<a itemprop=\"url\" href=\"https://x/p\">link</a>"
        "<div itemprop=\"offers\" itemscope itemtype=\"https://schema.org/Offer\">"
        "<meta itemprop=\"price\" content=\"9.99\">"
        "</div></div>";
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    mi_props p;
    mi_structured_extract(doc, &p);
    CHECK(mi_props_get(&p, "name") && !strcmp(mi_props_get(&p, "name"), "Widget"),
          "a microdata itemprop reads the element's text");
    CHECK(mi_props_get(&p, "url") && !strcmp(mi_props_get(&p, "url"), "https://x/p"),
          "an itemprop on an anchor reads its href");
    CHECK(mi_props_get(&p, "offers.price") &&
              !strcmp(mi_props_get(&p, "offers.price"), "9.99"),
          "a nested itemscope is dotted, and a meta reads its content");
    mi_props_free(&p);
    mi_html_free(doc);
}

static void test_rdfa(void) {
    const char *html =
        "<div vocab=\"https://schema.org/\" typeof=\"Person\">"
        "<span property=\"name\">Jane</span></div>";
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    mi_props p;
    mi_structured_extract(doc, &p);
    CHECK(mi_props_get(&p, "name") && !strcmp(mi_props_get(&p, "name"), "Jane"),
          "an RDFa property");
    mi_props_free(&p);
    mi_html_free(doc);
}

static void test_json_output(void) {
    const char *html =
        "<meta property=\"og:title\" content=\"T\">"
        "<script type=\"application/ld+json\">{\"a\":\"1\",\"b\":\"2\"}</script>";
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    mi_props p;
    mi_structured_extract(doc, &p);
    char *json = mi_props_to_json(&p);
    CHECK(json && strstr(json, "\"og:title\":\"T\"") != NULL, "a single value is a string");
    CHECK(json && strstr(json, "\"a\":\"1\"") != NULL, "a JSON-LD field");
    free(json);
    mi_props_free(&p);
    mi_html_free(doc);
}

static void test_empty(void) {
    const char *html = "<html><body><p>just a page</p></body></html>";
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    mi_props p;
    mi_structured_extract(doc, &p);
    CHECK(p.count == 0, "a page with no structured data yields none");
    mi_props_free(&p);
    mi_html_free(doc);
}

int main(void) {
    test_json_ld();
    test_json_ld_array();
    test_opengraph();
    test_microdata();
    test_rdfa();
    test_json_output();
    test_empty();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
