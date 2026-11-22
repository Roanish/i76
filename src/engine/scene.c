/*
 * scene.c — build a world scene from a mission file (see scene.h).
 *
 * Chunk format (verified on N01.CBT): [4cc tag][u32 len][payload]; len INCLUDES
 * the 8-byte header; chunks nest (next = off+len, children = off+8). We walk to
 * ODEF, iterate its OBJ records, read each object's name/orientation/position/
 * type, and (for structure objects) expand the named .sdf's SGEO parts.
 */

#include "engine/scene.h"
#include "engine/fs.h"
#include "engine/vfs.h"
#include "engine/meshcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>

/* ----------------------------------------------------------------------- */
/* Little-endian readers                                                     */
/* ----------------------------------------------------------------------- */

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static float rd_f32(const uint8_t *p) {
    uint32_t u = rd_u32(p); float f; memcpy(&f, &u, 4); return f;
}
static int tag_is(const uint8_t *p, const char *t) {
    return p[0] == (uint8_t)t[0] && p[1] == (uint8_t)t[1] &&
           p[2] == (uint8_t)t[2] && p[3] == (uint8_t)t[3];
}

/* Find a direct child chunk with `tag` inside [off, end). Returns child offset
 * (of its 8-byte header) or -1. */
static long find_chunk(const uint8_t *d, long off, long end, const char *tag)
{
    while (off + 8 <= end) {
        uint32_t len = rd_u32(d + off + 4);
        if (len < 8 || off + (long)len > end) break;
        if (tag_is(d + off, tag)) return off;
        off += len;
    }
    return -1;
}

/* ----------------------------------------------------------------------- */
/* 3x3 / transform helpers (row-major)                                       */
/* ----------------------------------------------------------------------- */

static void mat3_mul(const float a[9], const float b[9], float o[9])
{
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            o[r*3+c] = a[r*3+0]*b[0*3+c] + a[r*3+1]*b[1*3+c] + a[r*3+2]*b[2*3+c];
}
static void mat3_vec(const float m[9], const float v[3], float o[3])
{
    for (int r = 0; r < 3; r++)
        o[r] = m[r*3+0]*v[0] + m[r*3+1]*v[1] + m[r*3+2]*v[2];
}

/* ----------------------------------------------------------------------- */
/* Item list                                                                 */
/* ----------------------------------------------------------------------- */

typedef struct { SceneItem *v; int n, cap; } ItemVec;

static SceneItem *vec_push(ItemVec *iv)
{
    if (iv->n == iv->cap) {
        iv->cap = iv->cap ? iv->cap * 2 : 64;
        iv->v = realloc(iv->v, sizeof(SceneItem) * iv->cap);
    }
    SceneItem *it = &iv->v[iv->n++];
    memset(it, 0, sizeof(*it));
    return it;
}

/* ----------------------------------------------------------------------- */
/* .sdf expansion — append a draw item per SGEO part mesh                    */
/* ----------------------------------------------------------------------- */

/* Read a buffer for a loose file via the asset filesystem. */
static uint8_t *read_file(const char *path, long *size_out)
{
    FILE *f = fs_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(sz > 0 ? sz : 1);
    if (fread(b, 1, sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f);
    *size_out = sz;
    return b;
}

/* Read up to 8 name chars (alnum/underscore), trimmed, into out[9]. */
static void read_name8(const uint8_t *p, char out[9])
{
    int n = 0;
    for (; n < 8; n++) {
        unsigned char ch = p[n];
        if (!(isalnum(ch) || ch == '_')) break;
        out[n] = (char)ch;
    }
    out[n] = '\0';
}

typedef struct {
    const GeoMesh *m;
    float pR[9], pt[3];     /* part transform, RELATIVE TO PARENT (+0x08 / +0x2c) */
    float oR[9], ot[3];     /* resolved part-to-object transform (parent chain)   */
    char  name[9];          /* +0x00 part name                                    */
    char  parent[9];        /* +0x38 parent name ("WORLD" = object root)          */
    int   resolved;
} Part;

/* Compose a part's part-to-object transform by walking its parent chain.
 * Parent "WORLD" (case-insensitive) is the object root (identity). The original
 * parents parts via object_alloc (FUN_004b33a0); a child's +0x2c translation is
 * expressed in its PARENT's frame, so we must chain, not place against the root. */
static void resolve_part(Part *parts, int np, int i)
{
    Part *p = &parts[i];
    if (p->resolved) return;
    p->resolved = 1;                                  /* cycle guard */

    int pi = -1;
    if (strcasecmp(p->parent, "WORLD") != 0)
        for (int j = 0; j < np; j++)
            if (j != i && strcmp(parts[j].name, p->parent) == 0) { pi = j; break; }

    if (pi < 0) {                                     /* root: object frame = local */
        memcpy(p->oR, p->pR, sizeof p->oR);
        memcpy(p->ot, p->pt, sizeof p->ot);
        return;
    }
    resolve_part(parts, np, pi);
    Part *par = &parts[pi];
    mat3_mul(par->oR, p->pR, p->oR);                  /* oR = parentOR * partR      */
    float rt[3]; mat3_vec(par->oR, p->pt, rt);        /* parentOR * partT           */
    for (int k = 0; k < 3; k++) p->ot[k] = rt[k] + par->ot[k];
}

/* Try to expand `name` as a structure def. Returns the number of part items
 * appended (0 if there's no usable .sdf — caller then treats it as a marker). */
static int expand_sdf(ItemVec *iv, const char *name,
                      const float objR[9], const float objt[3])
{
    char path[64];
    snprintf(path, sizeof(path), "%s.sdf", name);
    size_t sz = 0;
    uint8_t *d = vfs_read_file(path, &sz);   /* .sdf lives inside the archive */
    if (!d) return 0;

    long sgeo = find_chunk(d, 0, (long)sz, "SGEO");
    if (sgeo < 0) { vfs_free(d); return 0; }

    uint32_t slen  = rd_u32(d + sgeo + 4);
    uint32_t count = rd_u32(d + sgeo + 8);   /* records-per-group (SGEO header) */
    long pstart = sgeo + 0xc;                 /* records begin after the 12B hdr */
    long pend   = sgeo + slen;

    /*
     * SGEO is a fixed 6 groups (3 LOD x 2 variant) of `count` parts, each part a
     * 0x78-byte record (confirmed: SGEO handler FUN_004b3be0 builds 6 group ptrs;
     * model builder FUN_004b33a0 iterates 3x2xcount). The original creates one
     * part-OBJECT per slot from group 0 and reuses it across LOD/variant groups,
     * which only swap the mesh. Each part record carries a PARENT name at +0x38
     * ("WORLD" = object root, else a sibling part); its +0x08 3x3 / +0x2c
     * translation are RELATIVE TO THAT PARENT, and object_alloc links them into a
     * hierarchy. We mirror that: group 0 is the canonical part list, pick the
     * highest-detail mesh per slot across the 6 groups, then compose each part up
     * its parent chain. (Placing every part against the object root — ignoring
     * the chain — was the old bug: deck parts parented to the SPN span were drawn
     * at the root, throwing the span and deck apart.)
     */
    enum { MAX_PARTS = 64 };
    Part parts[MAX_PARTS];
    int  np = 0;

    for (uint32_t i = 0; i < count && np < MAX_PARTS; i++) {
        long r0 = pstart + (long)i * 0x78;
        if (r0 + 0x78 > pend) break;
        const uint8_t *base = d + r0;
        char pname[9]; read_name8(base, pname);
        if (!pname[0]) continue;                       /* NULL/empty slot */

        /* Highest-detail mesh for this slot across all 6 LOD/variant groups. */
        const GeoMesh *best = NULL;
        for (int g = 0; g < 6; g++) {
            long rg = pstart + ((long)g * count + i) * 0x78;
            if (rg + 0x78 > pend) break;
            char gname[9]; read_name8(d + rg, gname);
            if (!gname[0]) continue;
            const GeoMesh *m = (const GeoMesh *)geo_cache_acquire(gname);
            if (!m || m->num_verts == 0) { if (m) geo_cache_release((void *)m); continue; }
            if (!best || m->num_faces > best->num_faces) {
                if (best) geo_cache_release((void *)best);
                best = m;
            } else {
                geo_cache_release((void *)m);
            }
        }
        if (!best) continue;

        Part *p = &parts[np++];
        p->m = best;
        for (int r = 0; r < 3; r++)                    /* +0x08 is column-major */
            for (int c = 0; c < 3; c++)
                p->pR[r*3+c] = rd_f32(base + 0x08 + 4*(c*3+r));
        for (int k = 0; k < 3; k++) p->pt[k] = rd_f32(base + 0x2c + 4*k);
        snprintf(p->name, sizeof p->name, "%s", pname);
        read_name8(base + 0x38, p->parent);
        p->resolved = 0;
    }
    vfs_free(d);

    for (int i = 0; i < np; i++) resolve_part(parts, np, i);

    for (int i = 0; i < np; i++) {
        SceneItem *it = vec_push(iv);
        it->mesh = parts[i].m;
        mat3_mul(objR, parts[i].oR, it->R);            /* world = obj * part-chain  */
        float rt[3]; mat3_vec(objR, parts[i].ot, rt);
        for (int k = 0; k < 3; k++) it->t[k] = rt[k] + objt[k];
        snprintf(it->name, sizeof(it->name), "%s", parts[i].name);
    }
    return np;
}

/* ----------------------------------------------------------------------- */
/* Terrain                                                                   */
/* ----------------------------------------------------------------------- */

/*
 * World scale (derived empirically — see asset_data_model memory): the mission
 * TDEF/ZMAP is a sparse 80x80 coarse grid (0xff = empty); each set cell holds a
 * 128x128 heightfield zone from the .TER file. A coarse cell is 640 world units
 * (tightly bounded by matching N01's patches to the placed objects), so a fine
 * cell is 640/128 = 5 units. .TER zone slab = 0x8000 bytes = 128*128 * 2 (the
 * first byte of each cell is the height; the second is material/surface type).
 */
#define COARSE_CELLS 80
#define COARSE_SIZE  640.0f
#define ZONE_DIM     128
#define ZONE_BYTES   0x8000
#define FINE_CELL    (COARSE_SIZE / ZONE_DIM)   /* 5.0 */

/* Parse the mission's TDEF (ZMAP grid + ZONE filename), load the .TER zone
 * file (sibling of the mission), and build one TerrainPatch per non-empty cell. */
static void load_terrain(Scene *s, const uint8_t *d, long sz, const char *mission_path)
{
    long tdef = find_chunk(d, 0, sz, "TDEF");
    if (tdef < 0) return;
    uint32_t tlen = rd_u32(d + tdef + 4);
    long zmap = find_chunk(d, tdef + 8, tdef + tlen, "ZMAP");
    long zone = find_chunk(d, tdef + 8, tdef + tlen, "ZONE");
    if (zmap < 0 || zone < 0) return;

    const uint8_t *grid = d + zmap + 9;            /* +8 hdr, +1 count byte */
    const char    *zname = (const char *)(d + zone + 9);  /* +8 hdr, +1 flag byte */

    /* .TER path = mission directory + zone filename. */
    char terpath[160];
    const char *slash = strrchr(mission_path, '/');
    if (slash) snprintf(terpath, sizeof terpath, "%.*s/%s",
                        (int)(slash - mission_path), mission_path, zname);
    else       snprintf(terpath, sizeof terpath, "%s", zname);

    long tersz = 0;
    uint8_t *ter = read_file(terpath, &tersz);
    if (!ter) { fprintf(stderr, "[scene] terrain '%s' not found\n", terpath); return; }
    int nzones = (int)(tersz / ZONE_BYTES);

    int cap = 0;
    for (int idx = 0; idx < COARSE_CELLS * COARSE_CELLS; idx++) {
        uint8_t zi = grid[idx];
        if (zi == 0xff || zi >= nzones) continue;       /* empty / out of range */
        if (s->patch_count == cap) {
            cap = cap ? cap * 2 : 8;
            s->patches = realloc(s->patches, sizeof(TerrainPatch) * cap);
        }
        int col = idx % COARSE_CELLS, row = idx / COARSE_CELLS;
        TerrainPatch *p = &s->patches[s->patch_count++];
        p->origin[0] = col * COARSE_SIZE;
        p->origin[1] = row * COARSE_SIZE;
        p->dim  = ZONE_DIM;
        p->cell = FINE_CELL;
        p->zone = zi;
        p->heights = malloc(ZONE_DIM * ZONE_DIM * sizeof(uint16_t));
        p->surface = malloc(ZONE_DIM * ZONE_DIM);
        const uint8_t *slab = ter + (size_t)zi * ZONE_BYTES;
        for (int i = 0; i < ZONE_DIM * ZONE_DIM; i++) {
            /* visible height = low 12 bits of the u16 cell; byte1>>4 = material */
            p->heights[i] = (uint16_t)(((slab[i*2+1] & 0x0F) << 8) | slab[i*2]);
            p->surface[i] = (uint8_t)(slab[i*2+1] >> 5);
        }
    }
    free(ter);
    fprintf(stderr, "[scene] terrain '%s': %d zones, %d patch(es) placed\n",
            terpath, nzones, s->patch_count);
}

/* Read the WDEF->WRLD terrain texture set: the palette (name[0]) and ground
 * texture (name[6]). The 8 asset names are a 13-byte-stride array at WRLD+0x26
 * (see docs/REVERSING.md "World definition (WDEF -> WRLD)"). */
static void load_world_textures(Scene *s, const uint8_t *d, long sz)
{
    s->hour = -1;
    long wdef = find_chunk(d, 0, sz, "WDEF");
    if (wdef < 0) return;
    uint32_t wlen = rd_u32(d + wdef + 4);
    long wrld = find_chunk(d, wdef + 8, wdef + (long)wlen, "WRLD");
    if (wrld < 0) return;

    long names = wrld + 0x26;            /* name[i] at +0x26 + i*13 */
    if (names + 8 * 13 > sz) return;
    const char *n = (const char *)(d + names);
    snprintf(s->palette_name, sizeof s->palette_name, "%.12s", n + 0 * 13);
    snprintf(s->ground_tex,   sizeof s->ground_tex,   "%.12s", n + 6 * 13);
    snprintf(s->lum_name,     sizeof s->lum_name,     "%.12s", n + 1 * 13);
    snprintf(s->sky_tex,      sizeof s->sky_tex,      "%.12s", n + 4 * 13);
    /* +0x9b: hour of day -> sky_light_init (FUN_0046f5d0), see
     * docs/REVERSING.md "Time of day + global lighting". */
    if (wrld + 0x9b + 4 <= sz) s->hour = (int)rd_u32(d + wrld + 0x9b);
    if (wrld + 0x9f + 8 * 20 <= sz) {
        for (int i = 0; i < 8; i++) {
            const uint8_t *e = d + wrld + 0x9f + i * 20;
            s->surf_grip[i]   = rd_f32(e + 0);
            s->surf_roll[i]   = rd_f32(e + 4);
            s->surf_bump[i]   = rd_f32(e + 8);
            s->surf_f3[i]     = rd_f32(e + 12);
            s->surf_effect[i] = (int)rd_u32(e + 16);
        }
        s->have_surfaces = 1;
    }
    fprintf(stderr, "[scene] world: palette '%s', lum '%s', ground '%s', sky '%s', hour %d\n",
            s->palette_name, s->lum_name, s->ground_tex, s->sky_tex, s->hour);
}

/* Parse RDEF -> every RSEG: the road network. RSEG payload = [u32 type]
 * [u32 count][count x 24-byte ribs]; a rib is edgeA (x,y,z) +0x00/04/08 and
 * edgeB (x,y,z) +0x0c/10/14, with Y filled from the terrain at load time
 * (roads_drape_to_terrain FUN_00493cc0; RSEG_loader FUN_004b4370 keeps the
 * type and copies the ribs verbatim). ISEG/TSEG (junction pieces, shared
 * handler 0044dee0) are not decoded yet. See REVERSING.md "RDEF / road segments". */
static void load_roads(Scene *s, const uint8_t *d, long sz)
{
    long rdef = find_chunk(d, 0, sz, "RDEF");
    if (rdef < 0) return;
    long end = rdef + (long)rd_u32(d + rdef + 4);
    if (end > sz) end = sz;

    /* Two passes: count, then fill. */
    int nseg = 0; long nrib = 0;
    for (int pass = 0; pass < 2; pass++) {
        int si = 0; long ri = 0;
        for (long o = rdef + 8; o + 8 <= end; ) {
            uint32_t len = rd_u32(d + o + 4);
            if (len < 8) break;
            if (memcmp(d + o, "RSEG", 4) == 0 && o + 16 <= end) {
                uint32_t type  = rd_u32(d + o + 8);
                uint32_t count = rd_u32(d + o + 12);
                long recs = o + 16;
                if (count > 0 && count <= 200000 && recs + (long)count * 24 <= o + (long)len) {
                    if (pass == 1) {
                        s->road_segs[si] = (RoadSeg){ (int)type, (int)ri, (int)count };
                        for (uint32_t k = 0; k < count; k++) {
                            const uint8_t *r = d + recs + (size_t)k * 24;
                            s->road[ri + k].lx = rd_f32(r + 0x00);   /* edgeA.X */
                            s->road[ri + k].lz = rd_f32(r + 0x08);   /* edgeA.Z */
                            s->road[ri + k].rx = rd_f32(r + 0x0c);   /* edgeB.X */
                            s->road[ri + k].rz = rd_f32(r + 0x14);   /* edgeB.Z */
                        }
                    }
                    si++; ri += count;
                }
            }
            o += len;
        }
        if (pass == 0) {
            nseg = si; nrib = ri;
            if (nseg == 0) return;
            s->road_segs = malloc(sizeof(RoadSeg) * (size_t)nseg);
            s->road      = malloc(sizeof(RoadRib) * (size_t)nrib);
            if (!s->road_segs || !s->road) {
                free(s->road_segs); free(s->road);
                s->road_segs = NULL; s->road = NULL;
                return;
            }
        }
    }
    s->road_seg_count = nseg;
    s->road_count     = (int)nrib;
    fprintf(stderr, "[scene] roads: %d segment(s), %d ribs\n", nseg, s->road_count);
}

/* ----------------------------------------------------------------------- */
/* Mission load                                                              */
/* ----------------------------------------------------------------------- */

Scene *scene_load_mission(const char *path)
{
    long sz = 0;
    uint8_t *d = read_file(path, &sz);
    if (!d) { fprintf(stderr, "[scene] cannot open %s\n", path); return NULL; }

    long odef = find_chunk(d, 0, sz, "ODEF");
    if (odef < 0) { fprintf(stderr, "[scene] no ODEF in %s\n", path); free(d); return NULL; }

    uint32_t odlen = rd_u32(d + odef + 4);
    long ostart = odef + 8;
    long oend   = odef + odlen;

    ItemVec iv = {0};
    int n_obj = 0, n_mesh = 0, n_marker = 0;

    long off = ostart;
    while (off + 8 <= oend) {
        uint32_t len = rd_u32(d + off + 4);
        if (len < 8 || off + (long)len > oend) break;
        if (tag_is(d + off, "OBJ\0") || tag_is(d + off, "OBJ ")) {
            const uint8_t *p = d + off + 8;        /* OBJ payload */
            /* name: up to 8 chars, ends at the first non-[alnum/underscore]
             * byte (the matrix at +0x08 begins right after). */
            char name[9]; int nl = 0;
            for (; nl < 8; nl++) {
                unsigned char ch = p[nl];
                if (!(isalnum(ch) || ch == '_')) break;
                name[nl] = (char)ch;
            }
            name[nl] = '\0';
            float R[9], t[3];
            /* The +0x08 3x3 is stored COLUMN-MAJOR; we keep R row-major, so read
             * transposed. Confirmed via N01's bridge (bewdbrg2 = Ry(+90); read
             * naively it rendered 180 out of phase; identity-oriented objects like
             * the bunker are unaffected either way). */
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++)
                    R[r*3+c] = rd_f32(p + 0x08 + 4*(c*3+r));
            for (int i = 0; i < 3; i++) t[i] = rd_f32(p + 0x2c + 4*i);
            int type = (int)rd_u32(p + 0x5c);
            n_obj++;

            int added = expand_sdf(&iv, name, R, t);
            if (added > 0) { n_mesh++; }
            else {
                SceneItem *it = vec_push(&iv);     /* marker */
                it->mesh = NULL;
                memcpy(it->R, R, sizeof R);
                memcpy(it->t, t, sizeof t);
                it->type = type;
                snprintf(it->name, sizeof(it->name), "%s", name);
                n_marker++;
            }
        }
        off += len;
    }

    Scene *s = calloc(1, sizeof(Scene));
    s->items = iv.v;
    s->count = iv.n;

    load_terrain(s, d, sz, path);
    load_world_textures(s, d, (long)sz);
    load_roads(s, d, (long)sz);
    free(d);

    /* Centroid + radius over item positions. */
    if (iv.n) {
        double c[3] = {0,0,0};
        for (int i = 0; i < iv.n; i++)
            for (int k = 0; k < 3; k++) c[k] += iv.v[i].t[k];
        for (int k = 0; k < 3; k++) s->center[k] = (float)(c[k] / iv.n);
        double rad = 1.0;
        for (int i = 0; i < iv.n; i++) {
            double dd = 0;
            for (int k = 0; k < 3; k++) { double e = iv.v[i].t[k] - s->center[k]; dd += e*e; }
            if (sqrt(dd) > rad) rad = sqrt(dd);
        }
        s->radius = (float)rad;
    } else {
        s->radius = 100.0f;
    }

    /* Make sure the camera frames the terrain patches too. */
    for (int i = 0; i < s->patch_count; i++) {
        const TerrainPatch *p = &s->patches[i];
        float ext = p->dim * p->cell;   /* patch world size (640) */
        float corners[4][2] = {
            { p->origin[0],       p->origin[1]       },
            { p->origin[0] + ext, p->origin[1]       },
            { p->origin[0],       p->origin[1] + ext },
            { p->origin[0] + ext, p->origin[1] + ext },
        };
        for (int c = 0; c < 4; c++) {
            float dx = corners[c][0] - s->center[0];
            float dz = corners[c][1] - s->center[2];
            float dist = sqrtf(dx*dx + dz*dz);
            if (dist > s->radius) s->radius = dist;
        }
    }

    fprintf(stderr, "[scene] %s: %d OBJ -> %d items (%d meshed, %d markers); "
            "center (%.0f,%.0f,%.0f) r=%.0f\n",
            path, n_obj, s->count, n_mesh, n_marker,
            s->center[0], s->center[1], s->center[2], s->radius);
    return s;
}

void scene_free(Scene *s)
{
    if (!s) return;
    for (int i = 0; i < s->count; i++)
        if (s->items[i].mesh) geo_cache_release((void *)s->items[i].mesh);
    for (int i = 0; i < s->patch_count; i++) {
        free(s->patches[i].heights);
        free(s->patches[i].surface);
    }
    free(s->patches);
    free(s->road);
    free(s->road_segs);
    free(s->items);
    free(s);
}
