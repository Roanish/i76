#ifndef TEXTURE_H
#define TEXTURE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * texture.h — 8-bit palettized texture loader for mesh faces.
 *
 * Resolves a face's texture name (from the OEG mesh) to an 8-bit indexed `.map`
 * bitmap and caches the decoded pixels. Mirrors the reversed resolution chain
 * (see docs/REVERSING.md "Mesh texture resolution chain"):
 *   - a `.tmt` is a Texture Map Table — a header + an array of `.map` base names
 *     (one per animation frame/state); we take entry 0 for a static view.
 *   - the actual pixels are always a `.map`: [u32 w][u32 h] + w*h palette indices.
 * Indices are into the mission `.act` palette (load with palette_load_act).
 */

typedef struct {
    int      w, h;
    uint8_t *pixels;   /* w*h palette indices, or NULL if the texture failed */
} Texture;

/*
 * Resolve + decode a face texture by name (the raw 16-byte name from the mesh,
 * with or without extension). Returns a cached Texture, or NULL if it has no
 * name / can't be resolved. Do not free — owned by the cache.
 */
const Texture *texture_get(const char *name);

/* Drop all cached textures (call when changing scene). */
void texture_cache_clear(void);

#endif /* TEXTURE_H */
