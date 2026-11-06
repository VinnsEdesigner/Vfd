/* mi_lang.h - what language a page is in, and what to ask for.
 *
 * Two halves of one job, and a crawler needs both:
 *
 *   ask      send `Accept-Language` so a site that negotiates serves the
 *            language the caller wants, rather than defaulting to English.
 *   detect   read what language the page actually came back in, so a crawl can
 *            keep or drop it — "only English pages", "everything but Chinese".
 *
 * Detection reads, in order of authority:
 *   1. the `<html lang="...">` attribute — the page saying so itself;
 *   2. a `Content-Language` header, when the fetch layer captured it;
 *   3. a `og:locale` meta;
 *   4. otherwise "unknown" — guessed at only when a caller asks, never reported
 *      as fact.
 *
 * The code is a BCP-47 primary subtag ("en", "fr", "zh"), lowercased. A caller
 * compares on that, not the full tag: "en-US" and "en-GB" are both "en" for the
 * purpose of keeping a page.
 */
#ifndef MI_LANG_H
#define MI_LANG_H

#include <stddef.h>
#include "mi_html.h"

/*
 * The language a page declares, as a primary subtag, or NULL when it declares
 * none. Owned by the caller.
 *
 * `content_language` is the response header value (may be NULL).
 */
char *mi_lang_detect(const mi_html_node *doc, const char *content_language);

/*
 * A light statistical guess when the page declares nothing.
 *
 * This is deliberately not a full language identifier: it scores the page on the
 * small set of languages whose *script* is unmistakable (CJK, Cyrillic, Arabic,
 * Greek, Hebrew) and the handful of very common function words in the big Latin
 * languages. Anything else is "unknown" rather than a wrong answer — a wrong
 * language label is worse than none, because a filter acts on it.
 */
char *mi_lang_guess(const char *text, size_t len);

/* Whether a page's language matches any of a comma-separated list of tags,
 * e.g. "en,fr". An empty or NULL list matches everything. */
int mi_lang_matches(const char *lang, const char *accept_list);

/*
 * An `Accept-Language` header value for a preference list, e.g. "en,fr" ->
 * "en,fr;q=0.9". NULL or empty yields NULL (send the default). Owned by caller.
 */
char *mi_lang_accept_header(const char *preferences);

#endif
