/* mi_fingerprint.h - near-duplicate detection by content, not by URL.
 *
 * At scale the same content appears under many URLs: a product at several
 * facets, a page at `?session=` and `?ref=`, an article syndicated in three
 * places. Deduplicating by URL misses all of them. This fingerprints the
 * *content* so a caller can tell "I have already seen this page, whatever it
 * was called".
 *
 * The fingerprint is a 64-bit **SimHash**, the technique search engines use for
 * near-duplicate detection:
 *
 *   1. tokenise the text into shingles (overlapping word runs), so a page is
 *      represented by its phrases, not its words;
 *   2. hash each shingle to 64 bits;
 *   3. for each bit position, count how many shingles have it set and how many
 *      clear it, and set the fingerprint's bit to whichever is larger.
 *
 * Two pages with a small *Hamming distance* between their fingerprints have
 * nearly the same content. A page that adds a "Related articles" sidebar, or
 * changes a timestamp, moves only a few bits — while a genuinely different page
 * moves about half of them. The threshold is what a caller tunes; a distance of
 * 3 or fewer out of 64 is the usual near-duplicate cut.
 *
 * Boilerplate is removed first (script, style, and the nav/footer a page repeats
 * on every URL), because a fingerprint over boilerplate would call every page of
 * a site a duplicate of every other.
 */
#ifndef MI_FINGERPRINT_H
#define MI_FINGERPRINT_H

#include <stddef.h>
#include "mi_html.h"

/* A 64-bit content fingerprint. 0 is "no fingerprint". */
typedef unsigned long long mi_simhash;

/*
 * The SimHash of a page's visible text, with boilerplate removed.
 *
 * `doc` is a parsed document. Returns 0 when the page has too little text to
 * fingerprint (a stub, an error page) — a caller should treat 0 as "unknown",
 * not as a duplicate of everything.
 */
mi_simhash mi_simhash_page(const mi_html_node *doc);

/* The SimHash of a plain text body. */
mi_simhash mi_simhash_text(const char *text, size_t len);

/* The Hamming distance between two fingerprints: the number of differing bits.
 * 0 means identical. */
int mi_simhash_distance(mi_simhash a, mi_simhash b);

/* Whether two fingerprints are near-duplicates at `max_distance` bits. */
int mi_simhash_similar(mi_simhash a, mi_simhash b, int max_distance);

#endif
