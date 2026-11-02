#define _GNU_SOURCE
#include "mi_hls.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mi_util.h"

static const mi_rendition DEFAULT_LADDER[] = {
    {1080, 5000, 128},
    {720, 2800, 128},
    {480, 1400, 96},
    {360, 800, 64},
};

const mi_rendition *mi_hls_default_ladder(size_t *count) {
    *count = sizeof(DEFAULT_LADDER) / sizeof(DEFAULT_LADDER[0]);
    return DEFAULT_LADDER;
}

void mi_hls_config_init(mi_hls_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->ffmpeg_path = "ffmpeg";
    cfg->segment_seconds = 6;
    cfg->gop_seconds = 2;
    cfg->renditions = NULL;
    cfg->rendition_count = 0;
    cfg->preset = "veryfast";
    cfg->audio_codec = "aac";
    cfg->audio_channels = 2;
    cfg->audio_sample_rate = 48000;
    cfg->overwrite = 1;
}

/* ------------------------------------------------------------------ */
/* Process runner                                                      */
/* ------------------------------------------------------------------ */

/* Run argv to completion, streaming stderr at debug level.
 * Returns the exit status (0 == success), or -1 if it could not start. */
static int run_process(char *const argv[]) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        /* Child */
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(pipefd[1]);
    FILE *err = fdopen(pipefd[0], "r");
    char line[1024];
    if (err) {
        while (fgets(line, sizeof line, err)) {
            size_t n = strlen(line);
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
            if (n > 0) mi_logf(MI_LOG_DEBUG, "ffmpeg: %s", line);
        }
        fclose(err);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* ------------------------------------------------------------------ */
/* ffprobe helpers                                                     */
/* ------------------------------------------------------------------ */

/* Return the first video stream height, or 0 if unknown. */
static int probe_height(const char *ffmpeg_path, const char *input) {
    char *ffprobe = mi_strdup(ffmpeg_path);
    if (!ffprobe) return 0;
    char *slash = strrchr(ffprobe, '/');
    if (slash) {
        memcpy(slash + 1, "ffprobe", 8);
    } else {
        free(ffprobe);
        ffprobe = mi_strdup("ffprobe");
    }
    if (!ffprobe) return 0;

    char *argv[] = {ffprobe, "-v", "error", "-select_streams", "v:0",
                    "-show_entries", "stream=height", "-of",
                    "default=noprint_wrappers=1:nokey=1", (char *)input, NULL};
    int pipefd[2];
    int height = 0;
    if (pipe(pipefd) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            dup2(pipefd[1], STDOUT_FILENO);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) dup2(devnull, STDERR_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
            execvp(argv[0], argv);
            _exit(127);
        } else if (pid > 0) {
            close(pipefd[1]);
            char buf[64] = {0};
            ssize_t got = read(pipefd[0], buf, sizeof buf - 1);
            close(pipefd[0]);
            int status = 0;
            waitpid(pid, &status, 0);
            if (got > 0) height = atoi(buf);
        } else {
            close(pipefd[0]);
            close(pipefd[1]);
        }
    }
    free(ffprobe);
    return height;
}

/* ------------------------------------------------------------------ */
/* Transcode                                                           */
/* ------------------------------------------------------------------ */

static int write_master_playlist(const char *out_dir, const mi_rendition *used,
                                 size_t used_count, mi_hls_result *out) {
    char *master = mi_path_join(out_dir, "master.m3u8");
    if (!master) return MI_ERR_NOMEM;
    FILE *f = fopen(master, "w");
    if (!f) {
        free(master);
        return MI_ERR_IO;
    }
    fprintf(f, "#EXTM3U\n#EXT-X-VERSION:3\n");
    for (size_t i = 0; i < used_count; i++) {
        int bw = (used[i].video_kbps + used[i].audio_kbps) * 1000;
        fprintf(f,
                "#EXT-X-STREAM-INF:BANDWIDTH=%d,RESOLUTION=%dx%d\n"
                "%dp/index.m3u8\n",
                bw, (int)(used[i].height * 16.0 / 9.0 + 0.5), used[i].height,
                used[i].height);
    }
    fclose(f);

    out->master_playlist = master;
    out->output_dir = mi_strdup(out_dir);
    out->rendition_count = used_count;
    out->variant_playlists = mi_calloc(used_count, sizeof(char *));
    if (!out->variant_playlists) return MI_ERR_NOMEM;
    for (size_t i = 0; i < used_count; i++) {
        char *dir = mi_asprintf("%dp", used[i].height);
        char *sub = mi_path_join(out_dir, dir);
        out->variant_playlists[i] = mi_path_join(sub, "index.m3u8");
        free(dir);
        free(sub);
    }
    return MI_OK;
}

int mi_hls_transcode(const mi_hls_config *cfg, const char *input,
                     const char *out_dir, mi_hls_result *out) {
    memset(out, 0, sizeof *out);

    size_t count = cfg->rendition_count;
    const mi_rendition *ladder = cfg->renditions;
    if (!ladder || count == 0) ladder = mi_hls_default_ladder(&count);

    if (mi_mkdir_p(out_dir) != 0) {
        mi_logf(MI_LOG_ERROR, "cannot create output dir %s", out_dir);
        return MI_ERR_IO;
    }

    /* Only keep renditions at or below the source height (avoid upscaling). */
    int source_h = probe_height(cfg->ffmpeg_path, input);
    const mi_rendition *used = ladder;
    size_t used_count = count;
    mi_rendition *filtered = NULL;
    if (source_h > 0) {
        filtered = mi_calloc(count, sizeof(mi_rendition));
        if (!filtered) return MI_ERR_NOMEM;
        size_t n = 0;
        for (size_t i = 0; i < count; i++) {
            if (ladder[i].height <= source_h) filtered[n++] = ladder[i];
        }
        if (n == 0) {
            /* Source smaller than every rung: use the smallest one. */
            int best = 0;
            for (size_t i = 1; i < count; i++)
                if (ladder[i].height < ladder[best].height) best = (int)i;
            filtered[0] = ladder[best];
            n = 1;
        }
        used = filtered;
        used_count = n;
    }

    mi_logf(MI_LOG_INFO, "transcoding %s -> %zu rendition(s)%s", input, used_count,
            source_h > 0 ? "" : " (source height unknown)");

    int rc = MI_OK;
    for (size_t i = 0; i < used_count && rc == MI_OK; i++) {
        char *variant = mi_asprintf("%s/%dp", out_dir, used[i].height);
        if (mi_mkdir_p(variant) != 0) {
            free(variant);
            rc = MI_ERR_IO;
            break;
        }
        char *playlist = mi_path_join(variant, "index.m3u8");
        char *segments = mi_path_join(variant, "seg_%03d.ts");

        char scale[64];
        snprintf(scale, sizeof scale, "scale=-2:%d", used[i].height);
        char gop[32];
        snprintf(gop, sizeof gop, "%d", cfg->gop_seconds * 25);
        char *vb = mi_asprintf("%dk", used[i].video_kbps);
        char *ab = mi_asprintf("%dk", used[i].audio_kbps);
        char *hls_time = mi_asprintf("%d", cfg->segment_seconds);
        char *channels = mi_asprintf("%d", cfg->audio_channels);
        char *rate = mi_asprintf("%d", cfg->audio_sample_rate);

        char *argv[48];
        int a = 0;
        argv[a++] = (char *)cfg->ffmpeg_path;
        if (cfg->overwrite) argv[a++] = "-y";
        argv[a++] = "-i";
        argv[a++] = (char *)input;
        argv[a++] = "-vf";
        argv[a++] = scale;
        argv[a++] = "-c:v";
        argv[a++] = "libx264";
        argv[a++] = "-preset";
        argv[a++] = (char *)cfg->preset;
        argv[a++] = "-profile:v";
        argv[a++] = "main";
        argv[a++] = "-pix_fmt";
        argv[a++] = "yuv420p";
        argv[a++] = "-b:v";
        argv[a++] = vb;
        argv[a++] = "-maxrate";
        argv[a++] = vb;
        argv[a++] = "-bufsize";
        argv[a++] = vb;
        argv[a++] = "-g";
        argv[a++] = gop;
        argv[a++] = "-keyint_min";
        argv[a++] = gop;
        argv[a++] = "-sc_threshold";
        argv[a++] = "0";
        argv[a++] = "-c:a";
        argv[a++] = (char *)cfg->audio_codec;
        argv[a++] = "-b:a";
        argv[a++] = ab;
        argv[a++] = "-ac";
        argv[a++] = channels;
        argv[a++] = "-ar";
        argv[a++] = rate;
        argv[a++] = "-f";
        argv[a++] = "hls";
        argv[a++] = "-hls_time";
        argv[a++] = hls_time;
        argv[a++] = "-hls_playlist_type";
        argv[a++] = "vod";
        argv[a++] = "-hls_segment_filename";
        argv[a++] = segments;
        argv[a++] = playlist;
        argv[a++] = NULL;

        int status = run_process(argv);
        if (status != 0) {
            mi_logf(MI_LOG_ERROR, "ffmpeg failed for %dp (exit %d)", used[i].height,
                    status);
            rc = MI_ERR;
        }
        free(variant);
        free(playlist);
        free(segments);
        free(vb);
        free(ab);
        free(hls_time);
        free(channels);
        free(rate);
    }

    if (rc == MI_OK) {
        rc = write_master_playlist(out_dir, used, used_count, out);
    }
    free(filtered);
    return rc;
}

void mi_hls_result_free(mi_hls_result *res) {
    if (!res) return;
    free(res->master_playlist);
    free(res->output_dir);
    if (res->variant_playlists) {
        for (size_t i = 0; i < res->rendition_count; i++)
            free(res->variant_playlists[i]);
        free(res->variant_playlists);
    }
    memset(res, 0, sizeof *res);
}
