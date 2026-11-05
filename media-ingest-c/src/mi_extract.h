/* mi_extract.h - the URLs a streaming site will not put in its HTML.
 *
 * WHY THIS IS A SEPARATE MODULE
 *
 * A crawl reads a page's links. That works for a datasheet host and fails
 * completely for a streaming site: the player's media URL is not in the HTML,
 * it is signed, and the signature is computed by obfuscated JavaScript at play
 * time. Measured on one YouTube watch page: the HTML carries `signatureCipher`
 * entries and no usable `url`, so a crawl finds nothing and the traffic watcher
 * sees only thumbnails.
 *
 * yt-dlp exists because that problem is large and changes constantly. It is the
 * maintained implementation of the transforms these sites require, and it
 * returns a plain list of URLs with their formats — which is the only thing
 * Vyzor actually needs.
 *
 * WHAT THIS IS NOT
 *
 * It is not a downloader. yt-dlp is asked to *resolve* a page, not to fetch the
 * media: the URLs it prints are handed to Vyzor's own download path, which then
 * reads them (through ffmpeg, for a manifest). One tool for finding the stream,
 * one for fetching it.
 *
 * And it is a fallback, not the first move. A crawl is tried first, because most
 * sites do not need this and shelling out to a Python program is not free.
 */
#ifndef MI_EXTRACT_H
#define MI_EXTRACT_H

#include <stddef.h>
#include <stdint.h>

/* One way of fetching the media, as the extractor describes it. */
typedef struct {
    char *url;
    /* The extractor's own id for the format, e.g. "137" or "22". */
    char *format_id;
    char *ext;
    /* "1920x1080" or NULL when the format does not say. */
    char *resolution;
    /* Video and audio codec names, or "none" for a muxed-away stream. */
    char *vcodec;
    char *acodec;
    /* The media type, e.g. "video/mp4". */
    char *mime;
    int64_t filesize;      /* -1 when unknown */
    double bitrate;        /* -1 when unknown */
    int height;            /* 0 when unknown */
    /* 1 when this format carries both video and audio, so it can be saved
     * as-is. A video-only or audio-only format needs the two muxed. */
    int has_video;
    int has_audio;
} mi_extract_format;

/* One item in a playlist, channel or search result. */
typedef struct {
    char *id;
    char *title;
    /* The page URL a second call would resolve. */
    char *url;
    char *uploader;
    double duration_seconds; /* -1 when unknown */
    char *thumbnail;
    /* 1 when this entry is itself a playlist rather than a video. */
    int is_playlist;
} mi_extract_entry;

typedef struct {
    /* The page's own title, when the extractor reported one. */
    char *title;
    /* The site it came from, e.g. "youtube". */
    char *extractor;
    double duration_seconds; /* -1 when unknown */
    /* A still image for the media, when there is one. */
    char *thumbnail;
    mi_extract_format *formats;
    size_t format_count;
    /*
     * The entries of a playlist, channel or search.
     *
     * A list URL describes many items and `--flat-playlist` returns them without
     * resolving each one, so these carry a title, an id and a page URL but no
     * stream URLs — a chosen one is resolved in a second call. Empty for a
     * single item, which is the common case.
     */
    mi_extract_entry *entries;
    size_t entry_count;
} mi_extract_result;

void mi_extract_result_free(mi_extract_result *res);

typedef struct {
    /*
     * The extractor binary. NULL uses "yt-dlp" from PATH.
     *
     * Named rather than hardcoded so an operator can point at their own build;
     * a distribution's copy can lag a site's changes by months.
     */
    const char *binary;
    /* Seconds allowed for the resolve. It is a network operation against a
     * site that may be slow to answer, so this is generous. */
    double timeout_seconds;
    /*
     * Whether to enumerate a playlist or channel rather than resolve one item.
     *
     * `--flat-playlist` lists the entries without resolving each one, which is
     * far cheaper and is what a discovery pass needs. Off by default: a caller
     * usually has one video in mind.
     */
    int enumerate;
    /* Cookies, in Netscape format, for media that is not public — a private or
     * unlisted video the caller has rights to. NULL sends none. */
    const char *cookies_file;
    /*
     * A search to run instead of resolving a URL.
     *
     * yt-dlp answers `ytsearch<N>:term` with a list of matching videos, so a
     * caller who knows *what* they want but not *where* it is can find it. When
     * this is set, `url` is ignored and the query is run instead — the two are
     * different questions and one call does one of them.
     *
     * A search is always enumerated (there is no single result to resolve) and
     * the entries carry titles and ids but no stream URLs, so a chosen entry is
     * resolved in a second call.
     */
    const char *search_term;
    /* The most search results to return. 0 uses a small default. */
    int search_limit;
} mi_extract_config;

void mi_extract_config_init(mi_extract_config *cfg);

/*
 * Ask the extractor what `url` offers.
 *
 * Returns MI_OK when the page was resolved, even if it yielded no formats —
 * "resolved and there is nothing here" is a result. A negative MI_ERR_* means
 * the extractor could not run or refused the URL.
 */
int mi_extract(const mi_extract_config *cfg, const char *url, mi_extract_result *out);

/*
 * Whether the extractor is usable: the binary exists and answers.
 *
 * Called before offering the feature, so the console never shows a control that
 * cannot work. Cached after the first call — it does not change at runtime.
 */
int mi_extract_available(const mi_extract_config *cfg);

#endif /* MI_EXTRACT_H */
