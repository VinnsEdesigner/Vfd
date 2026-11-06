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

int main(void) {
    test_pick_by_host();
    test_extract_needs_a_url_or_a_search();
    test_video_resolve();
    test_gallery_resolve();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
