/* mi_mediawatch.h - what a live page has actually loaded.
 *
 * WHY THIS IS NOT AN HTML SCAN
 *
 * A listing built by reading the page's markup finds the media a page *mentions*.
 * That is not the same as the media a page *plays*: a player fetches a manifest,
 * then segments, then perhaps a separate audio track, and none of those are in
 * the HTML. It also cannot give a real size or a real content type, because
 * nothing has been requested yet.
 *
 * So this watches the browser's own network activity instead. Every response the
 * page receives is offered here, and the ones that are media are kept — with the
 * URL that actually served, the byte count the browser received, and the content
 * type the server sent. That is what a download would use, and it is measured
 * rather than guessed.
 *
 * WHAT IT KEEPS
 *
 *   - progressive media: an MP4, WebM, MP3, and so on, by content type;
 *   - manifests: an HLS playlist or a DASH MPD, which is a *set* of qualities
 *     rather than one file, so it is marked as such and its variants are read
 *     from the playlist itself;
 *   - images, separately, and only above a size floor: a page's icons and
 *     spacers are decoration, and a list of them is a list of nothing. An image
 *     the viewer *picked* is always kept, whatever its size.
 *
 * Duplicates are folded: the same URL fetched twice (a range request, a
 * re-fetch after a seek) is one entry with the larger byte count.
 */
#ifndef MI_MEDIAWATCH_H
#define MI_MEDIAWATCH_H

#include <stddef.h>
#include <stdint.h>

typedef struct mi_mediawatch mi_mediawatch;

mi_mediawatch *mi_mediawatch_new(void);
void mi_mediawatch_free(mi_mediawatch *w);

/* Forget everything. Called when the page navigates: what the last page loaded
 * is not what this one has. */
void mi_mediawatch_clear(mi_mediawatch *w);

/*
 * Whether the list is being published.
 *
 * The collection itself always runs: it is a bounded list of URLs and sizes, and
 * the alternative is a list that is empty when someone first opens it, because
 * the page's requests happened before they asked. What this switches is whether
 * a change is *reported* — that is the part with a cost, since it is a socket
 * message per change.
 */
void mi_mediawatch_set_enabled(mi_mediawatch *w, int on);
int mi_mediawatch_enabled(const mi_mediawatch *w);

/*
 * Note something the viewer picked.
 *
 * WHAT THIS IS FOR
 *
 * Watching the network finds everything a page loads, which on a real page is
 * mostly decoration: icons, spacers, tracking pixels, and the same thumbnail
 * rendered at six sizes. A list of those is not a list of anything — the one
 * thing someone wanted is buried under a hundred that are 3 KB and 16x16.
 *
 * So a viewer can say so instead: clicking or long-pressing a picture or a player
 * is the clearest possible statement that *that* is the thing of interest. An
 * entry picked this way is marked as such and carries what the page knew about
 * it — its title, its own thumbnail, its natural size — so the list can show
 * something recognisable rather than a bare URL.
 */
int mi_mediawatch_note_picked(mi_mediawatch *w, const char *url, const char *title,
                              const char *thumb, const char *kind,
                              int width, int height);

/* Whether the viewer has picked anything, so a caller can say the list is
 * theirs rather than the page's. */
int mi_mediawatch_picked_count(const mi_mediawatch *w);

/*
 * How small an image has to be before it is treated as decoration.
 *
 * A page's icons and spacers are a few kilobytes; a photograph is not. This is
 * what keeps a gallery of thumbnails out of the list while still catching the
 * full-size image someone actually opened. 0 lists every image.
 */
void mi_mediawatch_set_min_image_bytes(mi_mediawatch *w, int64_t bytes);

/*
 * Note a response the page received.
 *
 * `url` is absolute; `mime` is the Content-Type the server sent, without its
 * parameters; `bytes` is what the browser actually got (-1 when the response had
 * no length, which is normal for a live stream). `resource_type` is the CDP
 * ResourceType, which says what the browser thought it was — useful because a
 * server can send a wrong content type, and the browser's own classification is
 * the tie-breaker.
 *
 * Returns 1 when this added or changed an entry, so a caller knows to publish.
 */
int mi_mediawatch_note(mi_mediawatch *w, const char *url, const char *mime,
                       const char *resource_type, int64_t bytes);

/*
 * Note that a manifest's variants are known.
 *
 * A playlist is one URL with several qualities behind it, so the variants are
 * attached to the entry rather than listed as separate items. `variants_json` is
 * an array of `{ "label", "bytes", "url" }`, already JSON.
 */
int mi_mediawatch_note_variants(mi_mediawatch *w, const char *url, const char *variants_json);

/* The list as JSON, newest first. Owned by the watcher. */
const char *mi_mediawatch_json(mi_mediawatch *w);

/* How many entries are kept, for a cheap "did anything change" check. */
size_t mi_mediawatch_count(const mi_mediawatch *w);

#endif /* MI_MEDIAWATCH_H */
