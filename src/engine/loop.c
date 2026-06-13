/*
 * loop.c — Gameplay frame loop (see loop.h)
 *
 * Fixed-timestep simulation, interpolated render:
 *
 *   accumulate real elapsed time, step the world in fixed FIXED_STEP chunks,
 *   then render once with the leftover time expressed as an interpolation
 *   factor (alpha). This keeps the simulation deterministic and frame-rate
 *   independent while still drawing smoothly at the display's rate.
 *
 * ORIGINAL BINARY:
 *   The hot loop is `while(DAT_004f30cc == 5)` in Fun_Real_Entry. Per
 *   REVERSING.md it pulls a time delta from FUN_004a2ce0 (returns seconds),
 *   prefetches the object pages (mem_prefetch_objects), pumps Win32 messages,
 *   updates, and presents. We reproduce that shape: time delta → input →
 *   fixed update(s) → render. FUN_004a2ce0's exact unit is still being
 *   confirmed; we use platform_get_ticks() (milliseconds) as the source.
 */

#include "loop.h"

#include <stdio.h>
#include <string.h>

#include "platform/platform.h"
#include "engine/gamestate.h"
#include "engine/input.h"
#include "engine/world.h"
#include "engine/font.h"
#include "render/render.h"

/* The framebuffer the world is software-rendered into, then GPU-blitted. */
#define FB_W 640
#define FB_H 480

/* Simulation step: 60 Hz. Deterministic; independent of display rate. */
#define FIXED_STEP   (1.0 / 60.0)

/*
 * Clamp on the per-frame delta. If the process stalls (debugger break, window
 * drag, heavy load) we don't want to "catch up" with a huge burst of ticks —
 * that's the classic spiral of death. Drop the extra time instead.
 */
#define MAX_FRAME_DT 0.25

/*
 * Render cap. The swapchain prefers MAILBOX (non-blocking) present, so without
 * a cap the loop would spin as fast as the GPU allows. Cap at the sim rate.
 */
#define TARGET_FRAME_MS (1000.0 / 60.0)

/* Debug HUD palette indices (see build_debug_palette). */
#define IDX_BG     0
#define IDX_TEXT   1
#define IDX_MARKER 2
#define IDX_ACCENT 3
#define IDX_DIM    4

static void build_debug_palette(Rgb8 *pal)
{
    memset(pal, 0, sizeof(Rgb8) * 256);
    pal[IDX_BG]     = (Rgb8){ 20,  20,  40 };   /* dark blue background     */
    pal[IDX_TEXT]   = (Rgb8){ 230, 230, 230 };  /* near-white HUD text      */
    pal[IDX_MARKER] = (Rgb8){ 80,  200, 80  };  /* green debug marker       */
    pal[IDX_ACCENT] = (Rgb8){ 220, 200, 60  };  /* amber, lit indicators    */
    pal[IDX_DIM]    = (Rgb8){ 90,  90,  90  };  /* grey, unlit indicators   */
}

/*
 * Debug overlay: frame stats and a row of input indicators that light up while
 * their button is held. This is what makes the loop verifiable by eye — it
 * proves timing, the render path, and input are all live. Temporary.
 */
static void draw_hud(const Font *font, uint8_t *fb, const InputState *in,
                     double fps, bool paused)
{
    if (font) {
        char line[96];
        snprintf(line, sizeof(line), "FPS %5.1f   STEP %.4fs%s",
                 fps, FIXED_STEP, paused ? "   [PAUSED]" : "");
        font_draw(font, line, fb, FB_W, FB_H, 8, 8, IDX_TEXT);
        font_draw(font, "ARROWS/WASD MOVE   P PAUSE   ESC QUIT",
                  fb, FB_W, FB_H, 8, 8 + (int)font->height + 4, IDX_DIM);
    }

    /* Input indicator bar along the bottom: one cell per button. */
    static const char *labels[INPUT_BTN_COUNT] =
        { "UP", "DN", "LF", "RT", "FIRE", "PAUSE" };
    int x = 8;
    int y = FB_H - 24;
    for (int i = 0; i < INPUT_BTN_COUNT; i++) {
        uint8_t col = in->held[i] ? IDX_ACCENT : IDX_DIM;
        int w = (int)strlen(labels[i]) * 6 + 8;
        for (int yy = y; yy < y + 14 && yy < FB_H; yy++)
            memset(fb + (size_t)yy * FB_W + x, col, (size_t)w);
        if (font)
            font_draw(font, labels[i], fb, FB_W, FB_H, x + 4, y + 4, IDX_BG);
        x += w + 6;
    }
}

void gameplay_run(void)
{
    static uint8_t fb[FB_W * FB_H];
    Rgb8 pal[256];
    build_debug_palette(pal);

    Font *font = font_load("base6x7.fnt");
    if (!font)
        fprintf(stdout, "[loop] base6x7.fnt not found — HUD text disabled.\n");

    input_init();
    world_init();

    fprintf(stdout, "[loop] Gameplay loop running (%.0f Hz sim). "
                    "ESC or close window to quit.\n", 1.0 / FIXED_STEP);

    double   accumulator = 0.0;
    uint64_t last        = platform_get_ticks();
    bool     paused      = false;

    /* FPS measured over a rolling one-second window. */
    double   fps          = 0.0;
    uint32_t fps_frames   = 0;
    uint64_t fps_mark      = last;

    while (g_gamestate == GS_GAMEPLAY) {
        uint64_t frame_start = platform_get_ticks();
        double   dt          = (double)(frame_start - last) / 1000.0;
        last = frame_start;
        if (dt > MAX_FRAME_DT)
            dt = MAX_FRAME_DT;

        /* --- input --- */
        InputState in;
        input_poll(&in);
        if (in.quit) {
            g_gamestate = GS_QUIT;
            break;
        }
        if (in.pressed[INPUT_BTN_PAUSE])
            paused = !paused;

        /* --- fixed-step simulation --- */
        if (!paused) {
            accumulator += dt;
            while (accumulator >= FIXED_STEP) {
                world_tick(&in, FIXED_STEP);
                accumulator -= FIXED_STEP;
            }
        }
        double alpha = accumulator / FIXED_STEP;

        /* --- render --- */
        render_begin_frame();
        world_render(fb, FB_W, FB_H, alpha);
        draw_hud(font, fb, &in, fps, paused);
        render_set_palette(pal);
        render_blit_indexed(fb, FB_W, FB_H);
        render_end_frame();

        /* --- fps bookkeeping --- */
        fps_frames++;
        if (frame_start - fps_mark >= 1000) {
            fps      = fps_frames * 1000.0 / (double)(frame_start - fps_mark);
            fps_frames = 0;
            fps_mark   = frame_start;
        }

        /* --- frame cap --- */
        double elapsed = (double)(platform_get_ticks() - frame_start);
        if (elapsed < TARGET_FRAME_MS)
            platform_sleep((uint32_t)(TARGET_FRAME_MS - elapsed));
    }

    world_shutdown();
    if (font)
        font_free(font);
}
