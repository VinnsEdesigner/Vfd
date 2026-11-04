/* test_identify.c - what ffprobe says a file is, and whether a browser can play it.
 *
 * The interesting cases are the ones the magic-byte detector cannot answer: a
 * file with no useful extension, and a container whose codecs decide whether a
 * conversion is needed at all. Anything needing ffmpeg is skipped when the host
 * has none, saying so rather than passing silently.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mi_identify.h"
#include "mi_util.h"

static int g_checks = 0;
static int g_failures = 0;
static int g_skipped = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
        }                                                                   \
    } while (0)

#define SKIP(msg)                             \
    do {                                      \
        g_skipped++;                          \
        fprintf(stderr, "skip: %s\n", (msg)); \
    } while (0)

/* Run a shell command, ignoring the result. */
static void sh(const char *cmd) {
    int rc = system(cmd);
    (void)rc;
}

static int have_ffmpeg(void) { return mi_which("ffmpeg") && mi_which("ffprobe"); }

static void test_availability(void) {
    /* The question is answerable whether or not ffprobe exists. */
    int a = mi_identify_available(NULL);
    CHECK(a == 0 || a == 1, "availability is answerable");
    CHECK(mi_identify_available("/nonexistent/ffprobe") == 0,
          "a path that does not exist is not available");
}

static void test_missing_file(void) {
    mi_identify id;
    CHECK(mi_identify_probe("/tmp/definitely-not-here-9f8a7b.bin", NULL, &id) != 0,
          "a file that does not exist cannot be probed");
    CHECK(id.probed == 0, "and nothing is claimed to be known");
    CHECK(!strcmp(id.extension, "bin"), "and the extension stays generic");
}

/*
 * The whole point: a media file whose extension is wrong, or missing, still gets
 * named and gets the right extension back.
 */
static void test_identify_live(void) {
    if (!have_ffmpeg()) {
        SKIP("no ffmpeg/ffprobe, so identification is not exercised");
        return;
    }

    /* An h264 + aac MP4, deliberately named `.bin`. */
    sh("ffmpeg -y -v error -f lavfi -i testsrc=size=320x240:rate=10:duration=2 "
       "-f lavfi -i sine=frequency=440:duration=2 "
       "-c:v libx264 -pix_fmt yuv420p -c:a aac -shortest -f mp4 /tmp/id-h264.bin 2>/dev/null");

    mi_identify id;
    int rc = mi_identify_probe("/tmp/id-h264.bin", NULL, &id);
    CHECK(rc == 0, "an mp4 named .bin is identified");
    CHECK(id.probed == 1, "and is reported as probed");
    CHECK(strstr(id.format, "mp4") || strstr(id.format, "mov"),
          "the container is recognised as an mp4 family");
    CHECK(!strcmp(id.video_codec, "h264"), "the video codec is h264");
    CHECK(!strcmp(id.audio_codec, "aac"), "the audio codec is aac");
    CHECK(id.has_video && id.has_audio, "both streams are seen");
    CHECK(id.width == 320 && id.height == 240, "the frame size is read");
    CHECK(!strcmp(id.extension, "mp4"), "the extension it should carry is mp4");
    CHECK(id.browser_playable == 1, "h264+aac in an mp4 is playable as it is");

    /* A matroska file carrying h265, which a browser cannot play. */
    sh("ffmpeg -y -v error -f lavfi -i testsrc=size=160x120:rate=10:duration=1 "
       "-c:v libx265 -pix_fmt yuv420p -tag:v hvc1 -f matroska /tmp/id-h265.mkv 2>/dev/null");
    if (mi_path_exists("/tmp/id-h265.mkv") && mi_file_size("/tmp/id-h265.mkv") > 0) {
        mi_identify h;
        CHECK(mi_identify_probe("/tmp/id-h265.mkv", NULL, &h) == 0, "an mkv is identified");
        CHECK(strstr(h.format, "matroska") != NULL, "the container is matroska");
        CHECK(!strcmp(h.video_codec, "hevc") || !strcmp(h.video_codec, "h265"),
              "the video codec is h265");
        CHECK(!strcmp(h.extension, "mkv"), "the extension is mkv");
        CHECK(h.browser_playable == 0,
              "h265 in an mkv is NOT playable, so it must be converted");
    } else {
        SKIP("no libx265 encoder, so the unplayable case is skipped");
    }

    /* A webm: vp9 + opus, which a browser does play. */
    sh("ffmpeg -y -v error -f lavfi -i testsrc=size=160x120:rate=10:duration=1 "
       "-c:v libvpx-vp9 -b:v 200k -f webm /tmp/id-vp9.webm 2>/dev/null");
    if (mi_path_exists("/tmp/id-vp9.webm") && mi_file_size("/tmp/id-vp9.webm") > 0) {
        mi_identify w;
        CHECK(mi_identify_probe("/tmp/id-vp9.webm", NULL, &w) == 0, "a webm is identified");
        CHECK(strstr(w.format, "webm") != NULL, "the container is webm");
        CHECK(!strcmp(w.extension, "webm"), "the extension is webm");
        CHECK(w.browser_playable == 1, "vp9 in a webm is playable");
    } else {
        SKIP("no libvpx-vp9 encoder, so the webm case is skipped");
    }

    /* A wav: audio only, playable. */
    sh("ffmpeg -y -v error -f lavfi -i sine=frequency=440:duration=1 -f wav /tmp/id.wav 2>/dev/null");
    if (mi_path_exists("/tmp/id.wav")) {
        mi_identify a;
        CHECK(mi_identify_probe("/tmp/id.wav", NULL, &a) == 0, "a wav is identified");
        CHECK(a.has_audio && !a.has_video, "it has audio and no video");
        CHECK(!strcmp(a.extension, "wav"), "the extension is wav");
        CHECK(a.browser_playable == 1, "a wav is playable");
    }

    /* A png: an image. ffprobe reports it as a *video* stream whose codec is
     * the image format, which is worth pinning — assuming "an image has no
     * streams" is what made every image look unplayable. */
    sh("ffmpeg -y -v error -f lavfi -i testsrc=size=64x64:duration=1 -frames:v 1 -f image2 /tmp/id.png 2>/dev/null");
    if (mi_path_exists("/tmp/id.png")) {
        mi_identify i;
        CHECK(mi_identify_probe("/tmp/id.png", NULL, &i) == 0, "a png is identified");
        CHECK(strcmp(i.video_codec, "png") == 0, "its codec is the image format");
        CHECK(!strcmp(i.extension, "png"), "the extension is png");
        CHECK(i.browser_playable == 1, "a png is playable");
    }

    /* Something that is not media at all must not be claimed as media. */
    {
        FILE *f = fopen("/tmp/id-notmedia.bin", "wb");
        if (f) {
            fputs("this is definitely not a media file, it is just text\n", f);
            fclose(f);
        }
        mi_identify n;
        int r = mi_identify_probe("/tmp/id-notmedia.bin", NULL, &n);
        CHECK(r != 0 || n.probed == 0, "a text file is not identified as media");
    }
}

int main(void) {
    mi_log_set_level(MI_LOG_ERROR);
    test_availability();
    test_missing_file();
    test_identify_live();
    printf("\n%d checks, %d failures", g_checks, g_failures);
    if (g_skipped) printf(", %d skipped", g_skipped);
    printf("\n");
    return g_failures == 0 ? 0 : 1;
}
