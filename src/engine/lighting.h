#ifndef LIGHTING_H
#define LIGHTING_H

#include <stdint.h>
#include <stddef.h>

#include "render/render.h"   /* RenderLighting */

/*
 * lighting.h — per-mission global lighting from the original's time of day.
 *
 * Faithful inputs (docs/REVERSING.md "Time of day + global lighting"):
 *   phase = ((hour + 2) % 24) * 8 / 24      (8 phases of 3 hours)
 *   per phase: sun or moon, angle about Z, intensity, ambient (always 0)
 *   light direction (toward the light) = (sin a, cos a, 0)
 *   depth haze = clamp(0.7496 + 0.000417 * depth, 0.75, 1)
 *
 * Also faithful: the brightness response. The span stage maps intensity to a
 * .lum shade row, row = (1 - I) * 31.5; we measure each row's brightness from
 * the mission's .lum (per channel, so its tint too) and the shader applies
 * that curve. Modernised: per-pixel evaluation, and interpolation between
 * rows instead of 32 discrete bands.
 */

typedef struct {
    int   hour;          /* input hour (0..23; -1 = mission had none -> noon) */
    int   phase;         /* 0..7                                            */
    int   is_day;        /* sun (1) or moon (0)                             */
    float angle;         /* radians, about the world Z axis                 */
    float intensity;     /* direct light intensity                          */
    const char *body_tex;/* "nk_1sun1.map" / "nk_1mon1.map" (sky renderer)  */
    RenderLighting gpu;  /* ready for render_set_lighting()                 */
} WorldLighting;

/*
 * Build the lighting for `hour`. pal768 = mission palette (256 x RGB);
 * lum/lum_size = the mission .lum (64 KB; may be NULL -> neutral grey ambient).
 */
void lighting_build(WorldLighting *out, int hour,
                    const uint8_t *pal768, const uint8_t *lum, size_t lum_size);

#endif /* LIGHTING_H */
