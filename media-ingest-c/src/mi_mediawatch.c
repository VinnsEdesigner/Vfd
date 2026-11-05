/* mi_mediawatch.c - the media a live page has actually loaded. */
#define _GNU_SOURCE
#include "mi_mediawatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "mi_json.h"
#include "mi_util.h"

/* A cap on entries, so a page that streams for an hour cannot grow Vyzor's
 * memory without bound. Segments of a live stream are the case this guards: a
 * long-lived playlist can fetch thousands, and only the manifest is useful. */
#define MW_MAX_ENTRIES 256

typedef struct {
    char *url;
    char *mime;
    /* "video", "audio", "image" or "manifest" — what the entry is, which is what
     * the list groups by. */
    char *kind;
    int64_t bytes;
    /* A playlist or an MPD: one URL, several qualities. */
    int is_manifest;
    /* The variants of a manifest, as JSON, or NULL. */
    char *variants;
    /* The CDP resource type, kept so a wrong content type can be reasoned about
     * rather than silently trusted. */
    char *resource_type;

    /*
     * The viewer picked this one.
     *
     * The difference between "the page loaded it" and "someone wants it": a
     * picked entry is kept whatever its size, is listed first, and carries the
     * metadata the page had for it.
     */
    int picked;
    /* What the page called it — an alt text, a link title, a caption. */
    char *title;
    /* A small image the page showed for this thing, so the list can show
     * something recognisable rather than a filename. */
    char *thumb;
    /* The element's natural size, when the page knew it. */
    int width, height;
} mw_entry;

struct mi_mediawatch {
    mw_entry *items;
    size_t count, cap;
    int enabled;
    /*
     * Below this, an image is decoration.
     *
     * A page loads dozens of icons and spacers of a few kilobytes each, and
     * listing them buries the one picture someone wanted. A picked image is
     * exempt: the viewer has said it matters.
     */
    int64_t min_image_bytes;
    int picked_count;
    /* Rebuilt lazily, because the list is asked for on a timer and changed on
     * every response. */
    char *json;
    int dirty;
};

mi_mediawatch *mi_mediawatch_new(void) {
    mi_mediawatch *w = mi_calloc(1, sizeof(mi_mediawatch));
    if (w) {
        /* 24 KB. Comfortably above an icon or a spacer (a few KB) and well
         * below a photograph or a full-size thumbnail, which is the line that
         * separates decoration from content on real pages. */
        w->min_image_bytes = 24 * 1024;
    }
    return w;
}

static void entry_free(mw_entry *e) {
    free(e->url);
    free(e->mime);
    free(e->kind);
    free(e->variants);
    free(e->resource_type);
    free(e->title);
    free(e->thumb);
    memset(e, 0, sizeof *e);
}

void mi_mediawatch_free(mi_mediawatch *w) {
    if (!w) return;
    for (size_t i = 0; i < w->count; i++) entry_free(&w->items[i]);
    free(w->items);
    free(w->json);
    free(w);
}

void mi_mediawatch_clear(mi_mediawatch *w) {
    if (!w) return;
    for (size_t i = 0; i < w->count; i++) entry_free(&w->items[i]);
    w->count = 0;
    w->picked_count = 0;
    w->dirty = 1;
}

void mi_mediawatch_set_min_image_bytes(mi_mediawatch *w, int64_t bytes) {
    if (!w) return;
    w->min_image_bytes = bytes;
    w->dirty = 1;
}

int mi_mediawatch_picked_count(const mi_mediawatch *w) {
    return w ? w->picked_count : 0;
}

void mi_mediawatch_set_enabled(mi_mediawatch *w, int on) {
    if (!w) return;
    w->enabled = on != 0;
}

int mi_mediawatch_enabled(const mi_mediawatch *w) { return w ? w->enabled : 0; }

size_t mi_mediawatch_count(const mi_mediawatch *w) { return w ? w->count : 0; }

/*
 * What an entry is, from the content type the server sent and the type the
 * browser assigned.
 *
 * The content type is believed first, because it is what a server says the bytes
 * are. The browser's classification is the fallback, and it is the more reliable
 * one for a few cases — a server that sends `application/octet-stream` for an
 * MP4 is common, and the browser still knows it was media because it played it.
 */
static const char *classify(const char *mime, const char *rtype, const char *url) {
    const char *m = mime ? mime : "";
    const char *r = rtype ? rtype : "";

    /* A manifest is a set of qualities, not one file, and is handled apart from
     * the rest. */
    if (strstr(m, "mpegurl") || strstr(m, "dash+xml") ||
        (url && (strstr(url, ".m3u8") || strstr(url, ".mpd"))))
        return "manifest";

    if (!strncmp(m, "video/", 6)) return "video";
    if (!strncmp(m, "audio/", 6)) return "audio";
    if (!strncmp(m, "image/", 6)) return "image";

    /* The server was vague; the browser's own view decides. */
    if (!strcasecmp(r, "Media")) {
        /* A segment of a stream is media the browser fetched, but it is a piece
         * of something rather than a thing to download on its own. */
        return "video";
    }
    if (!strcasecmp(r, "Image")) return "image";
    if (!strcasecmp(r, "XHR") || !strcasecmp(r, "Fetch")) {
        /* A fetch with no media type could still be a manifest by extension. */
        if (url && strstr(url, ".m3u8")) return "manifest";
        return NULL;
    }
    return NULL;
}

static mw_entry *find(mi_mediawatch *w, const char *url) {
    for (size_t i = 0; i < w->count; i++) {
        if (!strcmp(w->items[i].url, url)) return &w->items[i];
    }
    return NULL;
}

int mi_mediawatch_note(mi_mediawatch *w, const char *url, const char *mime,
                       const char *resource_type, int64_t bytes) {
    if (!w || !url || !*url) return 0;

    const char *kind = classify(mime, resource_type, url);
    if (!kind) return 0;

    /*
     * A segment is not a thing to download.
     *
     * An HLS or DASH stream fetches hundreds of small pieces, and listing each
     * one would bury the manifest that describes them. A segment is recognised
     * by its extension and by being small and numerous.
     */
    if (strstr(url, ".ts") || strstr(url, ".m4s") || strstr(url, ".cmfv") ||
        strstr(url, ".cmfa"))
        return 0;

    /*
     * A URL with no media type and no media extension is not media.
     *
     * The network view always knows the content type, but a *rescan* only has
     * what the page's markup says — and a page's markup is full of URLs that are
     * not media: inline scripts, stylesheets, tracking beacons. Listing those
     * would bury the handful of things someone actually wants, so an entry with
     * no type is only kept when its extension says it is media.
     */
    if (!mime || !*mime) {
        static const char *const EXTS[] = {
            ".mp4", ".webm", ".mkv", ".mov", ".m4v", ".avi", ".flv",
            ".mp3", ".m4a", ".aac", ".ogg", ".oga", ".opus", ".flac", ".wav",
            ".jpg", ".jpeg", ".png", ".gif", ".webp", ".avif", ".bmp", ".svg",
            ".m3u8", ".mpd",
            NULL};
        int ok = 0;
        for (size_t i = 0; EXTS[i]; i++) {
            size_t el = strlen(EXTS[i]);
            size_t ul = strlen(url);
            if (ul >= el && !strncasecmp(url + ul - el, EXTS[i], el)) { ok = 1; break; }
        }
        if (!ok) return 0;
    }

    mw_entry *e = find(w, url);
    if (e) {
        /*
         * The same URL again — a range request, a re-fetch after a seek. The
         * larger byte count is the better answer, because a range request reports
         * only its own slice.
         */
        if (bytes > e->bytes) {
            e->bytes = bytes;
            w->dirty = 1;
            return 1;
        }
        return 0;
    }

    /*
     * A small image is decoration.
     *
     * Icons, spacers, tracking pixels, and the same thumbnail at six sizes: a
     * real page loads dozens, and a list of them is a list of nothing. A picked
     * image is exempt, because the viewer has said it matters.
     */
    if (!strcmp(kind, "image") && w->min_image_bytes > 0 &&
        bytes >= 0 && bytes < w->min_image_bytes) {
        return 0;
    }

    if (w->count >= MW_MAX_ENTRIES) {
        /* Drop the oldest rather than refusing the newest: a list of what a page
         * just loaded is more useful than one of what it loaded first. */
        entry_free(&w->items[0]);
        memmove(w->items, w->items + 1, (w->count - 1) * sizeof *w->items);
        w->count--;
    }

    if (w->count == w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 16;
        mw_entry *grown = mi_realloc(w->items, cap * sizeof *grown);
        if (!grown) return 0;
        w->items = grown;
        w->cap = cap;
    }

    e = &w->items[w->count];
    memset(e, 0, sizeof *e);
    e->url = mi_strdup(url);
    e->mime = mi_strdup(mime ? mime : "");
    e->kind = mi_strdup(kind);
    e->resource_type = mi_strdup(resource_type ? resource_type : "");
    e->bytes = bytes;
    e->is_manifest = !strcmp(kind, "manifest");
    if (!e->url || !e->mime || !e->kind || !e->resource_type) {
        entry_free(e);
        return 0;
    }
    w->count++;
    w->dirty = 1;
    return 1;
}

int mi_mediawatch_note_picked(mi_mediawatch *w, const char *url, const char *title,
                              const char *thumb, const char *kind,
                              int width, int height) {
    if (!w || !url || !*url) return 0;
    if (!strncasecmp(url, "data:", 5) || !strncasecmp(url, "blob:", 5)) return 0;

    /* A picked entry is always kept, whatever its size: the viewer has said it
     * matters, and that overrules the decoration floor. */
    const char *k = (kind && *kind) ? kind : "image";
    mw_entry *e = find(w, url);
    int is_new = 0;
    if (!e) {
        if (w->count >= MW_MAX_ENTRIES) {
            entry_free(&w->items[0]);
            memmove(w->items, w->items + 1, (w->count - 1) * sizeof *w->items);
            w->count--;
        }
        if (w->count == w->cap) {
            size_t cap = w->cap ? w->cap * 2 : 16;
            mw_entry *grown = mi_realloc(w->items, cap * sizeof *grown);
            if (!grown) return 0;
            w->items = grown;
            w->cap = cap;
        }
        e = &w->items[w->count];
        memset(e, 0, sizeof *e);
        e->url = mi_strdup(url);
        e->mime = mi_strdup("");
        e->kind = mi_strdup(k);
        e->resource_type = mi_strdup("");
        e->bytes = -1;
        e->is_manifest = !strcmp(k, "manifest");
        if (!e->url || !e->mime || !e->kind || !e->resource_type) {
            entry_free(e);
            return 0;
        }
        w->count++;
        is_new = 1;
    }

    if (!e->picked) {
        e->picked = 1;
        w->picked_count++;
    }
    /* The page's own words for it, replaced only when this pick has more to
     * say — a later click should not blank a title an earlier one supplied. */
    if (title && *title) {
        free(e->title);
        e->title = mi_strdup(title);
    }
    if (thumb && *thumb) {
        free(e->thumb);
        e->thumb = mi_strdup(thumb);
    }
    if (width > 0) e->width = width;
    if (height > 0) e->height = height;
    w->dirty = 1;
    return is_new ? 1 : 1;
}

int mi_mediawatch_note_variants(mi_mediawatch *w, const char *url, const char *variants_json) {
    if (!w || !url || !variants_json) return 0;
    mw_entry *e = find(w, url);
    if (!e) return 0;
    free(e->variants);
    e->variants = mi_strdup(variants_json);
    w->dirty = 1;
    return 1;
}

const char *mi_mediawatch_json(mi_mediawatch *w) {
    if (!w) return "[]";
    if (!w->dirty && w->json) return w->json;

    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "[");
    /* Newest first: the thing a page just started playing is what someone is
     * looking for, and a long list should not bury it. */
    /*
     * Picked first, then the rest newest-first.
     *
     * The list is read from the top, and the top should be the thing the viewer
     * actually asked for rather than whatever happened to load last.
     */
    int written = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t n = 0; n < w->count; n++) {
            const mw_entry *e = &w->items[w->count - 1 - n];
            if ((pass == 0) != (e->picked != 0)) continue;
            if (written++) mi_json_raw(&b, ",");
            mi_json_raw(&b, "{\"url\":");
            mi_json_string(&b, e->url);
            mi_json_raw(&b, ",\"kind\":");
            mi_json_string(&b, e->kind);
            mi_json_raw(&b, ",\"mime\":");
            mi_json_string(&b, e->mime);
            mi_json_raw(&b, ",\"bytes\":");
            mi_json_int(&b, e->bytes);
            mi_json_raw(&b, ",\"manifest\":");
            mi_json_bool(&b, e->is_manifest);
            mi_json_raw(&b, ",\"picked\":");
            mi_json_bool(&b, e->picked);
            if (e->title) {
                mi_json_raw(&b, ",\"title\":");
                mi_json_string(&b, e->title);
            }
            if (e->thumb) {
                mi_json_raw(&b, ",\"thumb\":");
                mi_json_string(&b, e->thumb);
            }
            if (e->width > 0) {
                mi_json_raw(&b, ",\"width\":");
                mi_json_int(&b, e->width);
                mi_json_raw(&b, ",\"height\":");
                mi_json_int(&b, e->height);
            }
            if (e->variants) {
                mi_json_raw(&b, ",\"variants\":");
                mi_json_raw(&b, e->variants);
            }
            mi_json_raw(&b, "}");
        }
    }
    mi_json_raw(&b, "]");

    free(w->json);
    w->json = mi_strdup(mi_json_data(&b));
    mi_json_free(&b);
    w->dirty = 0;
    return w->json ? w->json : "[]";
}
