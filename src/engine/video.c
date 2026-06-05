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
#include "../render/render.h"
#include "../platform/platform.h"

#include "libsmacker/smacker.h"
#include <SDL2/SDL.h>

#include <stdio.h>

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

    while (ret == SMK_MORE || ret == SMK_LAST) {

        uint64_t frame_start = platform_get_ticks();

        /* Event pump — any key or window-close skips */
        PlatformEvent evt;
        if (!platform_pump_events(&evt)) break;
        if (evt.type == PLATFORM_EVENT_KEY_DOWN) break;

        const unsigned char *palette = smk_get_palette(s);
        const unsigned char *pixels  = smk_get_video(s);

        if (palette && pixels) {
            render_begin_frame();
            render_set_palette((const Rgb8 *)palette);
            render_blit_indexed(pixels, (uint32_t)w, blit_h);
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
        /* Drain queued audio before closing so the end doesn't cut off */
        while (SDL_GetQueuedAudioSize(audio_dev) > 0)
            SDL_Delay(10);
        SDL_CloseAudioDevice(audio_dev);
    }

    smk_close(s);
    return true;
}
