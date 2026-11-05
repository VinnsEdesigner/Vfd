/* mi_identify.h - what is this file, really?
 *
 * WHY THIS EXISTS
 *
 * Some sites serve media without a useful extension or content type — a `.bin`,
 * a `.php`, or a URL that ends in nothing at all. Vyzor's own detector reads
 * magic bytes, which is fast and correct for the containers it knows, but it
 * cannot name a file whose header it does not recognise, and it cannot tell an
 * h264 stream from an h265 one.
 *
 * ffprobe can. This asks it, and answers two questions the rest of Vyzor
 * needs:
 *
 *   what is it      the container and codecs, which decide the file's real
 *                   extension and whether a browser can play it
 *   can it play     whether the browser can play it as-is, or whether it has to
 *                   be converted first
 *
 * This is deliberately a *second* opinion, not the first: the built-in detector
 * is used when it recognises the bytes, and ffprobe only when it does not, or
 * when a conversion is being considered anyway. Probing is a subprocess and
 * costs tens of milliseconds; the magic-byte check costs nothing.
 */
#ifndef MI_IDENTIFY_H
#define MI_IDENTIFY_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    /* 1 when ffprobe answered at all. */
    int probed;
    /* The container ffprobe named, e.g. "matroska,webm", "mov,mp4,m4a,3gp,3g2,mj2",
     * "mpegts", "flac", "wav", "image2". Empty when unknown. */
    char format[128];
    /* The first video and audio codec names, e.g. "h264", "hevc", "vp9",
     * "aac", "opus". Empty when there is no such stream. */
    char video_codec[64];
    char audio_codec[64];
    /* Pixel dimensions of the first video stream, 0 when there is none. */
    int width;
    int height;
    /* Seconds, 0 when unknown or not applicable. */
    double duration_seconds;
    /* 1 when there is a video stream, an audio stream, or both. */
    int has_video;
    int has_audio;
    /* The extension this file should carry, from the container. "bin" when
     * ffprobe could not say. Points at a static string. */
    const char *extension;
    /*
     * 1 when a browser can play this as it is: an MP4/MOV container carrying
     * h264 (or av1/vp9 in an mp4) plus AAC or MP3, or a WebM carrying vp8/vp9/
     * av1 plus vorbis/opus, or a bare image/audio format it supports.
     *
     * The point is to avoid converting something that already plays: a
     * conversion costs CPU and time, and a remux that is not needed is waste.
     */
    int browser_playable;
} mi_identify;

/*
 * Ask ffprobe about a file.
 *
 * Returns 0 when ffprobe answered and the file is a media file it understands,
 * -1 otherwise (no ffprobe, not media, unreadable). `out` is always filled in —
 * `probed` says whether anything was learned.
 *
 * `ffprobe_path` NULL searches PATH.
 */
int mi_identify_probe(const char *path, const char *ffprobe_path, mi_identify *out);

/* Whether an ffprobe is available to ask. */
int mi_identify_available(const char *ffprobe_path);

#endif /* MI_IDENTIFY_H */
