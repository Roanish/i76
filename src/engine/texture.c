/*
 * texture.c — mesh face texture loader (see texture.h).
 *
 * Resolution (deterministic, extension-driven — mirrors the original
 * texture_load): a face name (lowercased, extension dropped) is resolved via
 * vfs_lod, trying ".tmt" (a name table -> real base), ".vqm" (vector-quantized),
 * then ".map" (8-bit indexed) in priority order. ".m16" (16-bit truecolour) is
 * recognised but skipped (the pipeline is 8-bit). vfs_lod is extension-aware and
 * resolves both pak records and loose files, so object textures packed in a
 * material pak (e.g. AN_1BDT1.vqm in anbunk1m.pak) resolve the same as loose
 * files. Results are cached by name in a small linked list.
 */

#include "texture.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/pix.h"   /* vfs_lod — extension-aware PIX/PAK + loose-file resolver */

/* .tmt header: count of index dims @+0x14, dim sizes @+0x18, name array @+0x40
 * (8-byte base names). We only need entry 0 for a static (non-animated) view. */
#define TMT_NAMES_OFF 0x40

typedef struct TexNode {
    char            name[20];
    Texture         tex;
    struct TexNode *next;
} TexNode;

static TexNode *s_cache = NULL;

static void str_lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

/* Decode an 8-bit `.map`: [u32 w][u32 h] + w*h index bytes. */
static bool decode_map(const uint8_t *buf, size_t sz, Texture *t)
{
    if (sz < 8) return false;
    uint32_t w, h;
    memcpy(&w, buf, 4);
    memcpy(&h, buf + 4, 4);
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return false;
    if (sz < 8 + (size_t)w * h) return false;
    t->pixels = malloc((size_t)w * h);
    if (!t->pixels) return false;
    memcpy(t->pixels, buf + 8, (size_t)w * h);
    t->w = (int)w;
    t->h = (int)h;
    return true;
}

/*
 * Decode a `.vqm` (4x4-block vector quantization) into 8-bit pixels.
 *   vqm:  [u32 w][u32 h][16-byte codebook name @ +8], tokens (u16/block) at +0x18.
 *   book: [4-byte header][16-byte 4x4 tiles] (the `.cbk`, named at vqm+8).
 * Token bit 0x8000 -> solid block (low byte = index); else codebook[token].
 */
static bool decode_vqm(const uint8_t *vqm, size_t vsz,
                       const uint8_t *book, size_t bsz, Texture *t)
{
    if (vsz < 0x18) return false;
    uint32_t w, h;
    memcpy(&w, vqm, 4);
    memcpy(&h, vqm + 4, 4);
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return false;

    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    if (vsz < (size_t)0x18 + (size_t)bw * bh * 2) return false;

    uint8_t *px = malloc((size_t)w * h);
    if (!px) return false;

    const uint8_t *tk = vqm + 0x18;
    for (uint32_t by = 0; by < h; by += 4) {
        for (uint32_t bx = 0; bx < w; bx += 4) {
            uint16_t v = (uint16_t)(tk[0] | (tk[1] << 8));
            tk += 2;
            for (uint32_t r = 0; r < 4 && by + r < h; r++) {
                for (uint32_t c = 0; c < 4 && bx + c < w; c++) {
                    uint8_t p;
                    if (v & 0x8000) {
                        p = (uint8_t)v;
                    } else {
                        size_t bi = 4 + (size_t)v * 16 + r * 4 + c;
                        p = (bi < bsz) ? book[bi] : 0;
                    }
                    px[(size_t)(by + r) * w + (bx + c)] = p;
                }
            }
        }
    }
    t->pixels = px;
    t->w = (int)w;
    t->h = (int)h;
    return true;
}

/* Decode an in-memory .vqm buffer: read its codebook name (@ +8), load the
 * codebook (a direct VFS file, e.g. ambient.cbk) and vector-dequantize into t. */
static void decode_vqm_buf(const void *vqm, size_t vsz, Texture *t)
{
    if (vsz < 0x18) return;
    char cbk[24];
    memcpy(cbk, (const char *)vqm + 8, 16);
    cbk[16] = '\0';
    str_lower(cbk);
    if (!strchr(cbk, '.')) strncat(cbk, ".cbk", sizeof cbk - strlen(cbk) - 1);
    size_t csz = 0;
    void *book = vfs_lod(cbk, &csz);   /* codebook may be pak'd or a direct file */
    if (book) {
        decode_vqm(vqm, vsz, book, csz, t);
        free(book);
    }
}

/* Read entry 0's "<base>.map" name from a `.tmt` blob into out (>=20 bytes). */
static void tmt_entry0(const void *buf, size_t sz, char *out)
{
    if (sz >= TMT_NAMES_OFF + 8) {
        char base[9];
        memcpy(base, (const uint8_t *)buf + TMT_NAMES_OFF, 8);
        base[8] = '\0';
        /* names are NUL-padded within 8 bytes */
        snprintf(out, 20, "%s.map", base);
    } else {
        out[0] = '\0';
    }
}

/* Resolve a base name to decoded pixels, deterministically by extension,
 * through the (extension-aware) VFS+pak index — which covers pak records and
 * loose files alike. Faithful priority: a ".tmt" name-table (entry 0 names the
 * real base) -> ".vqm" (vector-quantized) -> ".map" (8-bit indexed). A ".m16"
 * (16-bit truecolour) record is recognised but not renderable in the 8-bit
 * pipeline, so it's reported and skipped rather than mis-decoded. */
static void resolve_texture(const char *base, Texture *t, int depth)
{
    char name[24];
    size_t sz;
    void *buf;
    if (depth > 3) return;

    snprintf(name, sizeof name, "%s.tmt", base);
    if ((buf = vfs_lod(name, &sz))) {
        char m[20];
        tmt_entry0(buf, sz, m);
        free(buf);
        if (m[0]) {
            str_lower(m);
            char *d = strchr(m, '.');
            if (d) *d = '\0';
            resolve_texture(m, t, depth + 1);
            if (t->pixels) return;
        }
    }
    snprintf(name, sizeof name, "%s.vqm", base);
    if ((buf = vfs_lod(name, &sz))) {
        decode_vqm_buf(buf, sz, t);
        free(buf);
        if (t->pixels) return;
    }
    snprintf(name, sizeof name, "%s.map", base);
    if ((buf = vfs_lod(name, &sz))) {
        decode_map(buf, sz, t);
        free(buf);
        if (t->pixels) return;
    }
    snprintf(name, sizeof name, "%s.m16", base);
    if ((buf = vfs_lod(name, &sz))) {
        free(buf);
        fprintf(stderr, "[tex] %s.m16: 16-bit texture unsupported (skipped)\n", base);
    }
}

const Texture *texture_get(const char *rawname)
{
    if (!rawname || !rawname[0]) return NULL;

    char name[20];
    size_t n = 0;
    while (n < 16 && rawname[n]) { name[n] = rawname[n]; n++; }
    name[n] = '\0';
    if (!name[0]) return NULL;
    str_lower(name);

    for (TexNode *c = s_cache; c; c = c->next)
        if (strcmp(c->name, name) == 0)
            return c->tex.pixels ? &c->tex : NULL;

    /* Base name without extension. */
    char base[20];
    strncpy(base, name, sizeof base);
    base[sizeof base - 1] = '\0';
    char *d = strchr(base, '.');
    if (d) *d = '\0';

    TexNode *node = calloc(1, sizeof *node);
    strncpy(node->name, name, sizeof node->name - 1);

    resolve_texture(base, &node->tex, 0);

    if (!node->tex.pixels)
        fprintf(stderr, "[tex] MISSING %s\n", name);

    node->next = s_cache;
    s_cache = node;
    return node->tex.pixels ? &node->tex : NULL;
}

void texture_cache_clear(void)
{
    TexNode *c = s_cache;
    while (c) {
        TexNode *next = c->next;
        free(c->tex.pixels);
        free(c);
        c = next;
    }
    s_cache = NULL;
}
