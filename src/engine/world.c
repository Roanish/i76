/*
 * world.c — Simulated world (see world.h)
 *
 * ============================ PLACEHOLDER ==============================
 * There is no real world yet. Everything below is a single debug "marker"
 * that the player can drive around with the input buttons. Its only job is
 * to prove the frame loop end-to-end: input → fixed-step integration →
 * interpolated render. It will be deleted when the game-object system (the
 * g_obj_buckets entity store) lands here.
 * =======================================================================
 */

#include "world.h"

#include <string.h>

/* Palette indices the debug build draws with (see palette in loop.c). */
#define IDX_BG      0
#define IDX_MARKER  2

/* Marker dynamics, in framebuffer pixels and pixels/second. */
#define MARKER_SIZE   16
#define MARKER_ACCEL  900.0   /* px/s^2 while a direction is held */
#define MARKER_DRAG   3.0     /* velocity damping per second      */
#define MARKER_VMAX   400.0   /* px/s clamp                        */

/* World bounds — for now just the 640x480 framebuffer. */
#define WORLD_PX_W  640
#define WORLD_PX_H  480

typedef struct {
    double x, y;            /* current sim position (center)      */
    double prev_x, prev_y;  /* position at the previous tick      */
    double vx, vy;          /* velocity                           */
} Marker;

static Marker s_marker;

static double clampd(double v, double lo, double hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void world_init(void)
{
    memset(&s_marker, 0, sizeof(s_marker));
    s_marker.x = s_marker.prev_x = WORLD_PX_W / 2.0;
    s_marker.y = s_marker.prev_y = WORLD_PX_H / 2.0;
}

void world_shutdown(void)
{
    /* Nothing to release yet. */
}

void world_tick(const InputState *in, double dt)
{
    s_marker.prev_x = s_marker.x;
    s_marker.prev_y = s_marker.y;

    double ax = 0.0, ay = 0.0;
    if (in->held[INPUT_BTN_LEFT])  ax -= MARKER_ACCEL;
    if (in->held[INPUT_BTN_RIGHT]) ax += MARKER_ACCEL;
    if (in->held[INPUT_BTN_UP])    ay -= MARKER_ACCEL;
    if (in->held[INPUT_BTN_DOWN])  ay += MARKER_ACCEL;

    s_marker.vx += ax * dt;
    s_marker.vy += ay * dt;

    /* Exponential drag so the marker coasts to a stop when nothing is held. */
    double damp = 1.0 - MARKER_DRAG * dt;
    if (damp < 0.0) damp = 0.0;
    s_marker.vx *= damp;
    s_marker.vy *= damp;

    s_marker.vx = clampd(s_marker.vx, -MARKER_VMAX, MARKER_VMAX);
    s_marker.vy = clampd(s_marker.vy, -MARKER_VMAX, MARKER_VMAX);

    s_marker.x += s_marker.vx * dt;
    s_marker.y += s_marker.vy * dt;

    /* Keep the marker on screen. */
    double half = MARKER_SIZE / 2.0;
    s_marker.x = clampd(s_marker.x, half, WORLD_PX_W - half);
    s_marker.y = clampd(s_marker.y, half, WORLD_PX_H - half);
}

static void fill_rect(uint8_t *fb, int w, int h, int x0, int y0, int rw, int rh,
                      uint8_t color)
{
    int x1 = x0 + rw, y1 = y0 + rh;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (int y = y0; y < y1; y++)
        memset(fb + (size_t)y * w + x0, color, (size_t)(x1 - x0));
}

void world_render(uint8_t *fb, int w, int h, double alpha)
{
    memset(fb, IDX_BG, (size_t)w * h);

    /* Interpolate between the previous and current tick for smooth motion. */
    double ix = s_marker.prev_x + (s_marker.x - s_marker.prev_x) * alpha;
    double iy = s_marker.prev_y + (s_marker.y - s_marker.prev_y) * alpha;

    int px = (int)(ix - MARKER_SIZE / 2.0);
    int py = (int)(iy - MARKER_SIZE / 2.0);
    fill_rect(fb, w, h, px, py, MARKER_SIZE, MARKER_SIZE, IDX_MARKER);
}
