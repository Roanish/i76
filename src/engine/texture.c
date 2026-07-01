/*
 * texture.c — mesh face texture loader (see texture.h).
 *
 * Resolution (deterministic, extension-driven — mirrors the original
 * texture_load): a face name (lowercased, extension dropped) is resolved via
 * vfs_lod, trying ".tmt" (a name table -> real base), ".vqm" (vector-quantized),
 * then ".map" (8-bit indexed), and last ".m16" (8-bit indices + an embedded
 * 16-bit palette, quantized to the game palette via texture_set_palette).
 * .m16 is the high-colour-mode texture set; in our 8-bit path objects normally
 * resolve via a .vqm/.map sibling, so .m16 is only a fallback. vfs_lod is
 * extension-aware and resolves both pak records and loose files, so object
 * textures packed in a material pak (e.g. AN_1BDT1.vqm in anbunk1m.pak) resolve
 * the same as loose files. Results are cached by name in a small linked list.
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

/* Game palette (256 x RGB) for quantizing .m16's embedded 16-bit palette. */
static uint8_t s_pal[768];
static bool    s_have_pal;

void texture_set_palette(const uint8_t *pal768)
{
    if (!pal768) { s_have_pal = false; return; }
    memcpy(s_pal, pal768, sizeof s_pal);
    s_have_pal = true;
}

static uint8_t nearest_pal_index(int r, int g, int b)
{
    int best = 0, bestd = 1 << 30;
    for (int i = 0; i < 256; i++) {
        int dr = r - s_pal[i*3], dg = g - s_pal[i*3+1], db = b - s_pal[i*3+2];
        int d = dr*dr + dg*dg + db*db;
        if (d < bestd) { bestd = d; best = i; }
    }
    return (uint8_t)best;
}

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

/* Decode a .m16: [u32 w][u32 h|flags] + w*h 8-bit indices + [u32 count] +
 * count*u16 embedded RGB565 palette. Each embedded colour is quantized to the
 * nearest game-palette index, so the result is an ordinary 8-bit indexed
 * texture. Needs the game palette (texture_set_palette); returns false without. */
static bool decode_m16(const uint8_t *buf, size_t sz, Texture *t)
{
    if (!s_have_pal || sz < 12) return false;
    uint32_t w, h;
    memcpy(&w, buf, 4);
    memcpy(&h, buf + 4, 4);
    h &= 0xffff;                         /* low 16 = height; high bits are flags */
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return false;
    size_t npx = (size_t)w * h;
    if (sz < 8 + npx + 4) return false;
    uint32_t pcount;
    memcpy(&pcount, buf + 8 + npx, 4);
    size_t pal_off = 8 + npx + 4;
    if (pcount == 0 || pcount > 4096 || sz < pal_off + (size_t)pcount * 2) return false;

    uint8_t *remap = malloc(pcount);     /* embedded index -> game-palette index */
    if (!remap) return false;
    for (uint32_t i = 0; i < pcount; i++) {
        uint16_t c = (uint16_t)(buf[pal_off + 2*i] | (buf[pal_off + 2*i + 1] << 8));
        remap[i] = nearest_pal_index(((c >> 11) & 0x1f) << 3,   /* R565 */
                                     ((c >> 5)  & 0x3f) << 2,   /* G565 */
                                     ( c        & 0x1f) << 3);  /* B565 */
    }
    t->pixels = malloc(npx);
    if (!t->pixels) { free(remap); return false; }
    for (size_t i = 0; i < npx; i++) {
        uint8_t idx = buf[8 + i];
        t->pixels[i] = idx < pcount ? remap[idx] : 0;
    }
    t->w = (int)w;
    t->h = (int)h;
    free(remap);
    return true;
}

/* Resolve a base name to decoded pixels, deterministically by extension,
 * through the (extension-aware) VFS+pak index — which covers pak records and
 * loose files alike. Faithful priority: a ".tmt" name-table (entry 0 names the
 * real base) -> ".vqm" (vector-quantized) -> ".map" (8-bit indexed) -> ".m16"
 * (8-bit indices + embedded 16-bit palette, quantized to the game palette).
 * .m16 is last: an 8-bit .vqm/.map sibling is preferred when present. */
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
        bool ok = decode_m16(buf, sz, t);
        free(buf);
        if (!ok)
            fprintf(stderr, "[tex] %s.m16: decode failed%s\n", base,
                    s_have_pal ? "" : " (no game palette set)");
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
