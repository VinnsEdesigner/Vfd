#define _GNU_SOURCE
#include "mi_detect.h"

#include <stdlib.h>
#include <string.h>

#include "mi_util.h"

/* ------------------------------------------------------------------ */
/* Signature table                                                     */
/* ------------------------------------------------------------------ */

static const unsigned char SIG_MATROSKA[] = {0x1A, 0x45, 0xDF, 0xA3};
static const unsigned char SIG_ASF[] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66,
                                        0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA,
                                        0x00, 0x62, 0xCE, 0x6C};
static const unsigned char SIG_MPEG_PS[] = {0x00, 0x00, 0x01, 0xBA};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static int starts_with(const unsigned char *buf, size_t len,
                       const unsigned char *sig, size_t siglen) {
    return len >= siglen && memcmp(buf, sig, siglen) == 0;
}

static void set_info(mi_media_info *info, mi_media_kind kind, double conf,
                     const char *container, int is_video) {
    info->kind = kind;
    info->confidence = conf;
    info->is_video = is_video;
    info->is_image = 0;
    info->is_audio = 0;
    info->container[0] = '\0';
    if (container) {
        strncpy(info->container, container, sizeof info->container - 1);
        info->container[sizeof info->container - 1] = '\0';
    }
}

static int add_manifest_url(mi_media_info *info, const char *url, size_t len) {
    while (len > 0 && (url[0] == ' ' || url[0] == '\t' || url[0] == '\r')) {
        url++;
        len--;
    }
    while (len > 0 && (url[len - 1] == ' ' || url[len - 1] == '\t' ||
                       url[len - 1] == '\r')) {
        len--;
    }
    if (len == 0 || url[0] == '#') return 0;
    char **grown = mi_realloc(info->manifest_urls,
                              (info->manifest_count + 1) * sizeof(char *));
    if (!grown) return -1;
    info->manifest_urls = grown;
    info->manifest_urls[info->manifest_count] = mi_strndup(url, len);
    if (!info->manifest_urls[info->manifest_count]) return -1;
    info->manifest_count++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Individual detectors                                                */
/* ------------------------------------------------------------------ */

/* The tkhd box ends with two 16.16 fixed-point dimensions. Reading it costs
 * nothing: it sits near the front of the moov, inside the bytes we already
 * sniffed. Only the video track has non-zero dimensions, so the first tkhd with
 * a width is the video one. */
static void parse_tkhd_dimensions(const unsigned char *buf, size_t len,
                                  int *width, int *height) {
    for (size_t i = 4; i + 8 <= len; i += 4) {
        if (memcmp(buf + i, "tkhd", 4) != 0) continue;
        /* The box size sits in the four bytes before the type. */
        if (i < 4) continue;
        uint32_t box_size = ((uint32_t)buf[i - 4] << 24) | ((uint32_t)buf[i - 3] << 16) |
                            ((uint32_t)buf[i - 2] << 8) | (uint32_t)buf[i - 1];
        /* 8 header + 4 version/flags + 8 creation + 8 modification + 4 track id
         * + 4 reserved + 8 duration + 8 reserved + 8 layer/group/volume/reserved
         * + 36 matrix = 92 for v0, and 8 more for v1. */
        size_t end = (size_t)(i - 4) + box_size;
        if (end < 8 || end > len) continue;
        const unsigned char *p = buf + end - 8;
        uint32_t w = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] << 8) | (uint32_t)p[3];
        uint32_t h = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
                     ((uint32_t)p[6] << 8) | (uint32_t)p[7];
        int wi = (int)(w >> 16);
        int hi = (int)(h >> 16);
        if (wi > 0 && hi > 0) {
            *width = wi;
            *height = hi;
            return;
        }
    }
}

static int detect_iso_bmff(const unsigned char *buf, size_t len,
                           mi_media_info *out) {
    static const char *brands[] = {
        "isom", "iso2", "iso4", "iso5", "iso6", "mp41", "mp42", "avc1",
        "dash", "mmp4", "M4V ", "M4A ", "3gp",  "3g2",  "MSNV", "NDAS",
        "f4v",  "qt  "};
    size_t limit = len < 64 ? len : 64;
    for (size_t pos = 0; pos + 8 <= limit; pos += 4) {
        if (memcmp(buf + pos + 4, "ftyp", 4) != 0) continue;
        const unsigned char *brand = buf + pos + 8;
        if (pos + 12 > len) break;
        int known = 0;
        for (size_t i = 0; i < ARRAY_LEN(brands); i++) {
            if (memcmp(brand, brands[i], 4) == 0) {
                known = 1;
                break;
            }
        }
        int is_qt = memcmp(brand, "qt  ", 4) == 0;
        if (known || is_qt) {
            set_info(out, MI_KIND_MP4, 0.97, is_qt ? "mov" : "mp4", 1);
        } else {
            set_info(out, MI_KIND_MP4, 0.70, "mp4", 1);
        }
        parse_tkhd_dimensions(buf, len, &out->width, &out->height);
        return 1;
    }
    return 0;
}

static int detect_matroska(const unsigned char *buf, size_t len,
                           mi_media_info *out) {
    if (!starts_with(buf, len, SIG_MATROSKA, sizeof SIG_MATROSKA)) return 0;
    size_t head = len < 256 ? len : 256;
    if (memmem(buf, head, "webm", 4)) {
        set_info(out, MI_KIND_MATROSKA, 0.98, "webm", 1);
    } else {
        set_info(out, MI_KIND_MATROSKA, 0.95, "matroska", 1);
    }
    return 1;
}

static int detect_avi(const unsigned char *buf, size_t len, mi_media_info *out) {
    if (len >= 12 && memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "AVI ", 4) == 0) {
        set_info(out, MI_KIND_AVI, 0.97, "avi", 1);
        return 1;
    }
    return 0;
}

static int detect_flv(const unsigned char *buf, size_t len, mi_media_info *out) {
    if (starts_with(buf, len, (const unsigned char *)"FLV", 3)) {
        set_info(out, MI_KIND_FLV, 0.95, "flv", 1);
        return 1;
    }
    return 0;
}

static int detect_ogg(const unsigned char *buf, size_t len, mi_media_info *out) {
    if (!starts_with(buf, len, (const unsigned char *)"OggS", 4)) return 0;
    /* Ogg is a container: `theora` means video, `vorbis`/`opus` mean audio. */
    size_t head = len < 512 ? len : 512;
    if (memmem(buf, head, "theora", 6)) {
        set_info(out, MI_KIND_OGG, 0.9, "ogg", 1);
        return 1;
    }
    if (memmem(buf, head, "vorbis", 6) || memmem(buf, head, "OpusHead", 8)) {
        set_info(out, MI_KIND_OGG_AUDIO, 0.92, "ogg", 0);
        out->is_audio = 1;
        return 1;
    }
    set_info(out, MI_KIND_OGG, 0.8, "ogg", 1);
    return 1;
}

static int detect_asf(const unsigned char *buf, size_t len, mi_media_info *out) {
    if (starts_with(buf, len, SIG_ASF, sizeof SIG_ASF)) {
        set_info(out, MI_KIND_ASF, 0.95, "asf", 1);
        return 1;
    }
    return 0;
}

static int detect_mpeg_ts(const unsigned char *buf, size_t len,
                          mi_media_info *out) {
    if (len < 188 * 4) return 0;
    int syncs = 0;
    for (size_t i = 0; i < 188 * 4; i += 188) {
        if (buf[i] == 0x47) syncs++;
    }
    if (syncs >= 4) {
        set_info(out, MI_KIND_MPEG_TS, 0.90, "mpeg-ts", 1);
        return 1;
    }
    return 0;
}

static int detect_mpeg_ps(const unsigned char *buf, size_t len,
                          mi_media_info *out) {
    if (starts_with(buf, len, SIG_MPEG_PS, sizeof SIG_MPEG_PS)) {
        set_info(out, MI_KIND_MPEG_PS, 0.90, "mpeg-ps", 1);
        return 1;
    }
    return 0;
}

static int detect_hls(const unsigned char *buf, size_t len, mi_media_info *out) {
    size_t head = len < 2048 ? len : 2048;
    size_t start = 0;
    while (start < head && (buf[start] == ' ' || buf[start] == '\t' ||
                            buf[start] == '\r' || buf[start] == '\n')) {
        start++;
    }
    if (head - start < 7 || memcmp(buf + start, "#EXTM3U", 7) != 0) return 0;
    if (!memmem(buf, head, "#EXT-X-", 7) && !memmem(buf, head, "#EXTINF", 7)) {
        return 0;
    }
    set_info(out, MI_KIND_HLS, 0.95, "hls", 1);
    /* Collect non-comment lines as segment/child-playlist URLs. */
    size_t i = 0;
    while (i < len) {
        size_t line_start = i;
        while (i < len && buf[i] != '\n') i++;
        size_t line_len = i - line_start;
        if (i < len) i++; /* skip newline */
        if (line_len > 0 && buf[line_start] != '#') {
            add_manifest_url(out, (const char *)(buf + line_start), line_len);
        }
    }
    return 1;
}

static int detect_dash(const unsigned char *buf, size_t len, mi_media_info *out) {
    size_t head = len < 4096 ? len : 4096;
    size_t start = 0;
    while (start < head && (buf[start] == ' ' || buf[start] == '\t' ||
                            buf[start] == '\r' || buf[start] == '\n')) {
        start++;
    }
    int is_xml = (head - start >= 5 && memcmp(buf + start, "<?xml", 5) == 0);
    int is_mpd = memmem(buf, head, "<MPD", 4) != NULL;
    if (!is_mpd && !(is_xml && is_mpd)) return 0;
    if (!is_mpd) return 0;
    set_info(out, MI_KIND_DASH, 0.90, "dash", 1);
    /* Pull media="..." attributes as a best-effort list of segment URLs. */
    const unsigned char *p = buf;
    size_t remaining = len;
    while (remaining > 7) {
        const unsigned char *m = memmem(p, remaining, "media=\"", 7);
        if (!m) break;
        const unsigned char *q = m + 7;
        size_t off = (size_t)(q - p);
        const unsigned char *endq = memchr(q, '"', remaining - off);
        if (!endq) break;
        add_manifest_url(out, (const char *)q, (size_t)(endq - q));
        remaining -= (size_t)(endq - p) + 1;
        p = endq + 1;
    }
    return 1;
}

/* ---------------- images ---------------- */

static int detect_jpeg(const unsigned char *buf, size_t len, mi_media_info *out) {
    if (len >= 3 && buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF) {
        set_info(out, MI_KIND_JPEG, 0.97, "jpeg", 0);
        out->is_image = 1;
        return 1;
    }
    return 0;
}

static int detect_png(const unsigned char *buf, size_t len, mi_media_info *out) {
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (len >= 8 && memcmp(buf, sig, 8) == 0) {
        set_info(out, MI_KIND_PNG, 0.98, "png", 0);
        out->is_image = 1;
        return 1;
    }
    return 0;
}

static int detect_gif(const unsigned char *buf, size_t len, mi_media_info *out) {
    if (len >= 6 && (memcmp(buf, "GIF87a", 6) == 0 || memcmp(buf, "GIF89a", 6) == 0)) {
        set_info(out, MI_KIND_GIF, 0.98, "gif", 0);
        out->is_image = 1;
        return 1;
    }
    return 0;
}

static int detect_webp(const unsigned char *buf, size_t len, mi_media_info *out) {
    if (len >= 12 && memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WEBP", 4) == 0) {
        set_info(out, MI_KIND_WEBP, 0.97, "webp", 0);
        out->is_image = 1;
        return 1;
    }
    return 0;
}

static int detect_bmp(const unsigned char *buf, size_t len, mi_media_info *out) {
    if (len >= 2 && buf[0] == 'B' && buf[1] == 'M') {
        set_info(out, MI_KIND_BMP, 0.9, "bmp", 0);
        out->is_image = 1;
        return 1;
    }
    return 0;
}

static int detect_tiff(const unsigned char *buf, size_t len, mi_media_info *out) {
    if (len >= 4 && ((memcmp(buf, "II\x2a\x00", 4) == 0) ||
                     (memcmp(buf, "MM\x00\x2a", 4) == 0))) {
        set_info(out, MI_KIND_TIFF, 0.95, "tiff", 0);
        out->is_image = 1;
        return 1;
    }
    return 0;
}

static int detect_avif_heic(const unsigned char *buf, size_t len, mi_media_info *out) {
    /* ISO-BMFF brands: `avif`/`avis` for AVIF, `heic`/`heif`/`mif1` for HEIC. */
    size_t limit = len < 64 ? len : 64;
    for (size_t pos = 0; pos + 12 <= limit; pos += 4) {
        if (memcmp(buf + pos + 4, "ftyp", 4) != 0) continue;
        const unsigned char *brand = buf + pos + 8;
        if (memcmp(brand, "avif", 4) == 0 || memcmp(brand, "avis", 4) == 0) {
            set_info(out, MI_KIND_AVIF, 0.97, "avif", 0);
            out->is_image = 1;
            return 1;
        }
        if (memcmp(brand, "heic", 4) == 0 || memcmp(brand, "heix", 4) == 0 ||
            memcmp(brand, "heif", 4) == 0 || memcmp(brand, "mif1", 4) == 0) {
            set_info(out, MI_KIND_HEIC, 0.95, "heic", 0);
            out->is_image = 1;
            return 1;
        }
        break;
    }
    return 0;
}

/* ---------------- documents ---------------- */

/* A PDF starts with "%PDF-". High confidence: nothing else does. */
static int detect_pdf(const unsigned char *buf, size_t len, mi_media_info *out) {
    if (len >= 5 && memcmp(buf, "%PDF-", 5) == 0) {
        set_info(out, MI_KIND_PDF, 0.99, "pdf", 0);
        return 1;
    }
    return 0;
}

/* Plain text has no signature, so this is a *conservative* check: the leading
 * bytes must all be printable ASCII or common whitespace, and there must be
 * enough of them to mean something. That deliberately refuses to call a binary
 * blob text just because it happens to start with a letter.
 *
 * Markup is excluded on purpose. An HTML error page is the classic case where
 * "it is text" would be actively harmful: it would be saved as a .txt file that
 * looks like a successful download of something it is not. Keeping HTML unknown
 * means it is refused, which is the existing and correct behaviour. */
static int detect_text(const unsigned char *buf, size_t len, mi_media_info *out) {
    size_t probe = len < 512 ? len : 512;
    if (probe < 16) return 0;
    /* Skip a UTF-8 BOM and any leading whitespace before the markup check. */
    size_t start = 0;
    if (probe >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) start = 3;
    while (start < probe && (buf[start] == ' ' || buf[start] == '\t' ||
                             buf[start] == '\r' || buf[start] == '\n')) {
        start++;
    }
    if (start < probe && buf[start] == '<') return 0;

    int printable = 0;
    for (size_t i = 0; i < probe; i++) {
        unsigned char c = buf[i];
        if ((c >= 0x20 && c < 0x7F) || c == '\n' || c == '\r' || c == '\t') {
            printable++;
        } else if (c == 0) {
            /* A NUL in the first block means binary, not text. */
            return 0;
        }
    }
    /* Allow a little slack for UTF-8 multibyte sequences, which read as high
     * bytes, without accepting a file that is mostly binary. */
    if ((size_t)printable * 100 < probe * 90) return 0;
    set_info(out, MI_KIND_TEXT, 0.6, "text", 0);
    return 1;
}

/* ---------------- audio ---------------- */

static int detect_mp3(const unsigned char *buf, size_t len, mi_media_info *out) {
    if (len >= 3 && memcmp(buf, "ID3", 3) == 0) {
        set_info(out, MI_KIND_MP3, 0.96, "mp3", 0);
        out->is_audio = 1;
        return 1;
    }
    /* Bare MPEG frame sync: 11 set bits. */
    if (len >= 2 && buf[0] == 0xFF && (buf[1] & 0xE0) == 0xE0) {
        set_info(out, MI_KIND_MP3, 0.7, "mp3", 0);
        out->is_audio = 1;
        return 1;
    }
    return 0;
}

static int detect_aac(const unsigned char *buf, size_t len, mi_media_info *out) {
    /* ADTS: syncword 0xFFF, layer bits 00. */
    if (len >= 2 && buf[0] == 0xFF && (buf[1] & 0xF6) == 0xF0) {
        set_info(out, MI_KIND_AAC, 0.85, "aac", 0);
        out->is_audio = 1;
        return 1;
    }
    return 0;
}

static int detect_flac(const unsigned char *buf, size_t len, mi_media_info *out) {
    if (len >= 4 && memcmp(buf, "fLaC", 4) == 0) {
        set_info(out, MI_KIND_FLAC, 0.98, "flac", 0);
        out->is_audio = 1;
        return 1;
    }
    return 0;
}

static int detect_wav(const unsigned char *buf, size_t len, mi_media_info *out) {
    if (len >= 12 && memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WAVE", 4) == 0) {
        set_info(out, MI_KIND_WAV, 0.98, "wav", 0);
        out->is_audio = 1;
        return 1;
    }
    return 0;
}

typedef int (*detector_fn)(const unsigned char *, size_t, mi_media_info *);

static const detector_fn DETECTORS[] = {
    /* images and audio first: their signatures are unambiguous and would
       otherwise be shadowed by a container match */
    detect_jpeg, detect_png, detect_gif, detect_webp, detect_bmp, detect_tiff,
    detect_avif_heic, detect_flac, detect_wav, detect_mp3, detect_aac,
    /* containers */
    detect_iso_bmff, detect_matroska, detect_avi,  detect_flv, detect_ogg,
    detect_asf,      detect_mpeg_ts,  detect_mpeg_ps, detect_hls, detect_dash,
    /* documents, so an allowed non-media download still gets the right kind
       (and therefore the right file extension) */
    detect_pdf, detect_text,
};

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

const char *mi_kind_name(mi_media_kind kind) {
    switch (kind) {
        case MI_KIND_MP4: return "mp4";
        case MI_KIND_MATROSKA: return "matroska";
        case MI_KIND_AVI: return "avi";
        case MI_KIND_FLV: return "flv";
        case MI_KIND_OGG: return "ogg";
        case MI_KIND_ASF: return "asf";
        case MI_KIND_MPEG_TS: return "mpeg-ts";
        case MI_KIND_MPEG_PS: return "mpeg-ps";
        case MI_KIND_HLS: return "hls";
        case MI_KIND_DASH: return "dash";
        case MI_KIND_JPEG: return "jpeg";
        case MI_KIND_PNG: return "png";
        case MI_KIND_GIF: return "gif";
        case MI_KIND_WEBP: return "webp";
        case MI_KIND_BMP: return "bmp";
        case MI_KIND_TIFF: return "tiff";
        case MI_KIND_AVIF: return "avif";
        case MI_KIND_HEIC: return "heic";
        case MI_KIND_MP3: return "mp3";
        case MI_KIND_AAC: return "aac";
        case MI_KIND_FLAC: return "flac";
        case MI_KIND_WAV: return "wav";
        case MI_KIND_OGG_AUDIO: return "ogg-audio";
        case MI_KIND_PDF: return "pdf";
        case MI_KIND_TEXT: return "text";
        default: return "unknown";
    }
}

int mi_detect(const unsigned char *buf, size_t len, mi_media_info *out) {
    memset(out, 0, sizeof *out);
    out->kind = MI_KIND_UNKNOWN;
    if (!buf || len == 0) return 0;

    mi_media_info best;
    memset(&best, 0, sizeof best);
    best.kind = MI_KIND_UNKNOWN;
    int have_best = 0;

    for (size_t i = 0; i < ARRAY_LEN(DETECTORS); i++) {
        mi_media_info candidate;
        memset(&candidate, 0, sizeof candidate);
        candidate.kind = MI_KIND_UNKNOWN;
        if (!DETECTORS[i](buf, len, &candidate)) continue;
        if (!have_best || candidate.confidence > best.confidence) {
            if (have_best) mi_media_info_free(&best);
            best = candidate;
            have_best = 1;
        } else {
            mi_media_info_free(&candidate);
        }
    }

    if (have_best) {
        *out = best;
    }
    return 0;
}

int mi_detect_file(const char *path, size_t head_bytes, mi_media_info *out) {
    unsigned char *head = NULL;
    ssize_t got = mi_read_head(path, head_bytes, &head);
    if (got < 0) {
        memset(out, 0, sizeof *out);
        out->kind = MI_KIND_UNKNOWN;
        return MI_ERR_IO;
    }
    mi_detect(head, (size_t)got, out);
    free(head);
    return 0;
}

void mi_media_info_free(mi_media_info *info) {
    if (!info) return;
    if (info->manifest_urls) {
        for (size_t i = 0; i < info->manifest_count; i++) free(info->manifest_urls[i]);
        free(info->manifest_urls);
        info->manifest_urls = NULL;
        info->manifest_count = 0;
    }
    free(info->qualities);
    info->qualities = NULL;
    info->quality_count = 0;
}

int mi_media_info_copy(mi_media_info *dst, const mi_media_info *src) {
    memset(dst, 0, sizeof *dst);
    if (!src) return 0;
    dst->kind = src->kind;
    dst->confidence = src->confidence;
    dst->is_video = src->is_video;
    dst->is_image = src->is_image;
    dst->is_audio = src->is_audio;
    memcpy(dst->container, src->container, sizeof dst->container);
    if (src->quality_count > 0 && src->qualities) {
        dst->qualities = mi_calloc(src->quality_count, sizeof(mi_quality));
        if (!dst->qualities) return -1;
        memcpy(dst->qualities, src->qualities,
               src->quality_count * sizeof(mi_quality));
        dst->quality_count = src->quality_count;
    }
    if (src->manifest_count > 0 && src->manifest_urls) {
        dst->manifest_urls = mi_calloc(src->manifest_count, sizeof(char *));
        if (!dst->manifest_urls) return -1;
        for (size_t i = 0; i < src->manifest_count; i++) {
            dst->manifest_urls[i] = mi_strdup(src->manifest_urls[i]);
            if (!dst->manifest_urls[i]) {
                dst->manifest_count = i;
                mi_media_info_free(dst);
                return -1;
            }
        }
        dst->manifest_count = src->manifest_count;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Rendition parsing (HLS master playlists and DASH manifests)         */
/* ------------------------------------------------------------------ */

/* Append one rendition, growing the array. */
static int add_rendition(mi_media_info *info, const mi_quality *r) {
    mi_quality *grown = mi_realloc(info->qualities,
                                     (info->quality_count + 1) * sizeof(mi_quality));
    if (!grown) return -1;
    info->qualities = grown;
    info->qualities[info->quality_count++] = *r;
    return 0;
}

/* Copy up to `max-1` bytes of `src` into `dst`, NUL-terminated. */
static void copy_bounded(char *dst, size_t max, const char *src, size_t len) {
    if (max == 0) return;
    size_t n = len < max - 1 ? len : max - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* BANDWIDTH is bits/second; size = bits * seconds / 8. Only computed when the
 * caller supplied a real duration — otherwise the size stays unknown (-1)
 * rather than being invented. */
static int64_t bytes_from_bandwidth(int64_t bandwidth, double seconds) {
    if (bandwidth <= 0 || seconds <= 0) return -1;
    double bytes = ((double)bandwidth * seconds) / 8.0;
    if (bytes < 0 || bytes > 9.0e18) return -1;
    return (int64_t)bytes;
}

/* "1920x1080" -> width/height. Returns 1 on success. */
static int parse_resolution(const char *s, size_t len, int *w, int *h) {
    const char *x = memchr(s, 'x', len);
    if (!x) return 0;
    char wbuf[12] = {0}, hbuf[12] = {0};
    size_t wl = (size_t)(x - s);
    size_t hl = len - wl - 1;
    if (wl == 0 || wl >= sizeof wbuf || hl == 0 || hl >= sizeof hbuf) return 0;
    memcpy(wbuf, s, wl);
    memcpy(hbuf, x + 1, hl);
    int wi = atoi(wbuf), hi = atoi(hbuf);
    if (wi <= 0 || hi <= 0) return 0;
    *w = wi;
    *h = hi;
    return 1;
}

int mi_parse_hls_renditions(const char *text, size_t len, double duration_seconds,
                            mi_media_info *info) {
    if (!text || !info) return -1;

    /* Walk line by line. A #EXT-X-STREAM-INF applies to the NEXT non-comment
     * line, which is the variant playlist URL. */
    const char *p = text;
    const char *end = text + len;
    int64_t pending_bw = -1;
    int pending_w = 0, pending_h = 0;
    char pending_codecs[64] = {0};
    int have_pending = 0;

    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = eol ? (size_t)(eol - p) : (size_t)(end - p);
        while (line_len > 0 && (p[line_len - 1] == '\r' || p[line_len - 1] == ' ')) line_len--;

        if (line_len >= 18 && strncmp(p, "#EXT-X-STREAM-INF:", 18) == 0) {
            pending_bw = -1;
            pending_w = pending_h = 0;
            pending_codecs[0] = '\0';
            have_pending = 1;
            const char *attrs = p + 18;
            const char *attrs_end = p + line_len;

            const char *bw = memmem(attrs, (size_t)(attrs_end - attrs), "BANDWIDTH=", 10);
            if (bw) pending_bw = strtoll(bw + 10, NULL, 10);

            const char *res = memmem(attrs, (size_t)(attrs_end - attrs), "RESOLUTION=", 11);
            if (res) {
                const char *v = res + 11;
                const char *comma = memchr(v, ',', (size_t)(attrs_end - v));
                size_t rlen = comma ? (size_t)(comma - v) : (size_t)(attrs_end - v);
                parse_resolution(v, rlen, &pending_w, &pending_h);
            }

            const char *cod = memmem(attrs, (size_t)(attrs_end - attrs), "CODECS=\"", 8);
            if (cod) {
                const char *v = cod + 8;
                const char *q = memchr(v, '"', (size_t)(attrs_end - v));
                if (q) copy_bounded(pending_codecs, sizeof pending_codecs, v,
                                    (size_t)(q - v));
            }
        } else if (line_len > 0 && p[0] != '#' && have_pending) {
            mi_quality r;
            memset(&r, 0, sizeof r);
            r.width = pending_w;
            r.height = pending_h;
            r.bandwidth = pending_bw;
            r.est_bytes = bytes_from_bandwidth(pending_bw, duration_seconds);
            copy_bounded(r.codecs, sizeof r.codecs, pending_codecs, strlen(pending_codecs));
            copy_bounded(r.url, sizeof r.url, p, line_len);
            if (add_rendition(info, &r) != 0) return -1;
            have_pending = 0;
        }
        p = eol ? eol + 1 : end;
    }
    return 0;
}

/* Find an XML attribute inside a tag, matching only at a name boundary.
 * A plain memmem for `width="` also matches inside `bandwidth="`, which
 * silently read the bandwidth as the width — the boundary check prevents it. */
static const char *find_attr(const char *tag, size_t tag_len, const char *name) {
    size_t nlen = strlen(name);
    const char *p = tag;
    const char *end = tag + tag_len;
    while (p < end) {
        const char *hit = memmem(p, (size_t)(end - p), name, nlen);
        if (!hit) return NULL;
        /* A name boundary is any character that cannot be part of an XML
         * attribute name. `=` qualifies, which is what rejects the `width`
         * inside `bandwidth="`. */
        char prev = hit == tag ? '<' : hit[-1];
        int at_boundary = !((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') ||
                            (prev >= '0' && prev <= '9') || prev == '-' || prev == '_' ||
                            prev == ':' || prev == '.');
        if (at_boundary && hit + nlen < end && hit[nlen] == '=') {
            const char *v = hit + nlen + 1;
            /* Skip the opening quote so numeric callers can use atoi/strtoll
             * directly; atoi("\"640\"") would silently yield 0. */
            if (v < end && (*v == '"' || *v == '\'')) v++;
            return v;
        }
        p = hit + 1;
    }
    return NULL;
}

int mi_parse_dash_renditions(const char *text, size_t len, double duration_seconds,
                             mi_media_info *info) {
    if (!text || !info) return -1;

    /* Find <Representation ...> elements and read bandwidth / dimensions. */
    const char *p = text;
    const char *end = text + len;
    while (p < end) {
        const char *rep = memmem(p, (size_t)(end - p), "<Representation", 15);
        if (!rep) break;
        const char *tag_end = memchr(rep, '>', (size_t)(end - rep));
        if (!tag_end) break;
        size_t tag_len = (size_t)(tag_end - rep);

        mi_quality r;
        memset(&r, 0, sizeof r);
        r.bandwidth = -1;
        r.est_bytes = -1;

        const char *bw = find_attr(rep, tag_len, "bandwidth");
        if (bw) {
            r.bandwidth = strtoll(bw, NULL, 10);
            r.est_bytes = bytes_from_bandwidth(r.bandwidth, duration_seconds);
        }
        const char *w = find_attr(rep, tag_len, "width");
        if (w) r.width = atoi(w);
        const char *h = find_attr(rep, tag_len, "height");
        if (h) r.height = atoi(h);
        const char *cod = find_attr(rep, tag_len, "codecs");
        if (cod) {
            const char *q = memchr(cod, '"', (size_t)(tag_end - cod));
            if (q) copy_bounded(r.codecs, sizeof r.codecs, cod, (size_t)(q - cod));
        }
        const char *id = find_attr(rep, tag_len, "id");
        if (id) {
            const char *q = memchr(id, '"', (size_t)(tag_end - id));
            if (q) copy_bounded(r.url, sizeof r.url, id, (size_t)(q - id));
        }
        /* Only video representations are selectable qualities. */
        if (r.height > 0 || r.width > 0) {
            if (add_rendition(info, &r) != 0) return -1;
        }
        p = tag_end + 1;
    }
    return 0;
}
