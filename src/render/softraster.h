#ifndef SOFTRASTER_H
#define SOFTRASTER_H

#include <stdint.h>

/*
 * softraster.h — CPU triangle rasterizer into an 8-bit palettized framebuffer.
 *
 * The software-render backend: it mirrors the original game's software mode,
 * where the CPU rasterized the 3D scene into a system-memory 8-bit surface that
 * was then flipped to the display (here: presented via the GPU palette blit,
 * render_blit_indexed). Flat-shaded, z-buffered, two-sided (winding-agnostic).
 *
 * It consumes the SAME interleaved [pos3, nrm3] triangle soup the hardware path
 * uploads to the GPU, so a scene rendered SW vs HW differs only in the
 * rasterizer — exactly the comparison the original software/Glide split offered.
 *
 * Shading uses palette RAMPS: each material occupies `levels` consecutive
 * palette entries (dark -> bright); a triangle's lambert term picks the entry.
 * Caller lays out the ramps and builds the matching palette.
 */

typedef struct {
    int       w, h;
    uint8_t  *color;   /* w*h palette indices (the framebuffer)        */
    float    *depth;   /* w*h depth buffer (cleared to +inf each frame) */
} SoftTarget;

/* Clear colour to `bg` (a palette index) and depth to far. */
void soft_clear(SoftTarget *t, uint8_t bg);

/* Set the view + projection (column-major) used by subsequent soft_draw calls. */
void soft_set_viewproj(const float view[16], const float proj[16]);

/*
 * Rasterize an interleaved [px,py,pz, nx,ny,nz] triangle list (vertex_count a
 * multiple of 3) with the given column-major model matrix. The material's
 * shade ramp is palette indices [ramp_base, ramp_base+levels-1].
 */
void soft_draw(SoftTarget *t, const float *verts, uint32_t vertex_count,
               const float model[16], uint8_t ramp_base, uint8_t levels);

/*
 * Rasterize one triangle given WORLD-space positions `wp` (the current
 * view+projection is applied internally) with perspective-correct UV. If
 * `tex` is non-NULL, samples the tw*th 8-bit texture (wrapping) and writes the
 * texel index; otherwise fills with `flat_idx`. `uv` may be NULL (untextured).
 */
void soft_tri(SoftTarget *t, float wp[3][3], float uv[3][2],
              const uint8_t *tex, int tw, int th, uint8_t flat_idx);

#endif /* SOFTRASTER_H */
