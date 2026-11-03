/* mi_browse.h - list the media a remote page or directory offers.
 *
 * The console cannot fetch an arbitrary URL itself: the browser enforces CORS,
 * and most media hosts send no CORS headers at all. The engine has no such
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

typedef struct {
    char *url;      /* absolute */
    char *filename; /* the name a save should use */
    char *mime;     /* guessed from the extension; may be "" */
    int64_t bytes;  /* -1 when the source did not say */
} mi_browse_item;

typedef struct {
    mi_browse_item *items;
    size_t count;
    /* The page's own title, when it had one. */
    char *title;
} mi_browse_result;

void mi_browse_result_free(mi_browse_result *res);

/*
 * Fetch `url` and list the media it references.
 *
 * `base` is the URL the listing came from; relative references are resolved
 * against it, which is what makes a directory listing or a relative gallery
 * work. `user_agent` may be NULL for the built-in one. Returns MI_OK, or a
 * negative MI_ERR_*.
 */
int mi_browse_url(const char *url, const char *user_agent, mi_browse_result *out);

/*
 * The same, over bytes already in hand. Exposed so the extraction can be tested
 * against fixtures without a network.
 */
int mi_browse_parse(const char *body, size_t len, const char *base,
                    mi_browse_result *out);

#endif /* MI_BROWSE_H */
