#ifndef VIDEO_H
#define VIDEO_H

/*
 * video.h — Smacker video playback
 *
 * Original binary:
 *   FUN_004a1660  video_play(hwnd, smk_filename) — open + start playback
 *   FUN_004a1c20  video_frame_tick()             — decode + display one frame
 *   FUN_004a1c60  video_stop()                   — stop + close
 *
 * We merge these into a single blocking call. The original polled PeekMessage
 * in a while() between ticks; we do the same via platform_pump_events().
 * Any keypress or window-close skips the video, matching original behaviour.
 *
 * Backend: libsmacker (lib/libsmacker/).
 * Audio:   SDL2 audio queue, opened per-video, closed when done.
 * Video:   8-bit palettized frames fed directly to render_set_palette +
 *          render_blit_indexed — no format conversion needed.
 */

#include <stdbool.h>

/*
 * video_play()
 *   Play a Smacker file to completion (or until any key / window close).
 *   path is relative to the asset root (fs_fopen handles case folding).
 *   Returns true if the file was found and playback started, false if not found.
 */
bool video_play(const char *path);

#endif /* VIDEO_H */
