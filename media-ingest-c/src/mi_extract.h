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
    /* Cookies, in Netscape format, for media that is not public — a private or
     * unlisted video the caller has rights to. NULL sends none. */
    const char *cookies_file;
    /*
     * Whether to resolve a page's *playlist* rather than one item.
     *
     * A channel or a playlist URL describes many items; with this set, the
     * result is the first item's formats and the count is reported so a caller
     * can say so. Off by default: the caller usually has one video in mind.
     */
    int first_only;
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
