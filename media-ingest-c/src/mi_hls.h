/* mi_hls.h - FFmpeg -> HLS transcode pipeline.
 *
 * Transcodes a source file into an adaptive-bitrate HLS ladder (multiple
 * renditions + a master playlist) suitable for serving behind a CDN.
 */
#ifndef MI_HLS_H
#define MI_HLS_H

#include <stddef.h>

typedef struct {
    int height;      /* rendition height, e.g. 360, 720, 1080 */
    int video_kbps;  /* target video bitrate */
    int audio_kbps;  /* target audio bitrate */
} mi_rendition;

typedef struct {
    const char *ffmpeg_path;   /* default "ffmpeg" */
    int segment_seconds;       /* HLS target duration, default 6 */
    int gop_seconds;           /* keyframe interval, default 2 */
    const mi_rendition *renditions;
    size_t rendition_count;
    const char *preset;        /* x264 preset, default "veryfast" */
    const char *audio_codec;   /* default "aac" */
    int audio_channels;        /* default 2 */
    int audio_sample_rate;     /* default 48000 */
    int overwrite;             /* add -y */
} mi_hls_config;

typedef struct {
    char *master_playlist;     /* path to master.m3u8 */
    char *output_dir;
    size_t rendition_count;
    char **variant_playlists;  /* one per rendition */
} mi_hls_result;

void mi_hls_config_init(mi_hls_config *cfg);

/* Transcode `input` into `out_dir`. Returns 0 or a negative MI_ERR_*. */
int mi_hls_transcode(const mi_hls_config *cfg, const char *input,
                     const char *out_dir, mi_hls_result *out);
void mi_hls_result_free(mi_hls_result *res);

/* Default 1080/720/480/360 ladder. */
const mi_rendition *mi_hls_default_ladder(size_t *count);

#endif /* MI_HLS_H */
