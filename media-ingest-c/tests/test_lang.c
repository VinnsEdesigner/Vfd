/* test_lang.c - reading a page's language, and asking for one.
 *
 * Detection is checked in its order of authority (<html lang> beats the header,
 * og:locale is the fallback), the script-based guess is checked on real CJK and
 * Cyrillic text, and the filter and Accept-Language header are checked because a
 * crawl acts on them.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mi_html.h"
#include "mi_lang.h"

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

static char *detect(const char *html, const char *hdr) {
    mi_html_node *doc = mi_html_parse(html, strlen(html));
    char *l = mi_lang_detect(doc, hdr);
    mi_html_free(doc);
    return l;
}

static void test_html_lang(void) {
    char *l = detect("<html lang=\"en-US\"><body>x</body></html>", NULL);
    CHECK(l && !strcmp(l, "en"), "<html lang> is read and reduced to the primary subtag");
    free(l);
    l = detect("<html lang=\"fr\"><body>x</body></html>", NULL);
    CHECK(l && !strcmp(l, "fr"), "a plain tag");
    free(l);
    /* The page's own statement beats the header. */
    l = detect("<html lang=\"de\"><body>x</body></html>", "en");
    CHECK(l && !strcmp(l, "de"), "<html lang> beats a Content-Language header");
    free(l);
    /* No attribute: the header is used. */
    l = detect("<html><body>x</body></html>", "ja");
    CHECK(l && !strcmp(l, "ja"), "the header is used when the page says nothing");
    free(l);
    /* Nothing at all. */
    l = detect("<html><body>x</body></html>", NULL);
    CHECK(l == NULL, "no signal means NULL");
    free(l);
}

static void test_og_locale(void) {
    char *l = detect("<html><head><meta property=\"og:locale\" content=\"es_ES\">"
                     "</head><body>x</body></html>", NULL);
    CHECK(l && !strcmp(l, "es"), "an og:locale meta is read, underscore and all");
    free(l);
}

static void test_guess_script(void) {
    /* Kana (Japanese). */
    const char *kana = "\xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf"
                       "\xe4\xb8\x96\xe7\x95\x8c";
    char *g = mi_lang_guess(kana, strlen(kana));
    CHECK(g && !strcmp(g, "ja"), "kana is Japanese");
    free(g);
    /* Hangul (Korean). */
    g = mi_lang_guess("\xec\x95\x88\xeb\x85\x95\xed\x95\x98\xec\x84\xb8\xec\x9a\x94"
                      "\xec\x84\xb8\xea\xb3\x84", 18);
    CHECK(g && !strcmp(g, "ko"), "hangul is Korean");
    free(g);
    /* Cyrillic. */
    const char *cyr = "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82 \xd0\xbc\xd0\xb8\xd1\x80";
    g = mi_lang_guess(cyr, strlen(cyr));
    CHECK(g && !strcmp(g, "ru"), "cyrillic is Russian");
    free(g);
    /* CJK without kana. */
    const char *cjk = "\xe4\xb8\xad\xe6\x96\x87\xe6\xb5\x8b\xe8\xaf\x95\xe6\x96\x87\xe6\x9c\xac";
    g = mi_lang_guess(cjk, strlen(cjk));
    CHECK(g && !strcmp(g, "zh"), "CJK without kana is Chinese");
    free(g);
}

static void test_guess_latin(void) {
    const char *en =
        "The quick brown fox and the lazy dog. This is a test of the language "
        "detection, and it is not a very good one, but it is a start.";
    char *g = mi_lang_guess(en, strlen(en));
    CHECK(g && !strcmp(g, "en"), "common English words are recognised");
    free(g);

    const char *fr =
        "Le renard brun et rapide saute par dessus le chien paresseux. Ceci est "
        "un test de la detection, et ce n est pas tres bon, mais c est un debut.";
    g = mi_lang_guess(fr, strlen(fr));
    CHECK(g && !strcmp(g, "fr"), "common French words are recognised");
    free(g);

    /* Too little signal: unknown, not a guess. */
    g = mi_lang_guess("hi there", 8);
    CHECK(g == NULL, "a tiny page is not guessed at");
    free(g);
}

static void test_filter(void) {
    CHECK(mi_lang_matches("en", "en,fr"), "a match");
    CHECK(mi_lang_matches("fr", "en,fr"), "a second match");
    CHECK(!mi_lang_matches("de", "en,fr"), "a non-match");
    CHECK(mi_lang_matches("en", ""), "an empty filter keeps everything");
    CHECK(mi_lang_matches("en", NULL), "a NULL filter keeps everything");
    CHECK(!mi_lang_matches(NULL, "en"), "a filter with no language drops it");
    CHECK(mi_lang_matches("en", "*"), "a wildcard keeps everything");
    CHECK(mi_lang_matches("en", "en;q=0.9,fr"), "a q-weight is ignored in the match");
}

static void test_accept_header(void) {
    char *h = mi_lang_accept_header("en,fr");
    CHECK(h && strstr(h, "en") != NULL, "the first preference is in the header");
    CHECK(h && strstr(h, "fr;q=0.9") != NULL, "the second is weighted");
    free(h);
    CHECK(mi_lang_accept_header("") == NULL, "no preferences means no header");
    CHECK(mi_lang_accept_header(NULL) == NULL, "NULL means no header");
}

int main(void) {
    test_html_lang();
    test_og_locale();
    test_guess_script();
    test_guess_latin();
    test_filter();
    test_accept_header();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
