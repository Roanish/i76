/*
 * smk_dump.c — Decode a Smacker video to AVI/MP4/etc. via ffmpeg.
 *
 * Usage:
 *   smk_dump <input.smk> <output.[avi|mp4|mkv|...]>
 *
 * How it works:
 *   Pass 1 — decode all frames:
 *     - Video: RGB24 frames piped to ffmpeg → video-only temp file
 *     - Audio: raw PCM written to <output>.pcm
 *   Pass 2 — mux:
 *     - ffmpeg combines the video temp + PCM into the final output
 *   Temp files are deleted on completion.
 *
 * Y_DOUBLE / Y_INTERLACE:
 *   libsmacker only decodes h/2 rows per frame. We duplicate each row so
 *   the output has the correct display height.
 *
 * Dependencies: libsmacker (lib/), ffmpeg in PATH.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libsmacker/smacker.h"

/* -----------------------------------------------------------------------
 * Palette expansion + row doubling
 * ----------------------------------------------------------------------- */

static void expand_palette(const unsigned char *pixels,
                           const unsigned char *palette,
                           unsigned char *rgb,
                           unsigned long n_pixels)
{
    for (unsigned long i = 0; i < n_pixels; i++) {
        unsigned char idx = pixels[i];
        rgb[i * 3 + 0] = palette[idx * 3 + 0];
        rgb[i * 3 + 1] = palette[idx * 3 + 1];
        rgb[i * 3 + 2] = palette[idx * 3 + 2];
    }
}

static void double_rows(const unsigned char *src, unsigned char *dst,
                        unsigned long w, unsigned long enc_h)
{
    for (unsigned long y = 0; y < enc_h; y++) {
        const unsigned char *row = src + y * w * 3;
        memcpy(dst + (y * 2)     * w * 3, row, w * 3);
        memcpy(dst + (y * 2 + 1) * w * 3, row, w * 3);
    }
}

/* -----------------------------------------------------------------------
 * Main
 * ----------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: smk_dump <input.smk> <output.avi|.mp4|.mkv>\n");
        return 1;
    }

    const char *in_path  = argv[1];
    const char *out_path = argv[2];

    smk s = smk_open_file(in_path, SMK_MODE_DISK);
    if (!s) {
        fprintf(stderr, "smk_dump: cannot open '%s'\n", in_path);
        return 1;
    }

    unsigned long frame_count;
    double usf;
    smk_info_all(s, NULL, &frame_count, &usf);

    unsigned long w, h;
    unsigned char y_scale;
    smk_info_video(s, &w, &h, &y_scale);

    unsigned char track_mask, channels[7], bitdepth[7];
    unsigned long audio_rate[7];
    smk_info_audio(s, &track_mask, channels, bitdepth, audio_rate);

    int has_audio = (track_mask & SMK_AUDIO_TRACK_0) && audio_rate[0] > 0;
    int y_doubled = (y_scale == SMK_FLAG_Y_DOUBLE || y_scale == SMK_FLAG_Y_INTERLACE);

    unsigned long enc_h  = y_doubled ? h / 2 : h;
    unsigned long disp_h = h;
    double fps = usf > 0 ? 1e6 / usf : 15.0;

    fprintf(stderr, "Input : %s\n", in_path);
    fprintf(stderr, "Video : %lux%lu @ %.3f fps, %lu frames%s\n",
            w, disp_h, fps, frame_count,
            y_doubled ? " (Y_DOUBLE)" : "");
    if (has_audio)
        fprintf(stderr, "Audio : %lu Hz, %d ch, %d-bit\n",
                audio_rate[0], channels[0], bitdepth[0]);

    /* ----------------------------------------------------------------
     * Temp paths
     * ---------------------------------------------------------------- */

    char tmp_video[512], tmp_audio[512];
    snprintf(tmp_video, sizeof(tmp_video), "%s.tmp_video.mkv", out_path);
    snprintf(tmp_audio, sizeof(tmp_audio), "%s.tmp_audio.pcm", out_path);

    /* ----------------------------------------------------------------
     * Pass 1: video pipe → ffmpeg → tmp_video
     * ---------------------------------------------------------------- */

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -loglevel warning"
        " -f rawvideo -pix_fmt rgb24 -s %lux%lu -r %.6f -i pipe:0"
        " -c:v libx264 -crf 17 -pix_fmt yuv420p"
        " \"%s\"",
        w, disp_h, fps, tmp_video);

    fprintf(stderr, "Pass 1: encoding video...\n");

    FILE *ff = popen(cmd, "w");
    if (!ff) { fprintf(stderr, "smk_dump: cannot spawn ffmpeg\n"); smk_close(s); return 1; }

    FILE *af = has_audio ? fopen(tmp_audio, "wb") : NULL;

    smk_enable_video(s, 1);
    if (has_audio) smk_enable_audio(s, 0, 1);

    unsigned char *rgb_enc  = malloc(w * enc_h  * 3);
    unsigned char *rgb_disp = malloc(w * disp_h * 3);
    if (!rgb_enc || !rgb_disp) { fprintf(stderr, "OOM\n"); return 1; }

    char ret = smk_first(s);
    unsigned long n = 0;

    while (ret == SMK_MORE || ret == SMK_LAST) {
        const unsigned char *palette = smk_get_palette(s);
        const unsigned char *pixels  = smk_get_video(s);

        if (palette && pixels) {
            expand_palette(pixels, palette, rgb_enc, w * enc_h);
            if (y_doubled)
                double_rows(rgb_enc, rgb_disp, w, enc_h);
            else
                memcpy(rgb_disp, rgb_enc, w * disp_h * 3);
            fwrite(rgb_disp, 1, w * disp_h * 3, ff);
        }

        if (af) {
            const unsigned char *audio = smk_get_audio(s, 0);
            unsigned long        size  = smk_get_audio_size(s, 0);
            if (audio && size > 0) fwrite(audio, 1, size, af);
        }

        n++;
        fprintf(stderr, "\r  frame %lu / %lu", n, frame_count);

        if (ret == SMK_LAST) break;
        ret = smk_next(s);
    }
    fprintf(stderr, "\n");

    free(rgb_enc);
    free(rgb_disp);
    smk_close(s);
    if (af) fclose(af);
    pclose(ff);

    /* ----------------------------------------------------------------
     * Pass 2: mux video + audio into final output
     * ---------------------------------------------------------------- */

    if (has_audio && af) {
        fprintf(stderr, "Pass 2: muxing audio...\n");
        snprintf(cmd, sizeof(cmd),
            "ffmpeg -y -loglevel warning"
            " -i \"%s\""
            " -f %s -ar %lu -ac %d -i \"%s\""
            " -c:v copy -c:a aac"
            " \"%s\"",
            tmp_video,
            (bitdepth[0] == 16) ? "s16le" : "u8",
            audio_rate[0], channels[0], tmp_audio,
            out_path);
        system(cmd);
        remove(tmp_audio);
        remove(tmp_video);
    } else {
        /* No audio — just rename the temp to the final output */
        rename(tmp_video, out_path);
    }

    fprintf(stderr, "Done  : %s\n", out_path);
    return 0;
}
