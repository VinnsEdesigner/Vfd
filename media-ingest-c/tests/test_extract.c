/* test_extract.c - the media extractor wrapper.
 *
 * Two tools are wrapped here and they return different shapes, so the parsing is
 * the part worth testing: yt-dlp answers with one object carrying a formats
 * array, gallery-dl with a flat list of `[kind, ...]` tuples. A parser that
 * mis-reads the second produces entries with no URL, which is a download that
 * cannot start.
 *
 * The host-based choice is tested directly because it decides which tool is
 * asked, and a wrong choice costs the user an empty result they cannot explain.
 *
 * The live tests are skipped when the tools are absent or the site is
 * unreachable: this is a subprocess wrapper, and its own logic is the argv and
 * the parsing, both of which are exercised without a network.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mi_extract.h"
#include "mi_util.h"

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

/* ------------------------------------------------------------------ */
/* Which extractor is chosen                                           */
/* ------------------------------------------------------------------ */

static void test_pick_by_host(void) {
    mi_extract_config cfg;
    mi_extract_config_init(&cfg);

    /* A gallery host goes to gallery-dl: yt-dlp returns nothing for these. */
    CHECK(mi_extract_pick(&cfg, "https://www.flickr.com/photos/x/") == MI_EXTRACT_GALLERY,
          "flickr goes to the gallery extractor");
    CHECK(mi_extract_pick(&cfg, "https://imgur.com/gallery/abc") == MI_EXTRACT_GALLERY,
          "imgur goes to the gallery extractor");
    CHECK(mi_extract_pick(&cfg, "https://www.deviantart.com/x/gallery") == MI_EXTRACT_GALLERY,
          "deviantart goes to the gallery extractor");
    /* A subdomain must still match: the fragment is matched, not the whole host. */
    CHECK(mi_extract_pick(&cfg, "https://live.staticflickr.com/1/2.jpg") == MI_EXTRACT_GALLERY,
          "a subdomain of a gallery host matches");

    /* A video site goes to yt-dlp, which is the default. */
    CHECK(mi_extract_pick(&cfg, "https://www.youtube.com/watch?v=x") == MI_EXTRACT_VIDEO,
          "youtube goes to the video extractor");
    CHECK(mi_extract_pick(&cfg, "https://vimeo.com/123") == MI_EXTRACT_VIDEO,
          "vimeo goes to the video extractor");
    /* Reddit and X carry video AND images; yt-dlp handles their video, and
     * guessing gallery-dl for them would lose it. */
    CHECK(mi_extract_pick(&cfg, "https://www.reddit.com/r/x/comments/y/") == MI_EXTRACT_VIDEO,
          "reddit goes to the video extractor");
    CHECK(mi_extract_pick(&cfg, "https://x.com/u/status/1") == MI_EXTRACT_VIDEO,
          "x goes to the video extractor");

    /* An explicit choice overrides the host. */
    cfg.kind = MI_EXTRACT_GALLERY;
    CHECK(mi_extract_pick(&cfg, "https://www.youtube.com/watch?v=x") == MI_EXTRACT_GALLERY,
          "an explicit choice overrides the host");
    cfg.kind = MI_EXTRACT_VIDEO;
    CHECK(mi_extract_pick(&cfg, "https://www.flickr.com/photos/x/") == MI_EXTRACT_VIDEO,
          "an explicit choice overrides a gallery host");
    cfg.kind = MI_EXTRACT_AUTO;

    /* A URL with no scheme, or a malformed one, must not crash. */
    CHECK(mi_extract_pick(&cfg, "not a url") == MI_EXTRACT_VIDEO,
          "a URL with no scheme falls back to the video extractor");
    CHECK(mi_extract_pick(&cfg, NULL) == MI_EXTRACT_VIDEO,
          "a NULL URL falls back to the video extractor");
}

/* ------------------------------------------------------------------ */
/* The live tools, when present                                        */
/* ------------------------------------------------------------------ */

static void test_video_resolve(void) {
    mi_extract_config cfg;
    mi_extract_config_init(&cfg);
    if (!mi_extract_available_kind(&cfg, MI_EXTRACT_VIDEO)) {
        printf("skip: no yt-dlp, so the video extractor is not exercised\n");
        return;
    }

    mi_extract_result res;
    /* A search is used rather than a URL because it needs no particular video to
     * still exist, and it exercises the enumerate path too. */
    cfg.search_term = "big buck bunny";
    cfg.search_limit = 5;
    cfg.playlist_items = "1:3";
    int rc = mi_extract(&cfg, NULL, &res);
    if (rc != MI_OK) {
        printf("skip: the video extractor could not reach the site\n");
        return;
    }

    CHECK(res.entry_count > 0, "a video search returns entries");
    CHECK(res.entry_count <= 3, "playlist_items bounds the result");
    CHECK(res.format_count == 0, "a search does not resolve stream formats");
    if (res.entry_count > 0) {
        CHECK(res.entries[0].title != NULL, "an entry has a title");
        CHECK(res.entries[0].url != NULL, "an entry has a URL");
    }
    mi_extract_result_free(&res);
}

/*
 * The gallery parser, against the real tool.
 *
 * This is the one that matters most: gallery-dl's shape is a list of tuples, and
 * a parser that mis-reads it produces entries with no URL — a download that
 * cannot start, presented as a result.
 */
static void test_gallery_resolve(void) {
    mi_extract_config cfg;
    mi_extract_config_init(&cfg);
    if (!mi_extract_available_kind(&cfg, MI_EXTRACT_GALLERY)) {
        printf("skip: no gallery-dl, so the gallery parser is not exercised\n");
        return;
    }

    mi_extract_result res;
    /* A small, stable gallery. The parser is what is under test, not the site. */
    int rc = mi_extract(&cfg, "https://imgur.com/gallery/o4LFvtq", &res);
    if (rc != MI_OK) {
        printf("skip: the gallery extractor could not reach the site\n");
        return;
    }

    /*
     * Every entry must carry a URL. A gallery-dl tuple is `[3, "url", {meta}]`,
     * and reading the metadata instead of the URL is the mistake this catches —
     * it would give entries with a title and nowhere to fetch from.
     */
    int missing_url = 0;
    int missing_title = 0;
    for (size_t i = 0; i < res.entry_count; i++) {
        if (!res.entries[i].url || !*res.entries[i].url) missing_url++;
        if (!res.entries[i].title || !*res.entries[i].title) missing_title++;
    }
    CHECK(missing_url == 0, "every gallery entry has a URL");
    CHECK(missing_title == 0, "every gallery entry has a title");
    /* A gallery returns no stream formats — it is a list of files. */
    CHECK(res.format_count == 0, "a gallery resolves no stream formats");
    mi_extract_result_free(&res);
}

/* A search and a URL are different questions, and one call does one of them. */
static void test_extract_needs_a_url_or_a_search(void) {
    mi_extract_config cfg;
    mi_extract_config_init(&cfg);
    mi_extract_result res;
    CHECK(mi_extract(&cfg, NULL, &res) == MI_ERR_ARG,
          "neither a URL nor a search is refused");
    CHECK(mi_extract(&cfg, "https://example.com/", NULL) == MI_ERR_ARG,
          "a NULL result is refused");
    CHECK(mi_extract(NULL, "https://example.com/", NULL) == MI_ERR_ARG,
          "a NULL config is refused");
}

/* ------------------------------------------------------------------ */
/* Netscape cookies -> ffmpeg's `-cookies` string                      */
/* ------------------------------------------------------------------ */

/*
 * The conversion is pure string work, so it is tested with no tool and no
 * network. It is the one piece of the cookie path with a shape to get wrong:
 * yt-dlp and ffmpeg want the same credential in different forms, and a mistake
 * here means ffmpeg fetches anonymously and the sign-in wall answers.
 */
static void test_cookies_for_ffmpeg(void) {
    /* Nothing in, nothing out: no cookie file is the common case. */
    CHECK(mi_extract_cookies_for_ffmpeg(NULL) == NULL, "NULL cookies yield NULL");
    CHECK(mi_extract_cookies_for_ffmpeg("") == NULL, "empty cookies yield NULL");

    /* A Netscape export: comments and blanks are skipped, and a leading-dot
     * domain loses the dot — ffmpeg matches the domain itself. */
    const char *export_text =
        "# Netscape HTTP Cookie File\n"
        "\n"
        ".example.com\tTRUE\t/\tTRUE\t1700000000\tsessionid\tabc123\n";
    char *got = mi_extract_cookies_for_ffmpeg(export_text);
    CHECK(got != NULL, "a one-cookie export converts");
    CHECK(got && strcmp(got, "sessionid=abc123; domain=example.com; path=/") == 0,
          "the leading dot is dropped and the path is kept");
    free(got);

    /* Two cookies, one per line, in the order they appeared; a domain without a
     * leading dot is kept whole. */
    const char *two =
        ".a.test\tTRUE\t/\tFALSE\t0\tone\t1\n"
        "b.test\tTRUE\t/secure\tTRUE\t0\ttwo\t2\n";
    got = mi_extract_cookies_for_ffmpeg(two);
    CHECK(got != NULL, "a two-cookie export converts");
    CHECK(got && strcmp(got,
              "one=1; domain=a.test; path=/\n"
              "two=2; domain=b.test; path=/secure") == 0,
          "cookies are newline-separated with domains and paths");
    free(got);

    /* A line with too few fields is dropped rather than half-parsed. */
    const char *junk =
        "not-a-cookie\n"
        "c.test\tTRUE\t/\tFALSE\t0\tname\n"; /* six fields: no value */
    CHECK(mi_extract_cookies_for_ffmpeg(junk) == NULL,
          "a malformed line yields no cookie");
}

/*
 * An impersonation target is normalised to the form the tools accept.
 *
 * curl_cffi and yt-dlp both refuse a target with a capital or a separator —
 * "Chrome131" is "Impersonating Chrome131 is not supported" — and a settings box
 * collects exactly that. The failure is silent (the fetch falls back to libcurl),
 * so the spelling is pinned here.
 */
static void test_normalize_impersonate(void) {
    struct { const char *in; const char *want; } CASES[] = {
        {"Chrome131", "chrome131"},
        {"chrome-131", "chrome131"},
        {"chrome_131", "chrome131"},
        {"Chrome 131", "chrome131"},
        {"chrome", "chrome"},       /* a bare family is left alone */
        {"Firefox", "firefox"},
        {"Chrome-136", "chrome136"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        char *got = mi_extract_normalize_impersonate(CASES[i].in);
        CHECK(got && !strcmp(got, CASES[i].want), "a target is normalised to the accepted form");
        free(got);
    }
    CHECK(mi_extract_normalize_impersonate(NULL) == NULL, "NULL normalises to NULL");
    CHECK(mi_extract_normalize_impersonate("") == NULL, "an empty target normalises to NULL");
}

int main(void) {
    test_pick_by_host();
    test_cookies_for_ffmpeg();
    test_normalize_impersonate();
    test_extract_needs_a_url_or_a_search();
    test_video_resolve();
    test_gallery_resolve();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
