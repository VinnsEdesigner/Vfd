/* test_extract_rows.c - turning a page into rows, by rule.
 *
 * The point of this module is that a person can say "one row per product, with
 * its name, price and link" and get CSV out. The cases here are the shapes that
 * appear on real pages: a repeated card, a table, a field that is sometimes
 * missing, and a relative link that must come out absolute.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mi_extract_rows.h"

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

static const char *PAGE =
    "<html><body>"
    "<div class=\"product\"><h2 class=\"name\">Widget</h2>"
      "<span class=\"price\">$9.99</span><a href=\"/w/1\">buy</a></div>"
    "<div class=\"product\"><h2 class=\"name\">Gadget</h2>"
      "<span class=\"price\">$19.50</span><a href=\"/w/2\">buy</a></div>"
    "<div class=\"product\"><h2 class=\"name\">Thing</h2>"
      "<a href=\"https://x/3\">buy</a></div>"  /* no price */
    "</body></html>";

static void test_cards(void) {
    const char *rule =
        "{\"container\":\".product\",\"fields\":["
        "{\"name\":\"title\",\"selector\":\".name\",\"from\":\"text\"},"
        "{\"name\":\"price\",\"selector\":\".price\",\"from\":\"text\",\"default\":\"\"},"
        "{\"name\":\"url\",\"selector\":\"a\",\"from\":\"href\"}]}";
    char err[200];
    mi_row_rule *r = mi_row_rule_parse(rule, err, sizeof err);
    CHECK(r != NULL, "the rule parses");
    if (!r) { fprintf(stderr, "  (%s)\n", err); return; }

    mi_rows rows;
    CHECK(mi_extract_rows(r, PAGE, strlen(PAGE), "https://shop.example/list",
                          &rows, err, sizeof err) == 0, "extraction runs");
    CHECK(rows.row_count == 3, "one row per product");
    CHECK(rows.name_count == 3, "three columns");
    CHECK(!strcmp(rows.rows[0].values[0], "Widget"), "first title");
    CHECK(!strcmp(rows.rows[0].values[1], "$9.99"), "first price");
    CHECK(!strcmp(rows.rows[1].values[0], "Gadget"), "second title");
    /* The relative link came out absolute against the page URL. */
    CHECK(!strcmp(rows.rows[0].values[2], "https://shop.example/w/1"),
          "a relative href is resolved");
    CHECK(!strcmp(rows.rows[2].values[2], "https://x/3"),
          "an absolute href is left alone");
    /* The missing price is empty, not absent. */
    CHECK(!strcmp(rows.rows[2].values[1], ""), "a missing field is empty");
    mi_rows_free(&rows);
    mi_row_rule_free(r);
}

static void test_csv(void) {
    const char *rule =
        "{\"container\":\".product\",\"fields\":["
        "{\"name\":\"title\",\"selector\":\".name\"},"
        "{\"name\":\"price\",\"selector\":\".price\"}]}";
    char err[200];
    mi_row_rule *r = mi_row_rule_parse(rule, err, sizeof err);
    mi_rows rows;
    mi_extract_rows(r, PAGE, strlen(PAGE), NULL, &rows, err, sizeof err);
    char *csv = mi_rows_to_csv(&rows);
    CHECK(csv && !strncmp(csv, "title,price\r\n", 13), "the CSV header");
    CHECK(csv && strstr(csv, "Widget,$9.99") != NULL, "a CSV row");
    CHECK(csv && strstr(csv, "Thing,\r\n") != NULL, "a missing value is an empty field");
    free(csv);
    mi_rows_free(&rows);
    mi_row_rule_free(r);
}

static void test_csv_quoting(void) {
    const char *page = "<div class=\"p\"><b>a,b</b><i>he said \"hi\"</i></div>";
    const char *rule =
        "{\"container\":\".p\",\"fields\":["
        "{\"name\":\"a\",\"selector\":\"b\"},{\"name\":\"b\",\"selector\":\"i\"}]}";
    char err[200];
    mi_row_rule *r = mi_row_rule_parse(rule, err, sizeof err);
    mi_rows rows;
    mi_extract_rows(r, page, strlen(page), NULL, &rows, err, sizeof err);
    char *csv = mi_rows_to_csv(&rows);
    CHECK(csv && strstr(csv, "\"a,b\"") != NULL, "a comma forces quoting");
    CHECK(csv && strstr(csv, "\"he said \"\"hi\"\"\"") != NULL, "a quote is doubled");
    free(csv);
    mi_rows_free(&rows);
    mi_row_rule_free(r);
}

static void test_table(void) {
    const char *page =
        "<table><tr><th>Name</th><th>Size</th></tr>"
        "<tr><td>a.txt</td><td>10</td></tr>"
        "<tr><td>b.txt</td><td>20</td></tr></table>";
    /* The rows after the header: `tr:nth-child(n+2)`, cells by position. */
    const char *rule =
        "{\"container\":\"table tr:nth-child(n+2)\",\"fields\":["
        "{\"name\":\"file\",\"selector\":\"td:nth-child(1)\"},"
        "{\"name\":\"size\",\"selector\":\"td:nth-child(2)\"}]}";
    char err[200];
    mi_row_rule *r = mi_row_rule_parse(rule, err, sizeof err);
    mi_rows rows;
    mi_extract_rows(r, page, strlen(page), NULL, &rows, err, sizeof err);
    CHECK(rows.row_count == 2, "two data rows, header skipped");
    CHECK(rows.row_count == 2 && !strcmp(rows.rows[0].values[0], "a.txt"), "first cell");
    CHECK(rows.row_count == 2 && !strcmp(rows.rows[1].values[1], "20"), "second row size");
    mi_rows_free(&rows);
    mi_row_rule_free(r);
}

/* An XPath field works alongside CSS. */
static void test_xpath_field(void) {
    const char *page = "<div class=\"p\"><a href=\"/x\">link</a></div>";
    const char *rule =
        "{\"container\":\".p\",\"fields\":["
        "{\"name\":\"href\",\"xpath\":\"//a/@href\"},"
        "{\"name\":\"text\",\"xpath\":\"//a/text()\"}]}";
    char err[200];
    mi_row_rule *r = mi_row_rule_parse(rule, err, sizeof err);
    mi_rows rows;
    mi_extract_rows(r, page, strlen(page), NULL, &rows, err, sizeof err);
    /* An /@attr result yields the containing node; the module reads the attr. */
    CHECK(rows.row_count == 1, "one row");
    CHECK(rows.row_count == 1 && !strcmp(rows.rows[0].values[1], "link"),
          "an xpath text() field");
    mi_rows_free(&rows);
    mi_row_rule_free(r);
}

static void test_whole_page(void) {
    const char *page = "<html><head><title>A Title</title></head><body><h1>Hi</h1></body></html>";
    const char *rule = "{\"fields\":[{\"name\":\"title\",\"selector\":\"title\"}]}";
    char err[200];
    mi_row_rule *r = mi_row_rule_parse(rule, err, sizeof err);
    mi_rows rows;
    mi_extract_rows(r, page, strlen(page), NULL, &rows, err, sizeof err);
    CHECK(rows.row_count == 1, "no container means one row for the page");
    CHECK(rows.row_count == 1 && !strcmp(rows.rows[0].values[0], "A Title"),
          "the page title");
    mi_rows_free(&rows);
    mi_row_rule_free(r);
}

static void test_bad_rule(void) {
    char err[200];
    CHECK(mi_row_rule_parse("{", err, sizeof err) == NULL, "invalid JSON is refused");
    CHECK(mi_row_rule_parse("{\"fields\":[]}", err, sizeof err) == NULL,
          "an empty fields array is refused");
    CHECK(mi_row_rule_parse("{\"fields\":[{\"selector\":\"p\"}]}", err, sizeof err) == NULL,
          "a field with no name is refused");
    CHECK(mi_row_rule_parse("{\"fields\":[{\"name\":\"x\"}]}", err, sizeof err) == NULL,
          "a field with no selector is refused");
    CHECK(mi_row_rule_parse(
              "{\"fields\":[{\"name\":\"x\",\"selector\":\"a\",\"from\":\"attr\"}]}",
              err, sizeof err) == NULL,
          "an attr field with no attr name is refused");
}

int main(void) {
    test_cards();
    test_csv();
    test_csv_quoting();
    test_table();
    test_xpath_field();
    test_whole_page();
    test_bad_rule();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
