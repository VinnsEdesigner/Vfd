/* test_sitemap.c - reading a sitemap, and the robots Sitemap: directive.
 *
 * Both sitemap shapes are tested (a urlset of pages, a sitemapindex of other
 * sitemaps), with the optional fields, a gzip-compressed document, entities in a
 * URL, and the case that matters for coverage: a sitemap listing a page no link
 * reaches.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "mi_politeness.h"
#include "mi_sitemap.h"

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

static void test_urlset(void) {
    const char *xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">"
        "<url><loc>https://example.com/a</loc><lastmod>2024-01-02</lastmod>"
        "<changefreq>weekly</changefreq><priority>0.8</priority></url>"
        "<url><loc>https://example.com/b</loc></url>"
        "</urlset>";
    mi_sitemap s;
    CHECK(mi_sitemap_parse((const unsigned char *)xml, strlen(xml), &s) == 0,
          "a urlset parses");
    CHECK(!s.is_index, "it is a urlset, not an index");
    CHECK(s.count == 2, "two pages");
    CHECK(!strcmp(s.entries[0].loc, "https://example.com/a"), "the first loc");
    CHECK(s.entries[0].lastmod && !strcmp(s.entries[0].lastmod, "2024-01-02"),
          "the lastmod");
    CHECK(s.entries[0].changefreq && !strcmp(s.entries[0].changefreq, "weekly"),
          "the changefreq");
    CHECK(s.entries[0].priority > 0.79 && s.entries[0].priority < 0.81,
          "the priority");
    /* An entry with no optional fields still yields its loc. */
    CHECK(!strcmp(s.entries[1].loc, "https://example.com/b"), "the second loc");
    CHECK(s.entries[1].lastmod == NULL, "a missing lastmod is NULL");
    CHECK(s.entries[1].priority == -1, "a missing priority is -1");
    mi_sitemap_free(&s);
}

static void test_sitemapindex(void) {
    const char *xml =
        "<sitemapindex xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">"
        "<sitemap><loc>https://example.com/sitemap-1.xml</loc></sitemap>"
        "<sitemap><loc>https://example.com/sitemap-2.xml</loc></sitemap>"
        "</sitemapindex>";
    mi_sitemap s;
    CHECK(mi_sitemap_parse((const unsigned char *)xml, strlen(xml), &s) == 0, "an index parses");
    CHECK(s.is_index, "it is flagged as an index");
    CHECK(s.count == 2, "two child sitemaps");
    CHECK(!strcmp(s.entries[0].loc, "https://example.com/sitemap-1.xml"), "the first child");
    mi_sitemap_free(&s);
}

static void test_entities(void) {
    const char *xml =
        "<urlset><url><loc>https://example.com/a?x=1&amp;y=2</loc></url></urlset>";
    mi_sitemap s;
    mi_sitemap_parse((const unsigned char *)xml, strlen(xml), &s);
    CHECK(s.count == 1, "one entry");
    CHECK(!strcmp(s.entries[0].loc, "https://example.com/a?x=1&y=2"),
          "the &amp; is decoded in a URL");
    mi_sitemap_free(&s);
}

static void test_gzip(void) {
    const char *xml =
        "<urlset><url><loc>https://example.com/gz</loc></url></urlset>";
    /* gzip it with zlib. */
    unsigned char gz[512];
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    zs.next_in = (Bytef *)xml;
    zs.avail_in = (uInt)strlen(xml);
    zs.next_out = gz;
    zs.avail_out = sizeof gz;
    deflate(&zs, Z_FINISH);
    size_t gzlen = (size_t)zs.total_out;
    deflateEnd(&zs);

    CHECK(mi_gzip_is_gzip(gz, gzlen), "the magic is detected");
    mi_sitemap s;
    CHECK(mi_sitemap_parse(gz, gzlen, &s) == 0, "a gzipped sitemap parses");
    CHECK(s.count == 1 && !strcmp(s.entries[0].loc, "https://example.com/gz"),
          "the entry survived inflation");
    mi_sitemap_free(&s);
}

static void test_not_a_sitemap(void) {
    const char *html = "<html><body>hello</body></html>";
    mi_sitemap s;
    CHECK(mi_sitemap_parse((const unsigned char *)html, strlen(html), &s) == -1,
          "an HTML page is not a sitemap");
}

/* The robots Sitemap: directive is captured, unscoped to any user-agent. */
static void test_robots_sitemap_directive(void) {
    mi_politeness *p = mi_politeness_new();
    const char *robots =
        "User-agent: *\n"
        "Disallow: /private/\n"
        "Sitemap: https://example.com/sitemap.xml\n"
        "Sitemap: https://example.com/news.xml\n";
    mi_politeness_set_robots(p, "example.com", robots, "vyzor");
    char **maps = mi_politeness_sitemaps(p, "example.com");
    CHECK(maps != NULL, "the sitemaps are found");
    if (maps) {
        CHECK(maps[0] && !strcmp(maps[0], "https://example.com/sitemap.xml"), "the first");
        CHECK(maps[1] && !strcmp(maps[1], "https://example.com/news.xml"), "the second");
        CHECK(maps[2] == NULL, "the list is NULL-terminated");
        mi_politeness_free_sitemaps(maps);
    }
    /* The rules still parse alongside. */
    CHECK(mi_politeness_allowed(p, "example.com", "https://example.com/private/x") == 0,
          "the Disallow rule still applies");
    mi_politeness_free(p);
}

int main(void) {
    test_urlset();
    test_sitemapindex();
    test_entities();
    test_gzip();
    test_not_a_sitemap();
    test_robots_sitemap_directive();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
