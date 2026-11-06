#define _GNU_SOURCE
#include "mi_fingerprint.h"
#include "mi_json.h"
#include "mi_util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* A 64-bit hash                                                       */
/* ------------------------------------------------------------------ */

/*
 * FNV-1a, 64-bit, with a final avalanche mix.
 *
 * FNV alone distributes poorly in the high bits, and SimHash needs every bit to
 * be independent — a biased bit would make unrelated pages look similar. The
 * mix (the MurmurHash3 finalizer) spreads the input across all 64 bits.
 */
static mi_simhash hash64(const char *s, size_t len) {
    mi_simhash h = 1469598103934665603ULL; /* FNV offset basis */
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL; /* FNV prime */
    }
    /* The MurmurHash3 fmix64 avalanche. */
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

/* ------------------------------------------------------------------ */
/* Shingling                                                           */
/* ------------------------------------------------------------------ */

/*
 * Feed overlapping 3-word shingles into the bit counters.
 *
 * A page is represented by its *phrases* rather than its words: "the quick brown
 * fox" contributes "the quick brown" and "quick brown fox". That is what makes
 * the fingerprint stable when a word is inserted or a sentence reordered, which
 * is exactly the near-duplicate case.
 */
static size_t shingle(const char *text, int *counts, int shingle_words) {
    /* Walk the words, keeping the last `shingle_words` to form each shingle. */
    const char *words[8];
    size_t lens[8];
    int have = 0;
    size_t n_shingles = 0;
    const char *p = text;
    while (*p) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '\'')) p++;
        size_t len = (size_t)(p - start);
        if (len == 0 || len > 40) continue;
        if (have < shingle_words) {
            words[have] = start;
            lens[have] = len;
            have++;
        } else {
            memmove(words, words + 1, (size_t)(shingle_words - 1) * sizeof *words);
            memmove(lens, lens + 1, (size_t)(shingle_words - 1) * sizeof *lens);
            words[shingle_words - 1] = start;
            lens[shingle_words - 1] = len;
        }
        if (have < shingle_words) continue;

        /* Build the shingle as "w1 w2 w3" and hash it. */
        char buf[160];
        size_t o = 0;
        for (int i = 0; i < shingle_words; i++) {
            if (i) buf[o++] = ' ';
            size_t n = lens[i] < sizeof buf - o - 1 ? lens[i] : sizeof buf - o - 1;
            memcpy(buf + o, words[i], n);
            o += n;
        }
        mi_simhash h = hash64(buf, o);
        for (int b = 0; b < 64; b++) {
            if (h & (1ULL << b)) counts[b]++;
            else counts[b]--;
        }
        n_shingles++;
    }
    return n_shingles;
}

/* ------------------------------------------------------------------ */
/* Boilerplate removal                                                 */
/* ------------------------------------------------------------------ */

/* Tags whose text is not page content. */
static int skip_tag(const char *name) {
    static const char *SKIP[] = {"script", "style", "noscript", "template",
                                 "svg", "canvas", "head", "nav", "footer",
                                 "aside", "form", "iframe", NULL};
    for (size_t i = 0; SKIP[i]; i++) if (!strcasecmp(name, SKIP[i])) return 1;
    return 0;
}

/* Whether an element is boilerplate by role. A nav/footer is already skipped by
 * tag; this catches the `role="navigation"` and `class="menu"` shapes. */
static int is_boilerplate(const mi_html_node *n) {
    const char *role = mi_html_attr(n, "role");
    if (role) {
        if (!strcasecmp(role, "navigation") || !strcasecmp(role, "banner") ||
            !strcasecmp(role, "contentinfo") || !strcasecmp(role, "search") ||
            !strcasecmp(role, "complementary"))
            return 1;
    }
    const char *cls = mi_html_attr(n, "class");
    if (cls) {
        /* A coarse check for the usual boilerplate class names. */
        static const char *NEEDLES[] = {"nav", "menu", "footer", "header",
                                        "sidebar", "breadcrumb", "comment",
                                        "share", "social", "cookie", NULL};
        for (size_t i = 0; NEEDLES[i]; i++) {
            const char *p = cls;
            size_t nl = strlen(NEEDLES[i]);
            while ((p = strcasestr(p, NEEDLES[i])) != NULL) {
                /* Word-ish match: not part of a longer word. */
                char before = p > cls ? p[-1] : ' ';
                char after = p[nl];
                if (!isalnum((unsigned char)before) && !isalnum((unsigned char)after))
                    return 1;
                p += nl;
            }
        }
    }
    return 0;
}

/* Collect the visible text, skipping boilerplate subtrees. */
static void collect_visible(const mi_html_node *n, mi_json_buf *b) {
    for (; n; n = n->next_sibling) {
        if (n->type == MI_HTML_TEXT) {
            if (n->text) mi_json_raw(b, n->text);
            mi_json_raw(b, " ");
            continue;
        }
        if (n->type != MI_HTML_ELEMENT) continue;
        if (skip_tag(n->name) || is_boilerplate(n)) continue;
        collect_visible(n->first_child, b);
    }
}

/* ------------------------------------------------------------------ */
/* The fingerprint                                                     */
/* ------------------------------------------------------------------ */

static mi_simhash finish(const int *counts, size_t shingles) {
    /* Too few shingles: the fingerprint would be noise, so report "unknown". */
    if (shingles < 5) return 0;
    mi_simhash h = 0;
    for (int b = 0; b < 64; b++) {
        if (counts[b] > 0) h |= (1ULL << b);
    }
    return h;
}

mi_simhash mi_simhash_text(const char *text, size_t len) {
    if (!text || len == 0) return 0;
    int counts[64] = {0};
    size_t n = shingle(text, counts, 3);
    return finish(counts, n);
}

mi_simhash mi_simhash_page(const mi_html_node *doc) {
    if (!doc) return 0;
    mi_json_buf b;
    mi_json_init(&b);
    collect_visible(doc->first_child, &b);
    mi_simhash h = mi_simhash_text(mi_json_data(&b), b.len);
    mi_json_free(&b);
    return h;
}

int mi_simhash_distance(mi_simhash a, mi_simhash b) {
    mi_simhash x = a ^ b;
    int n = 0;
    while (x) { n += (int)(x & 1); x >>= 1; }
    return n;
}

int mi_simhash_similar(mi_simhash a, mi_simhash b, int max_distance) {
    /* A zero fingerprint is "unknown", never a match. */
    if (a == 0 || b == 0) return 0;
    return mi_simhash_distance(a, b) <= max_distance;
}
