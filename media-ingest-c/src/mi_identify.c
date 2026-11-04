/* mi_identify.c - ask ffprobe what a file is, and whether a browser can play it.
 *
 * The whole reason this is a separate module from mi_detect is that the two
 * answer different questions and cost different amounts. mi_detect reads magic
 * bytes: free, and right for the containers it knows. This runs a subprocess:
 * tens of milliseconds, but it can name a file whose header is unfamiliar, and
 * it knows the codecs, which is what decides whether a browser can play it.
 */
#define _GNU_SOURCE
#include "mi_identify.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mi_util.h"

int mi_identify_available(const char *ffprobe_path) {
    if (ffprobe_path && *ffprobe_path) return mi_which(ffprobe_path);
    return mi_which("ffprobe");
}

/*
 * Does `hay` contain `needle` (case-insensitive)?
 *
 * A plain substring test, because the strings being matched are ffmpeg's own
 * container and codec names — `matroska,webm`, `mov,mp4,m4a,3gp,3g2,mj2`,
 * `png_pipe` — where the name of interest is one element of a comma-separated
 * list or a prefix. A word-boundary rule looked tidier and was wrong: it rejected
 * `png` inside `png_pipe` and so failed to recognise a PNG as an image.
 */
static int has_token(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return 0;
    return strcasestr(hay, needle) != NULL;
}

/*
 * The extension for a container, so a `.bin` becomes something the OS can open.
 *
 * The codecs are needed, not just the format name: ffprobe calls *both* an MKV
 * and a WebM `matroska,webm`, so the container name cannot tell them apart. What
 * distinguishes them is that a WebM may only carry VP8/VP9/AV1 video with
 * Vorbis/Opus audio; anything else in that container is an MKV.
 *
 * Only containers ffprobe commonly names are listed; anything else keeps "bin",
 * because a wrong extension is worse than a generic one — it makes a player try
 * and fail instead of asking the user.
 */
static const char *extension_for(const mi_identify *id) {
    const char *format = id->format;
    if (!format || !*format) return "bin";

    if (has_token(format, "matroska") || has_token(format, "webm")) {
        const int webm_video = id->video_codec[0] == '\0' || has_token(id->video_codec, "vp8") ||
                               has_token(id->video_codec, "vp9") || has_token(id->video_codec, "av1");
        const int webm_audio = id->audio_codec[0] == '\0' ||
                               has_token(id->audio_codec, "vorbis") || has_token(id->audio_codec, "opus");
        return (webm_video && webm_audio) ? "webm" : "mkv";
    }
    if (has_token(format, "mp4") || has_token(format, "mov") || has_token(format, "m4a") ||
        has_token(format, "3gp") || has_token(format, "mj2"))
        return "mp4";
    if (has_token(format, "mpegts")) return "ts";
    if (has_token(format, "mpeg") || has_token(format, "vob")) return "mpg";
    if (has_token(format, "avi")) return "avi";
    if (has_token(format, "flv")) return "flv";
    if (has_token(format, "asf")) return "wmv";
    if (has_token(format, "ogg")) return "ogg";
    if (has_token(format, "flac")) return "flac";
    if (has_token(format, "wav")) return "wav";
    if (has_token(format, "mp3")) return "mp3";
    if (has_token(format, "aac")) return "aac";
    /* `png_pipe` is what ffprobe calls a bare PNG; `image2` a still sequence. */
    if (has_token(format, "png")) return "png";
    if (has_token(format, "image2") || has_token(format, "mjpeg")) return "jpg";
    if (has_token(format, "gif")) return "gif";
    if (has_token(format, "webp")) return "webp";
    if (has_token(format, "bmp")) return "bmp";
    if (has_token(format, "tiff")) return "tiff";
    if (has_token(format, "pdf")) return "pdf";
    return "bin";
}

/*
 * Whether a browser can play this combination as it is.
 *
 * The list is deliberately short and conservative: an answer of "no" costs a
 * conversion, while an answer of "yes" that is wrong leaves the user with a file
 * that will not play, which is the worse failure.
 */
static int browser_can_play(const mi_identify *id) {
    const char *fmt = id->format;
    const char *v = id->video_codec;
    const char *a = id->audio_codec;

    /*
     * A still image is reported by ffprobe as a "video" stream whose codec is the
     * image format — a PNG is `codec_name=png, codec_type=video` in `png_pipe`,
     * not "no streams" as one might assume. So images are recognised by their
     * codec first, before the video/audio split below.
     */
    const int image_codec = has_token(v, "png") || has_token(v, "mjpeg") ||
                            has_token(v, "gif") || has_token(v, "webp") ||
                            has_token(v, "bmp") || has_token(v, "tiff") ||
                            has_token(fmt, "png") || has_token(fmt, "image2") ||
                            has_token(fmt, "mjpeg");
    if (image_codec) return 1;

    /* No streams at all: not something a browser plays as media. */
    if (!id->has_video && !id->has_audio) return 0;

    /* Audio-only: mp3, aac in an mp4/adts, flac, wav, ogg/opus, or raw PCM.
     * `pcm_s16le` is a prefix match, not a whole token, so it is checked with a
     * plain substring — the format is one ffmpeg names, not user input. */
    if (!id->has_video && id->has_audio) {
        if (has_token(a, "mp3") || has_token(a, "flac") || has_token(fmt, "wav") ||
            strncasecmp(a, "pcm_", 4) == 0)
            return 1;
        if (has_token(a, "aac") && (has_token(fmt, "mp4") || has_token(fmt, "mov") ||
                                    has_token(fmt, "aac")))
            return 1;
        if ((has_token(a, "opus") || has_token(a, "vorbis")) && has_token(fmt, "ogg")) return 1;
        return 0;
    }

    /* Video. The container and both codecs have to be playable. */
    const int vp9_family = has_token(v, "h264") || has_token(v, "av1") || has_token(v, "vp9");
    const int vp8_family = has_token(v, "vp8") || has_token(v, "vp9") || has_token(v, "av1");
    const int aac_family = has_token(a, "aac") || has_token(a, "mp3") || a[0] == '\0';
    const int opus_family = has_token(a, "opus") || has_token(a, "vorbis") || a[0] == '\0';

    if (has_token(fmt, "mp4") || has_token(fmt, "mov")) return vp9_family && aac_family;
    if (has_token(fmt, "webm")) return vp8_family && opus_family;
    return 0;
}

/*
 * Read ffprobe's answer.
 *
 * A single `-show_entries` call, printed as `key=value` lines, so the parse is a
 * line scan rather than a JSON walk. `-v error` keeps ffprobe quiet; `-of
 * default=noprint_wrappers=1` is what produces the bare `key=value` form.
 */
static void parse_ffprobe(const char *text, mi_identify *out) {
    for (const char *line = text; *line;) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        char key[64] = {0};
        char val[256] = {0};
        const char *eq = memchr(line, '=', len);
        if (eq) {
            size_t klen = (size_t)(eq - line);
            if (klen < sizeof key) {
                memcpy(key, line, klen);
                key[klen] = '\0';
                size_t vlen = len - klen - 1;
                if (vlen >= sizeof val) vlen = sizeof val - 1;
                memcpy(val, eq + 1, vlen);
                val[vlen] = '\0';
            }
        }
        if (!strcmp(key, "format_name")) snprintf(out->format, sizeof out->format, "%s", val);
        else if (!strcmp(key, "codec_type")) {
            if (!strcmp(val, "video")) out->has_video = 1;
            else if (!strcmp(val, "audio")) out->has_audio = 1;
        } else if (!strcmp(key, "width")) out->width = atoi(val);
        else if (!strcmp(key, "height")) out->height = atoi(val);
        else if (!strcmp(key, "duration")) out->duration_seconds = atof(val);

        if (!eol) break;
        line = eol + 1;
    }
}

int mi_identify_probe(const char *path, const char *ffprobe_path, mi_identify *out) {
    memset(out, 0, sizeof *out);
    out->extension = "bin";
    if (!path || !*path) return -1;

    const char *probe = (ffprobe_path && *ffprobe_path) ? ffprobe_path : "ffprobe";
    if (!mi_which(probe)) return -1;

    /*
     * The entry list is ordered by stream so a codec can be attributed to video
     * or audio: ffprobe prints `codec_type` before the `codec_name` that follows
     * it, so a small state machine pairs them.
     */
    int p2c[2], c2p[2];
    if (pipe(p2c) != 0 || pipe(c2p) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]);
        return -1;
    }
    if (pid == 0) {
        close(p2c[1]);
        close(c2p[0]);
        dup2(p2c[0], STDIN_FILENO);
        dup2(c2p[1], STDOUT_FILENO);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) dup2(dn, STDERR_FILENO);
        char *argv[] = {
            (char *)probe, "-v", "error",
            "-show_entries",
            "format=format_name,duration:stream=codec_type,codec_name,width,height",
            "-of", "default=noprint_wrappers=1",
            (char *)path, NULL,
        };
        execvp(argv[0], argv);
        _exit(127);
    }
    close(p2c[0]);
    close(p2c[1]);
    close(c2p[1]);

    /* Read the answer, bounded: an ffprobe answer is small, and a file with a
     * thousand streams must not be able to exhaust memory here. */
    char buf[16384];
    size_t used = 0;
    for (;;) {
        if (used + 1 >= sizeof buf) break;
        ssize_t n = read(c2p[0], buf + used, sizeof buf - used - 1);
        if (n <= 0) break;
        used += (size_t)n;
    }
    buf[used] = '\0';
    close(c2p[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    if (used == 0) return -1;

    /*
     * Pair codec_name with the codec_type that *follows* it.
     *
     * This ordering is not documented and was got wrong once: ffprobe prints
     * `codec_name=h264` then `codec_type=video`, so a state machine that waited
     * for a type before reading a name attributed every codec to nothing, and
     * every "is it playable" answer came back wrong. Verified against real
     * output rather than assumed.
     */
    int saw_video = 0, saw_audio = 0;
    char pending_codec[64] = {0};
    for (const char *line = buf; *line;) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        if (len > 11 && !strncmp(line, "codec_name=", 11)) {
            size_t vlen = len - 11;
            if (vlen >= sizeof pending_codec) vlen = sizeof pending_codec - 1;
            memcpy(pending_codec, line + 11, vlen);
            pending_codec[vlen] = '\0';
        } else if (len > 11 && !strncmp(line, "codec_type=", 11)) {
            const int is_video = len >= 16 && !strncmp(line + 11, "video", 5);
            const int is_audio = len >= 16 && !strncmp(line + 11, "audio", 5);
            if (is_video && !out->video_codec[0] && pending_codec[0]) {
                snprintf(out->video_codec, sizeof out->video_codec, "%s", pending_codec);
                saw_video = 1;
            } else if (is_audio && !out->audio_codec[0] && pending_codec[0]) {
                snprintf(out->audio_codec, sizeof out->audio_codec, "%s", pending_codec);
                saw_audio = 1;
            }
            pending_codec[0] = '\0';
        }
        if (!eol) break;
        line = eol + 1;
    }

    /* The format and dimensions come from the flat key/value scan. */
    parse_ffprobe(buf, out);
    out->has_video = out->has_video || saw_video;
    out->has_audio = out->has_audio || saw_audio;
    out->probed = (out->format[0] || out->video_codec[0] || out->audio_codec[0]) ? 1 : 0;
    if (!out->probed) return -1;

    out->extension = extension_for(out);
    out->browser_playable = browser_can_play(out);
    return 0;
}
