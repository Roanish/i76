/*
 * texture.c — mesh face texture loader (see texture.h).
 *
 * Resolution: a face name (lowercased) → if it (or "<name>.tmt") names a Texture
 * Map Table, read entry 0's `.map` base name; otherwise "<name>.map". Then decode
 * the `.map` (8-bit indexed [w][h]+pixels). Results are cached by name in a small
 * linked list — a scene has at most a few dozen distinct textures.
 */

#include "texture.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/vfs.h"   /* vfs_read_file / vfs_exists / vfs_free — extension-aware,
                           * tried FIRST so .tmt/.map don't collide on the 8-char base. */
#include "engine/pix.h"   /* vfs_lod — 8-char-base PIX/PAK resolver; LAST-resort fallback
                           * for pak'd textures (e.g. the WDEF ground tex tt161sd_). */

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

/* Load "<name>.vqm" + its codebook (name @ vqm+8) and decode into t. */
static void load_vqm(const char *vqmname, Texture *t)
{
    size_t vsz = 0;
    void *vqm = vfs_read_file(vqmname, &vsz);
    if (!vqm) return;
    if (vsz >= 0x18) {
        char cbk[24];
        memcpy(cbk, (const char *)vqm + 8, 16);
        cbk[16] = '\0';
        str_lower(cbk);
        if (!strchr(cbk, '.')) strncat(cbk, ".cbk", sizeof cbk - strlen(cbk) - 1);
        size_t csz = 0;
        void *book = vfs_read_file(cbk, &csz);
        if (book) {
            decode_vqm(vqm, vsz, book, csz, t);
            vfs_free(book);
        }
    }
    vfs_free(vqm);
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

/* Resolve a base name (no extension) to a concrete ".map" file name. */
static void resolve_map_name(const char *base, char *out)
{
    char tmt[20];
    snprintf(tmt, sizeof tmt, "%s.tmt", base);
    if (vfs_exists(tmt)) {
        size_t sz = 0;
        void *b = vfs_read_file(tmt, &sz);
        if (b) {
            char m[20];
            tmt_entry0(b, sz, m);
            vfs_free(b);
            if (m[0]) { str_lower(m); strncpy(out, m, 20); out[19] = '\0'; return; }
        }
    }
    snprintf(out, 20, "%s.map", base);
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

    /* Faithful order (mirrors texture_load): "<base>.vqm" (+codebook) first,
     * then the uncompressed "<base>.map" (resolving "<base>.tmt" if present). */
    char vqmname[24];
    snprintf(vqmname, sizeof vqmname, "%s.vqm", base);
    if (vfs_exists(vqmname))
        load_vqm(vqmname, &node->tex);

    if (!node->tex.pixels) {
        char mapname[20];
        resolve_map_name(base, mapname);
        size_t sz = 0;
        void *buf = vfs_read_file(mapname, &sz);
        if (buf) {
            decode_map(buf, sz, &node->tex);
            vfs_free(buf);
        }
    }

    /* Pak fallback: textures named by an 8-char base (e.g. the WDEF ground
     * texture tt161sd_, packed in tt16m.pak) aren't direct VFS files — resolve
     * them through the PIX/PAK index. The .map pixel layout is the same. */
    if (!node->tex.pixels) {
        size_t sz = 0;
        void *buf = vfs_lod(base, &sz);   /* malloc'd; free with free() */
        if (buf) {
            decode_map(buf, sz, &node->tex);
            free(buf);
        }
    }

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
