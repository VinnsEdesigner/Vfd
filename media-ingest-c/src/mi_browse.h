/* mi_browse.h - list the media a remote page or directory offers.
 *
 * The console cannot fetch an arbitrary URL itself: the browser enforces CORS,
 * and most media hosts send no CORS headers at all. Vyzor has no such
 * limit, so the listing happens here and the console asks for it.
 *
 * Two sources are understood:
 *   - an HTML page, from which media URLs are extracted (a listing, a gallery,
 *     a media page);
 *   - a JSON listing, whose entries are read from well-known field names.
 *
 * Only references are returned. Nothing is fetched here: each item carries the
 * URL a later download would use.
 */
#ifndef MI_BROWSE_H
#define MI_BROWSE_H

#include <stddef.h>
#include <stdint.h>

#include "mi_extract.h"

typedef struct {
    char *url;      /* absolute */
    char *filename; /* the name a save should use */
    /* The link text, when the reference came from an anchor. NULL otherwise.
     * It is what a person would read on the page, so a listing of a site's
     * contents is readable rather than a wall of URLs. */
    char *title;
    char *mime;     /* guessed from the extension; may be "" */
    int64_t bytes;  /* -1 when the source did not say */
    /*
     * 1 when a media extractor resolved this item rather than the page's own
     * markup yielding it.
     *
     * The distinction matters to a caller. An extracted item's URL is the
     * *page*, not a stream URL: a stream URL is signed and expires within hours,
     * so a browse result that carried one would be dead by the time it was
     * clicked — the download path re-resolves the page instead. And an extracted
     * item is the only one that is actually playable on a page built by
     * JavaScript, whose markup references no media at all.
     */
    int resolved;
    /* A poster for the item, when the extractor reported one. NULL otherwise. */
    char *thumbnail;
} mi_browse_item;

/*
 * How much of a page to list.
 *
 *   MEDIA  only references that look like media — what this app can play or
 *          show. The default, and what a downloader wants.
 *   ALL    every reference, as a browser sees the page: links to other pages,
 *          stylesheets, archives, anything. This is "read the site", not
 *          "find me something to play", and is useful for walking a site.
 */
typedef enum {
    MI_BROWSE_MEDIA = 0,
    MI_BROWSE_ALL
} mi_browse_filter;

typedef struct {
    mi_browse_item *items;
    size_t count;
    /* The page's own title, when it had one. */
    char *title;
} mi_browse_result;

void mi_browse_result_free(mi_browse_result *res);

/*
 * Fetch `url` and list what it references.
 *
 * `user_agent` may be NULL for the built-in one. `filter` chooses how much to
 * list: MI_BROWSE_MEDIA keeps only media-looking references, MI_BROWSE_ALL
 * keeps everything, as a browser would see it. Returns MI_OK, or a negative
 * MI_ERR_*.
 */
int mi_browse_url(const char *url, const char *user_agent, mi_browse_filter filter,
                  mi_browse_result *out);

/*
 * A configuration for a browse that may also ask a media extractor.
 *
 * Everything a browse needs beyond the URL: the egress and identity settings the
 * rest of the server uses, so a page behind a sign-in or a refusal is read the
 * same way a download would read it. All fields may be left NULL.
 */
typedef struct {
    const char *user_agent;
    const char *cookies_text;
    /* A browser whose own cookie store the extractor should read. NULL is off. */
    const char *cookies_from_browser;
    const char *proxy;
    const char *impersonate;
    const char *extractor_path;
    const char *gallery_extractor_path;
    /* The torrent extractor's binary; NULL uses "py1337x" from PATH. */
    const char *torrent_extractor_path;
    /* A BitTorrent index to search, e.g. "1337x"; NULL uses the default. */
    const char *torrent_site;
    /* When non-zero, a page whose markup references no playable media is asked
     * of the extractor, which is the only way a JavaScript-built page (a video
     * watch page) yields its media at all. */
    int use_extractor;
} mi_browse_config;

/* The same as mi_browse_url, with the settings above. */
int mi_browse_url_ext(const mi_browse_config *cfg, const char *url,
                      mi_browse_filter filter, mi_browse_result *out);

/*
 * The same, over bytes already in hand. Exposed so the extraction can be tested
 * against fixtures without a network.
 */
int mi_browse_parse(const char *body, size_t len, const char *base,
                    mi_browse_filter filter, mi_browse_result *out);

#endif /* MI_BROWSE_H */
