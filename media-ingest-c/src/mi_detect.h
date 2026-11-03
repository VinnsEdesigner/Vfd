/* mi_detect.h - content-based media detection.
 *
 * Detection works from the bytes, never from the file extension or the
 * Content-Type header (both are routinely wrong or spoofed). */
#ifndef MI_DETECT_H
#define MI_DETECT_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    MI_KIND_UNKNOWN = 0,
    MI_KIND_MP4,      /* ISO-BMFF: mp4, m4v, mov, 3gp */
    MI_KIND_MATROSKA, /* mkv, webm */
    MI_KIND_AVI,
    MI_KIND_FLV,
    MI_KIND_OGG,
    MI_KIND_ASF,      /* wmv, asf */
    MI_KIND_MPEG_TS,
    MI_KIND_MPEG_PS,
    MI_KIND_HLS,      /* m3u8 manifest */
    MI_KIND_DASH,     /* mpd manifest */

    /* images */
    MI_KIND_JPEG,
    MI_KIND_PNG,
    MI_KIND_GIF,
    MI_KIND_WEBP,
    MI_KIND_BMP,
    MI_KIND_TIFF,
    MI_KIND_AVIF,
    MI_KIND_HEIC,

    /* audio */
    MI_KIND_MP3,
    MI_KIND_AAC,      /* ADTS */
    MI_KIND_FLAC,
    MI_KIND_WAV,
    MI_KIND_OGG_AUDIO, /* Ogg carrying Vorbis/Opus rather than Theora */

    /* documents — only downloaded when the caller allows non-media */
    MI_KIND_PDF,
    MI_KIND_TEXT
} mi_media_kind;

/* One selectable rendition advertised by an HLS master playlist or a DASH
 * manifest. Sizes are MEASURED from the manifest when it declares them
 * (CODECS/BANDWIDTH, or a #EXT-X-STREAM-INF BANDWIDTH), and reported as -1
 * when the manifest does not say — never guessed. */
typedef struct {
    int height;          /* 0 when the manifest does not state a resolution */
    int width;           /* 0 when unknown */
    int64_t bandwidth;   /* bits per second, -1 when unknown */
    int64_t est_bytes;   /* -1 when it cannot be derived from the manifest */
    char codecs[64];
    char url[512];       /* the variant playlist / representation id */
} mi_quality;

typedef struct {
    mi_media_kind kind;
    double confidence;    /* 0.0 .. 1.0 */
    char container[16];   /* "mp4", "webm", ... (empty when unknown) */
    int is_video;
    int is_image;
    int is_audio;
    /* Frame size, when the container states it in the bytes we sniffed.
     * 0 means unknown — a single-file source has no rendition list, so this is
     * the only resolution there is to report. */
    int width;
    int height;
    char **manifest_urls; /* segment/representation URLs for HLS/DASH */
    size_t manifest_count;
    mi_quality *qualities; /* HLS/DASH variants, when parseable */
    size_t quality_count;
} mi_media_info;

const char *mi_kind_name(mi_media_kind kind);

/* Identify media from a leading chunk of bytes (>= 4 KiB recommended).
 * Never fails: unknown input yields MI_KIND_UNKNOWN. Returns 0. */
int mi_detect(const unsigned char *buf, size_t len, mi_media_info *out);

/* Convenience: sniff the first `head_bytes` of a local file. */
int mi_detect_file(const char *path, size_t head_bytes, mi_media_info *out);

/* Free the manifest_urls owned by `info` (safe on zeroed structs). */
void mi_media_info_free(mi_media_info *info);

/* Deep-copy `src` into `dst` (dst need not be initialised). Returns 0/-1. */
int mi_media_info_copy(mi_media_info *dst, const mi_media_info *src);

/* Parse an HLS master playlist for selectable renditions. `duration_seconds`
 * is used to turn BANDWIDTH into a size; pass <= 0 when unknown, in which case
 * est_bytes stays -1 rather than being invented. Returns 0 and appends to
 * `info->renditions`. */
int mi_parse_hls_renditions(const char *text, size_t len, double duration_seconds,
                            mi_media_info *info);

/* Parse a DASH MPD for selectable video representations. */
int mi_parse_dash_renditions(const char *text, size_t len, double duration_seconds,
                             mi_media_info *info);

#endif /* MI_DETECT_H */
