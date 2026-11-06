#define _GNU_SOURCE
#include "mi_sitemap.h"
#include "mi_util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ------------------------------------------------------------------ */
/* gzip                                                                */
/* ------------------------------------------------------------------ */

int mi_gzip_is_gzip(const unsigned char *in, size_t len) {
    return in && len >= 2 && in[0] == 0x1F && in[1] == 0x8B;
}

unsigned char *mi_gzip_decompress(const unsigned char *in, size_t len, size_t *out_len) {
    *out_len = 0;
    if (!mi_gzip_is_gzip(in, len)) return NULL;

    z_stream zs;
    memset(&zs, 0, sizeof zs);
    /* 15 + 16: a gzip header, not a bare zlib stream. */
    if (inflateInit2(&zs, 15 + 16) != Z_OK) return NULL;

    size_t cap = len * 6 + 4096; /* a sitemap compresses several-fold */
    unsigned char *out = mi_malloc(cap + 1);
    if (!out) { inflateEnd(&zs); return NULL; }

    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)len;
    zs.next_out = out;
    zs.avail_out = (uInt)cap;

    for (;;) {
        int rc = inflate(&zs, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) break;
        if (rc == Z_OK || rc == Z_BUF_ERROR) {
            if (zs.avail_out == 0) {
                /* Grow and continue. */
                size_t used = (size_t)zs.total_out;
                cap *= 2;
                unsigned char *grown = mi_realloc(out, cap + 1);
                if (!grown) { free(out); inflateEnd(&zs); return NULL; }
                out = grown;
                zs.next_out = out + used;
                zs.avail_out = (uInt)(cap - used);
                continue;
            }
            if (zs.avail_in == 0) break; /* truncated */
            continue;
        }
        /* A real error. */
        free(out);
        inflateEnd(&zs);
        return NULL;
    }
    size_t total = (size_t)zs.total_out;
    inflateEnd(&zs);
    out[total] = 0;
    *out_len = total;
    return out;
}

/* ------------------------------------------------------------------ */
/* XML                                                                 */
/* ------------------------------------------------------------------ */

/* Decode the five XML entities and numeric references in place-ish. */
static char *xml_decode(const char *s, size_t n) {
    char *out = mi_malloc(n * 4 + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') { out[o++] = s[i]; continue; }
        const char *semi = memchr(s + i, ';', n - i);
        if (!semi || (size_t)(semi - (s + i)) > 10) { out[o++] = s[i]; continue; }
        size_t elen = (size_t)(semi - (s + i + 1));
        unsigned cp = 0;
        int ok = 0;
        if (elen >= 1 && s[i + 1] == '#') {
            char tmp[16]; size_t k = 0;
            size_t j = i + 2;
            int hex = 0;
            if (j < (size_t)(semi - s) && (s[j] == 'x' || s[j] == 'X')) { hex = 1; j++; }
            for (; j < (size_t)(semi - s) && k < 15; j++) tmp[k++] = s[j];
            tmp[k] = 0;
            if (k) { cp = (unsigned)strtoul(tmp, NULL, hex ? 16 : 10); ok = 1; }
        } else if (elen == 3 && !strncmp(s + i + 1, "amp", 3)) { cp = '&'; ok = 1; }
        else if (elen == 2 && !strncmp(s + i + 1, "lt", 2)) { cp = '<'; ok = 1; }
        else if (elen == 2 && !strncmp(s + i + 1, "gt", 2)) { cp = '>'; ok = 1; }
        else if (elen == 4 && !strncmp(s + i + 1, "quot", 4)) { cp = '"'; ok = 1; }
        else if (elen == 4 && !strncmp(s + i + 1, "apos", 4)) { cp = '\''; ok = 1; }
        if (ok && cp) {
            /* UTF-8. */
            if (cp < 0x80) out[o++] = (char)cp;
            else if (cp < 0x800) {
                out[o++] = (char)(0xC0 | (cp >> 6));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out[o++] = (char)(0xE0 | (cp >> 12));
                out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            } else {
                out[o++] = (char)(0xF0 | (cp >> 18));
                out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            }
            i = (size_t)(semi - s);
        } else {
            out[o++] = s[i];
        }
    }
    out[o] = 0;
    return out;
}

/* The text of the first <name>...</name> inside the region [from,to). */
static char *tag_text(const char *from, const char *to, const char *name) {
    char open[32], close[40];
    snprintf(open, sizeof open, "<%s", name);
    snprintf(close, sizeof close, "</%s", name);
    size_t open_len = strlen(open), close_len = strlen(close);
    for (const char *p = from; p + open_len <= to; p++) {
        if (strncasecmp(p, open, open_len) != 0) continue;
        /* Must be the tag itself, not a prefix of a longer name. */
        char after = p[open_len];
        if (after != '>' && !isspace((unsigned char)after)) continue;
        const char *gt = memchr(p, '>', (size_t)(to - p));
        if (!gt) return NULL;
        const char *cs = gt + 1;
        const char *found = NULL;
        for (const char *q = cs; q + close_len <= to; q++) {
            if (strncasecmp(q, close, close_len) == 0 &&
                (q[close_len] == '>' || isspace((unsigned char)q[close_len]))) {
                found = q;
                break;
            }
        }
        if (!found) return NULL;
        /* The text may itself contain a nested tag; keep the raw text. */
        return xml_decode(cs, (size_t)(found - cs));
    }
    return NULL;
}

int mi_sitemap_parse(const unsigned char *bytes, size_t len, mi_sitemap *out) {
    memset(out, 0, sizeof *out);
    if (!bytes || len == 0) return -1;

    /* A .gz sitemap is inflated first. */
    unsigned char *owned = NULL;
    if (mi_gzip_is_gzip(bytes, len)) {
        size_t dlen = 0;
        owned = mi_gzip_decompress(bytes, len, &dlen);
        if (!owned) return -1;
        bytes = owned;
        len = dlen;
    }

    /* A NUL-terminated view, so the string helpers apply. */
    char *xml = mi_malloc(len + 1);
    if (!xml) { free(owned); return -1; }
    memcpy(xml, bytes, len);
    xml[len] = 0;
    free(owned);

    /* Which shape is it? A sitemapindex wraps <sitemap> elements; a urlset
     * wraps <url>. The outermost element decides. */
    const char *index = strstr(xml, "<sitemapindex");
    const char *urlset = strstr(xml, "<urlset");
    if (!index && !urlset) { free(xml); return -1; }
    out->is_index = index && (!urlset || index < urlset);
    const char *child = out->is_index ? "sitemap" : "url";

    /* Walk each child element. A `<loc>` inside it is the URL; the other fields
     * are optional and only meaningful for a urlset. */
    char open[32], close[40];
    snprintf(open, sizeof open, "<%s>", child);
    snprintf(close, sizeof close, "</%s>", child);
    size_t close_len = strlen(close);

    size_t cap = 16;
    out->entries = mi_calloc(cap, sizeof *out->entries);

    const char *p = xml;
    while ((p = strstr(p, open)) != NULL) {
        const char *end = strstr(p, close);
        if (!end) break;
        if (out->count == cap) {
            cap *= 2;
            mi_sitemap_entry *g = mi_realloc(out->entries, cap * sizeof *g);
            if (!g) break;
            out->entries = g;
        }
        mi_sitemap_entry *e = &out->entries[out->count];
        memset(e, 0, sizeof *e);
        e->priority = -1;
        e->loc = tag_text(p, end, "loc");
        e->lastmod = tag_text(p, end, "lastmod");
        e->changefreq = tag_text(p, end, "changefreq");
        char *pri = tag_text(p, end, "priority");
        if (pri) { e->priority = atof(pri); free(pri); }
        if (e->loc && *e->loc) {
            out->count++;
        } else {
            /* An entry with no <loc> is useless; drop it. */
            free(e->loc);
            free(e->lastmod);
            free(e->changefreq);
            memset(e, 0, sizeof *e);
        }
        p = end + close_len;
    }
    free(xml);
    return 0;
}

void mi_sitemap_free(mi_sitemap *s) {
    if (!s) return;
    for (size_t i = 0; i < s->count; i++) {
        free(s->entries[i].loc);
        free(s->entries[i].lastmod);
        free(s->entries[i].changefreq);
    }
    free(s->entries);
    memset(s, 0, sizeof *s);
}
