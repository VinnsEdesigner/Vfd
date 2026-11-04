/* mi_mediawatch.c - the media a live page has actually loaded. */
#define _GNU_SOURCE
#include "mi_mediawatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "mi_json.h"
#include "mi_util.h"

/* A cap on entries, so a page that streams for an hour cannot grow the engine's
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
} mw_entry;

struct mi_mediawatch {
    mw_entry *items;
    size_t count, cap;
    int enabled;
    /* Rebuilt lazily, because the list is asked for on a timer and changed on
     * every response. */
    char *json;
    int dirty;
};

mi_mediawatch *mi_mediawatch_new(void) {
    return mi_calloc(1, sizeof(mi_mediawatch));
}

static void entry_free(mw_entry *e) {
    free(e->url);
    free(e->mime);
    free(e->kind);
    free(e->variants);
    free(e->resource_type);
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
    w->dirty = 1;
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
    if (!w || !w->enabled || !url || !*url) return 0;

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
    for (size_t n = 0; n < w->count; n++) {
        const mw_entry *e = &w->items[w->count - 1 - n];
        if (n) mi_json_raw(&b, ",");
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
        if (e->variants) {
            mi_json_raw(&b, ",\"variants\":");
            mi_json_raw(&b, e->variants);
        }
        mi_json_raw(&b, "}");
    }
    mi_json_raw(&b, "]");

    free(w->json);
    w->json = mi_strdup(mi_json_data(&b));
    mi_json_free(&b);
    w->dirty = 0;
    return w->json ? w->json : "[]";
}
