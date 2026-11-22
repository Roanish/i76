/*
 * lighting.c — per-mission global lighting (see lighting.h for what is
 * faithful vs modernised).
 */

#include "engine/lighting.h"

#include <math.h>
#include <stdio.h>

/* The original's phase table: day flag, celestial texture, angle, intensity,
 * ambient. Ambient is 0 in every phase. */
typedef struct { int day; const char *tex; float angle, intensity, ambient; } Phase;
static const Phase PHASES[8] = {
    { 0, "nk_1mon1.map", 0.52359879f, 0.09375f, 0.0f },   /* 22-00 */
    { 0, "nk_1mon1.map", 1.22173047f, 0.03125f, 0.0f },   /* 01-03 */
    { 1, "nk_1sun1.map", 1.30899692f, 0.625f,   0.0f },   /* 04-06 */
    { 1, "nk_1sun1.map", 0.95993108f, 1.0f,     0.0f },   /* 07-09 */
    { 1, "nk_1sun1.map", 0.17453292f, 1.0f,     0.0f },   /* 10-12 */
    { 1, "nk_1sun1.map", 0.78539819f, 1.0f,     0.0f },   /* 13-15 */
    { 1, "nk_1sun1.map", 1.22173047f, 1.0f,     0.0f },   /* 16-18 */
    { 1, "nk_1sun1.map", 1.30899692f, 1.0f,     0.0f },   /* 19-21 */
};

/* Depth haze, the original's constants, clamped to [0.75, 1]. */
#define HAZE_BASE  0.74958265f
#define HAZE_SLOPE 0.00041735f
#define HAZE_MIN   0.75f

/* .lum = 32 shade rows x 256-colour remap (rows 32+ are not shade rows).
 * Brightness response per row, per channel = mean colour of the remapped
 * palette over the mean of the base palette. The span stage picks the row as
 * (1 - I) * 31.5, so row 0 = full light (~1.4x),
 * row 31 = no light (~0.21x) — the original's non-linear response, as data. */
static void measure_curve(const uint8_t *pal, const uint8_t *lum, size_t lum_size,
                          float curve[RENDER_LIGHT_ROWS][3])
{
    if (!pal || !lum || lum_size < (size_t)RENDER_LIGHT_ROWS * 256) {
        /* No table: a linear stand-in over the typical range (not data). */
        for (int r = 0; r < RENDER_LIGHT_ROWS; r++)
            curve[r][0] = curve[r][1] = curve[r][2] =
                1.4f + (0.21f - 1.4f) * (float)r / (RENDER_LIGHT_ROWS - 1);
        return;
    }
    double base[3] = { 0, 0, 0 };
    for (int c = 0; c < 256; c++)
        for (int ch = 0; ch < 3; ch++) base[ch] += pal[c * 3 + ch];
    for (int r = 0; r < RENDER_LIGHT_ROWS; r++) {
        const uint8_t *row = lum + r * 256;
        for (int ch = 0; ch < 3; ch++) {
            double sum = 0;
            for (int c = 0; c < 256; c++) sum += pal[row[c] * 3 + ch];
            curve[r][ch] = base[ch] > 0 ? (float)(sum / base[ch]) : 1.0f;
        }
    }
}

void lighting_build(WorldLighting *o, int hour,
                    const uint8_t *pal768, const uint8_t *lum, size_t lum_size)
{
    o->hour = hour;
    int h = hour < 0 ? 12 : hour % 24;                /* no WRLD hour: noon */
    o->phase = ((h + 2) % 24) * 8 / 24;
    const Phase *p = &PHASES[o->phase];
    o->is_day    = p->day;
    o->angle     = p->angle;
    o->intensity = p->intensity;
    o->body_tex  = p->tex;

    RenderLighting *g = &o->gpu;
    /* Toward the light: row 1 of the rotation about Z by `angle`, used with
     * the outward normal. */
    g->light_dir[0] = sinf(p->angle);
    g->light_dir[1] = cosf(p->angle);
    g->light_dir[2] = 0.0f;

    g->intensity = p->intensity;
    g->ambient   = p->ambient;                        /* 0 in every phase */
    measure_curve(pal768, lum, lum_size, g->curve);
    g->haze_base  = HAZE_BASE;
    g->haze_slope = HAZE_SLOPE;
    g->haze_min   = HAZE_MIN;

    fprintf(stdout, "[light] hour %d -> phase %d (%s, %.0f deg, I=%.3f), curve row0 %.2f %.2f %.2f"
                    " row7 %.2f row31 %.2f %.2f %.2f\n",
            hour, o->phase, p->day ? "sun" : "moon", p->angle * 57.29578f, p->intensity,
            g->curve[0][0], g->curve[0][1], g->curve[0][2], g->curve[7][1],
            g->curve[31][0], g->curve[31][1], g->curve[31][2]);
}
