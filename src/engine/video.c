/*
 * video.c — Smacker video playback via libsmacker
 *
 * Frame loop mirrors the original's PeekMessage + video_frame_tick() pattern.
 * Audio is fed to SDL2's audio queue so it plays asynchronously while we
 * decode and present the next video frame.
 *
 * smk_open_filepointer() takes ownership of the FILE* and closes it.
 */

#include "video.h"
#include "fs.h"
#include "font.h"
#include "fidelity.h"
#include "../render/render.h"
#include "../platform/platform.h"

#include "libsmacker/smacker.h"
#include <SDL2/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SKIP_HINT "Press space to skip"

/* Brightest palette entry, so the hint stays readable whatever the palette. */
static uint8_t brightest(const unsigned char *pal)
{
    int best = 0, best_l = -1;
    for (int i = 0; i < 256; i++) {
        int l = pal[i*3] * 3 + pal[i*3+1] * 6 + pal[i*3+2];
        if (l > best_l) { best_l = l; best = i; }
    }
    return (uint8_t)best;
}

bool video_play(const char *path)
{
    FILE *f = fs_fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[video] Not found: %s\n", path);
        return false;
    }

    /* smk_open_filepointer owns f from here — do not fclose() it */
    smk s = smk_open_filepointer(f, SMK_MODE_DISK);
    if (!s) {
        fprintf(stderr, "[video] Failed to open: %s\n", path);
        return false;
    }

    unsigned long frame_count;
    double usf; /* microseconds per frame */
    smk_info_all(s, NULL, &frame_count, &usf);

    unsigned long w, h;
    unsigned char y_scale;
    smk_info_video(s, &w, &h, &y_scale);

    unsigned char track_mask, channels[7], bitdepth[7];
    unsigned long audio_rate[7];
    smk_info_audio(s, &track_mask, channels, bitdepth, audio_rate);

    bool has_audio = (track_mask & SMK_AUDIO_TRACK_0) && audio_rate[0] > 0;
    smk_enable_video(s, 1);
    if (has_audio) smk_enable_audio(s, 0, 1);

    /* SDL2 audio queue — open device per-video */
    SDL_AudioDeviceID audio_dev = 0;
    if (has_audio) {
        SDL_AudioSpec want = {
            .freq     = (int)audio_rate[0],
            .format   = (bitdepth[0] == 16) ? AUDIO_S16LSB : AUDIO_U8,
            .channels = channels[0],
            .samples  = 4096,
            .callback = NULL,
        };
        SDL_AudioSpec got;
        audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
        if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);
    }

    /*
     * Y_DOUBLE: the file only encodes h/2 rows per frame; each row is meant
     * to be displayed twice. libsmacker does not perform the doubling — it
     * simply decodes the h/2 rows and leaves the rest of the w*h buffer
     * stale. Passing h/2 to render_blit_indexed lets the GPU stretch the
     * valid half to fill the screen via the nearest-neighbour sampler.
     *
     * Y_INTERLACE: only even rows are encoded. Same fix — pass h/2 and let
     * the GPU stretch. Not perfectly correct but avoids the garbage rows.
     */
    uint32_t blit_h = (y_scale != SMK_FLAG_Y_NONE) ? (uint32_t)(h / 2)
                                                    : (uint32_t)h;

    fprintf(stdout, "[video] Playing %s (%lux%lu → blit %ux%u, %lu frames, %.1f fps%s)\n",
            path, w, h, (uint32_t)w, blit_h, frame_count,
            usf > 0 ? 1e6 / usf : 0.0,
            has_audio ? ", audio" : "");

    /* Pre-queue a couple of frames of audio before starting to avoid an
     * underrun on the first few frames while the GPU pipeline warms up. */
    uint32_t frame_ms = usf > 0 ? (uint32_t)(usf / 1000.0) : 33;

    char ret = smk_first(s);

    /* Prime the audio queue with the first frame before we start the clock */
    if (has_audio && audio_dev && (ret == SMK_MORE || ret == SMK_LAST)) {
        const unsigned char *audio = smk_get_audio(s, 0);
        unsigned long        size  = smk_get_audio_size(s, 0);
        if (audio && size > 0)
            SDL_QueueAudio(audio_dev, audio, (uint32_t)size);
    }

    bool skipped = false;

    /* Skip hint, drawn over a copy of each frame (not in the original). */
    Font    *font  = fidelity_legacy() ? NULL : font_load("base6x7.fnt");
    uint8_t *frame = font ? malloc((size_t)w * blit_h) : NULL;

    while (ret == SMK_MORE || ret == SMK_LAST) {

        uint64_t frame_start = platform_get_ticks();

        /* Event pump — any key or mouse click skips; window-close quits.
         * Drain the WHOLE queue each frame: one-event-per-frame polling let a
         * flood of mouse-motion events bury the keypress for seconds. */
        for (;;) {
            PlatformEvent evt;
            if (!platform_pump_events(&evt)) { skipped = true; break; }
            if (evt.type == PLATFORM_EVENT_NONE) break;
            if (evt.type == PLATFORM_EVENT_KEY_DOWN ||
                evt.type == PLATFORM_EVENT_MOUSE_DOWN) skipped = true;
        }
        if (skipped) break;

        const unsigned char *palette = smk_get_palette(s);
        const unsigned char *pixels  = smk_get_video(s);

        if (palette && pixels) {
            render_begin_frame();
            render_set_palette((const Rgb8 *)palette);
            const unsigned char *shown = pixels;
            if (frame) {
                memcpy(frame, pixels, (size_t)w * blit_h);
                int tx = (int)w - font_text_width(font, SKIP_HINT) - 8;
                int ty = (int)blit_h - (int)font->height - 4;
                font_draw(font, SKIP_HINT, frame, (int)w, (int)blit_h,
                          tx, ty, brightest(palette));
                shown = frame;
            }
            render_blit_indexed(shown, (uint32_t)w, blit_h);
            render_end_frame();
        }

        if (ret == SMK_LAST) break;
        ret = smk_next(s);

        /* Queue audio for the *next* frame now so SDL has it ready */
        if (has_audio && audio_dev) {
            const unsigned char *audio = smk_get_audio(s, 0);
            unsigned long        size  = smk_get_audio_size(s, 0);
            if (audio && size > 0)
                SDL_QueueAudio(audio_dev, audio, (uint32_t)size);
        }

        /* Sleep only the time remaining in this frame budget */
        uint64_t elapsed = platform_get_ticks() - frame_start;
        if (elapsed < frame_ms)
            SDL_Delay((uint32_t)(frame_ms - elapsed));
    }

    if (audio_dev) {
        if (skipped) {
            /* User skipped: cut the sound instantly. */
            SDL_ClearQueuedAudio(audio_dev);
        } else {
            /* Ran to the end: drain queued audio so the end doesn't cut off */
            while (SDL_GetQueuedAudioSize(audio_dev) > 0)
                SDL_Delay(10);
        }
        SDL_CloseAudioDevice(audio_dev);
    }

    free(frame);
    if (font) font_free(font);

    smk_close(s);

    /* Leave a clean queue: the skip press (and anything else buffered during
     * playback) must not leak into the next video or the game/viewer loop. */
    platform_flush_events();

    return true;
}
