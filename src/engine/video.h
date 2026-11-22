#ifndef VIDEO_H
#define VIDEO_H

/*
 * video.h — Smacker video playback
 *
 * The original opens, ticks and stops a video in three calls; here it is
 * one blocking call. The original polled PeekMessage
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
