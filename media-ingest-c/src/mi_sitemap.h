/* mi_sitemap.h - read a sitemap, the site's own index of its pages.
 *
 * A sitemap is what a site publishes to say "here is everything", and for a
 * crawler it is better coverage than link-following: a page no link reaches is
 * still in the sitemap. robots.txt is where the sitemaps are declared, so a
 * crawler that already reads robots should read these too.
 *
 * Two shapes, both XML:
 *
 *   <urlset>       the pages themselves, each a <url> with <loc> and optionally
 *                  <lastmod>, <changefreq> and <priority>.
 *   <sitemapindex> a list of *other* sitemaps, each a <sitemap><loc>. A large
 *                  site shards its index this way; the caller follows them.
 *
 * A sitemap is often served gzip-compressed (`.xml.gz`, or a Content-Type of
 * application/gzip), so the bytes are sniffed for the gzip magic and inflated
 * before parsing. The parser is deliberately tolerant of a malformed XML
 * document: a real sitemap is generated, and refusing to read one over a
 * stray byte would lose the whole index.
 */
#ifndef MI_SITEMAP_H
#define MI_SITEMAP_H

#include <stddef.h>

typedef struct {
    char *loc;
    char *lastmod;    /* as published; may be NULL */
    char *changefreq; /* may be NULL */
    /* The published priority, or -1 when absent. */
    double priority;
} mi_sitemap_entry;

typedef struct {
    mi_sitemap_entry *entries;
    size_t count;
    /* 1 when this was a <sitemapindex>: the entries are other sitemaps, not
     * pages, and the caller should fetch them rather than crawl them. */
    int is_index;
} mi_sitemap;

void mi_sitemap_free(mi_sitemap *s);

/*
 * Parse sitemap bytes (which may be gzip-compressed) into `out`.
 *
 * Returns 0 on success — including a document with no entries, which is a valid
 * (if useless) sitemap — or -1 when the bytes are not a sitemap at all.
 */
int mi_sitemap_parse(const unsigned char *bytes, size_t len, mi_sitemap *out);

/*
 * Decompress gzip bytes. Returns a new buffer the caller frees, or NULL when the
 * input is not gzip. `out_len` receives the length.
 */
unsigned char *mi_gzip_decompress(const unsigned char *in, size_t len, size_t *out_len);

/* Whether the bytes begin with the gzip magic number. */
int mi_gzip_is_gzip(const unsigned char *in, size_t len);

#endif
