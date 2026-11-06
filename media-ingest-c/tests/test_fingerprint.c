/* test_fingerprint.c - near-duplicate detection by content.
 *
 * The properties that matter: identical content fingerprints identically; the
 * same content with a "Related articles" sidebar or a changed timestamp stays
 * close; genuinely different content is far. And a stub is "unknown", not a
 * duplicate of everything.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mi_fingerprint.h"
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

static mi_simhash page(const char *html) {
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    mi_simhash h = mi_simhash_page(doc);
    mi_html_free(doc);
    return h;
}

static void test_identical(void) {
    const char *a =
        "<html><body><article><h1>A title</h1><p>The quick brown fox jumps over "
        "the lazy dog and keeps on running through the field.</p></article></body></html>";
    mi_simhash h1 = page(a);
    mi_simhash h2 = page(a);
    CHECK(h1 != 0, "a real page has a fingerprint");
    CHECK(h1 == h2, "the same content fingerprints identically");
    CHECK(mi_simhash_distance(h1, h2) == 0, "the distance is zero");
}

/* The same article plus a sidebar and a footer stays a near-duplicate. */
static void test_boilerplate_added(void) {
    const char *base =
        "<html><body><article><h1>News</h1><p>Yesterday the committee approved "
        "the new budget after a long debate about the roads and the schools in "
        "the northern district.</p></article></body></html>";
    const char *with_extra =
        "<html><body><nav><a href=\"/\">Home</a><a href=\"/about\">About</a></nav>"
        "<article><h1>News</h1><p>Yesterday the committee approved the new budget "
        "after a long debate about the roads and the schools in the northern "
        "district.</p></article>"
        "<aside>Related: another story, and another one</aside>"
        "<footer>Copyright 2024</footer></body></html>";
    mi_simhash a = page(base);
    mi_simhash b = page(with_extra);
    CHECK(a != 0 && b != 0, "both pages fingerprint");
    int d = mi_simhash_distance(a, b);
    /* Nav, aside and footer are boilerplate and are excluded, so the distance
     * stays small. */
    CHECK(d <= 8, "a page with added boilerplate is close");
    CHECK(mi_simhash_similar(a, b, 8), "and counts as a near-duplicate");
}

/* A one-word change in a long page stays close. The fixture is a full article,
 * not one sentence: SimHash needs a real number of shingles to be stable, and a
 * two-sentence paragraph is the case where it is *not* — which is why the
 * fingerprint reports "unknown" for a stub rather than a confident value. */
static void test_small_edit(void) {
    const char *a =
        "<article><p>The committee approved the new budget on Tuesday after a "
        "long debate. Members discussed the roads in the northern district at "
        "length. The schools there will receive additional funding next year. A "
        "spokesperson said the decision was difficult but necessary. The vote was "
        "twelve to seven with one abstention. Work on the roads is expected to "
        "begin in the spring. The council will review the budget again in six "
        "months time. Residents raised concerns about the timetable and the cost "
        "of the repairs. The committee agreed to publish a full breakdown of the "
        "spending soon. A public meeting will be held next month.</p></article>";
    const char *b =
        "<article><p>The committee approved the new budget on Tuesday after a "
        "long debate. Members discussed the roads in the southern district at "
        "length. The schools there will receive additional funding next year. A "
        "spokesperson said the decision was difficult but necessary. The vote was "
        "twelve to seven with one abstention. Work on the roads is expected to "
        "begin in the spring. The council will review the budget again in six "
        "months time. Residents raised concerns about the timetable and the cost "
        "of the repairs. The committee agreed to publish a full breakdown of the "
        "spending soon. A public meeting will be held next month.</p></article>";
    int d = mi_simhash_distance(page(a), page(b));
    CHECK(d <= 12, "a one-word edit in a full article stays close");
    CHECK(mi_simhash_similar(page(a), page(b), 12), "and is a near-duplicate");
}

/* Different content is far apart. */
static void test_different(void) {
    const char *a =
        "<article><p>Yesterday the committee approved the new budget after a long "
        "debate about the roads and the schools in the northern district.</p></article>";
    const char *b =
        "<article><p>Recipe for chocolate cake: mix flour, sugar and cocoa, then "
        "bake for thirty minutes and serve with fresh cream and berries.</p></article>";
    int d = mi_simhash_distance(page(a), page(b));
    CHECK(d > 16, "different content is far apart");
    CHECK(!mi_simhash_similar(page(a), page(b), 8), "and is not a near-duplicate");
}

/* A stub is unknown, not a duplicate of everything. */
static void test_stub(void) {
    mi_simhash s = page("<p>Hi</p>");
    CHECK(s == 0, "a page with too little text is unknown");
    CHECK(!mi_simhash_similar(s, page("<p>Hello there friend</p>"), 8),
          "an unknown fingerprint matches nothing");
}

static void test_script_ignored(void) {
    const char *a =
        "<article><p>Real content about the committee and the budget and the "
        "roads and the schools in the district.</p></article>";
    const char *b =
        "<article><p>Real content about the committee and the budget and the "
        "roads and the schools in the district.</p>"
        "<script>var x = 'a lot of script text that should not count toward the "
        "fingerprint at all, no not at all';</script></article>";
    CHECK(mi_simhash_distance(page(a), page(b)) <= 4,
          "script content is excluded from the fingerprint");
}

int main(void) {
    test_identical();
    test_boilerplate_added();
    test_small_edit();
    test_different();
    test_stub();
    test_script_ignored();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
