/*
 * vehicle.c — load a .vdf chassis model from its VGEO part grid (see vehicle.h).
 *
 * Faithful to the reversed loaders (docs/REVERSING.md "VDF vehicle definition"):
 *   vgeo_load (FUN_004b90b0): VGEO payload = [u32 part_count] + 28 groups
 *     (7 damage states x 4 LODs, contiguous) of part_count x 0x64-byte records.
 *   vdf_parts_build (FUN_004b3140): parts are created from group (0,0); record
 *     +0x38 parent name ("WORLD"/"none" = vehicle root, else a sibling) chains
 *     them; the mesh name is the record name + ".geo" with every char & 0x7f
 *     (geo_name_format FUN_004b3060 — high bits may be set in the file).
 * Verified byte-exact on vdstag.vdf (28*31*0x64 == VGEO payload).
 */

#include "engine/vehicle.h"
#include "engine/vfs.h"
#include "engine/meshcache.h"
#include "engine/mods.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

#define REC_SIZE 0x64

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static float rd_f32(const uint8_t *p) {
    uint32_t u = rd_u32(p); float f; memcpy(&f, &u, 4); return f;
}

/* Chunk stream: [4cc tag][u32 len incl. 8-byte header]; EXIT chunks (len 8)
 * separate sections and are walked over like any other. */
static long find_chunk(const uint8_t *d, long off, long end, const char *tag)
{
    while (off + 8 <= end) {
        uint32_t len = rd_u32(d + off + 4);
        if (len < 8 || off + (long)len > end) break;
        if (memcmp(d + off, tag, 4) == 0) return off;
        off += len;
    }
    return -1;
}

/* Record name: 8 chars, each masked & 0x7f (the file sets high bits as flags),
 * NUL-trimmed. "NULL"/"none"/empty mean a mesh-less mount-point record. */
static void read_name8(const uint8_t *p, char out[9])
{
    int n = 0;
    for (; n < 8; n++) {
        char ch = (char)(p[n] & 0x7f);
        if (ch == '\0') break;
        out[n] = ch;
    }
    out[n] = '\0';
}
static int name_is_null(const char *n)
{
    return !n[0] || strcasecmp(n, "NULL") == 0 || strcasecmp(n, "none") == 0;
}

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

typedef struct {
    float pR[9], pt[3];     /* parent-relative (+0x08 col-major / +0x2c)      */
    float oR[9], ot[3];     /* composed part-to-vehicle-root transform        */
    char  name[9];
    char  parent[9];        /* "WORLD" = vehicle root                         */
    int   type;             /* +0x5c part role code                           */
    int   resolved;
} Part;

/* Compose part->root by walking the parent chain (same rule as .sdf SGEO). */
static void resolve_part(Part *parts, int np, int i)
{
    Part *p = &parts[i];
    if (p->resolved) return;
    p->resolved = 1;                                  /* cycle guard */

    int pi = -1;
    if (strcasecmp(p->parent, "WORLD") != 0 && !name_is_null(p->parent))
        for (int j = 0; j < np; j++)
            if (j != i && strcmp(parts[j].name, p->parent) == 0) { pi = j; break; }

    if (pi < 0) {
        memcpy(p->oR, p->pR, sizeof p->oR);
        memcpy(p->ot, p->pt, sizeof p->ot);
        return;
    }
    resolve_part(parts, np, pi);
    Part *par = &parts[pi];
    mat3_mul(par->oR, p->pR, p->oR);
    float rt[3]; mat3_vec(par->oR, p->pt, rt);
    for (int k = 0; k < 3; k++) p->ot[k] = rt[k] + par->ot[k];
}

/* Decode one 0x64-byte part record (shared by VGEO and WGEO). */
static void read_part(const uint8_t *r, Part *p)
{
    memset(p, 0, sizeof *p);
    read_name8(r, p->name);
    read_name8(r + 0x38, p->parent);
    for (int rr = 0; rr < 3; rr++)             /* +0x08 is column-major */
        for (int cc = 0; cc < 3; cc++)
            p->pR[rr*3+cc] = rd_f32(r + 0x08 + 4*(cc*3+rr));
    for (int k = 0; k < 3; k++) p->pt[k] = rd_f32(r + 0x2c + 4*k);
    p->type = (int)rd_u32(r + 0x5c);
}

static int type_to_slot(int type);

/* Resolve a part set's parent chains, then append it to v with every part
 * placed under root (rootR/roott, vehicle frame); NULL root = identity. */
static void append_parts(VehicleModel *v, Part *parts, int np,
                         const float *rootR, const float *roott, const char *src)
{
    for (int i = 0; i < np; i++) resolve_part(parts, np, i);
    int n0 = v->count;
    v->parts     = realloc(v->parts, sizeof(SceneItem) * (size_t)(n0 + np + 1));
    v->part_slot = realloc(v->part_slot, sizeof(int) * (size_t)(n0 + np + 1));
    for (int i = 0; i < np; i++) {
        SceneItem *it = &v->parts[v->count];
        memset(it, 0, sizeof *it);
        if (rootR) {
            mat3_mul(rootR, parts[i].oR, it->R);
            mat3_vec(rootR, parts[i].ot, it->t);
            for (int k = 0; k < 3; k++) it->t[k] += roott[k];
        } else {
            memcpy(it->R, parts[i].oR, sizeof it->R);
            memcpy(it->t, parts[i].ot, sizeof it->t);
        }
        it->type = parts[i].type;
        v->part_slot[v->count] = type_to_slot(parts[i].type);
        snprintf(it->name, sizeof it->name, "%s", parts[i].name);
        v->count++;
        if (!name_is_null(parts[i].name)) {
            const GeoMesh *m = geo_cache_acquire(parts[i].name);  /* -> .geo */
            if (m && m->num_verts == 0) { geo_cache_release((void *)m); m = NULL; }
            it->mesh = m;
            if (m) v->mesh_count++;
            else fprintf(stderr, "[veh] %s: part %s mesh missing\n", src, parts[i].name);
        }
    }
}

/* ----------------------------------------------------------------------- */
/* Skin (.vtf) — in-game VTFC handler FUN_004ba600 (chunk table 0x505508).  */
/* The handler is passed the CHUNK START (tag), so offsets below are        */
/* relative to the "VTFC" tag. Layout (all names 13-byte slots, "null"-     */
/* prefixed = empty, skipped via _strnicmp(slot, "null", 4)):               */
/*   +0x08 vdf name[13], +0x15 colour name[16] (garage list, FUN_004bb030)  */
/*   +0x25 + state*0x152: per damage state V1/V3/V5, four 6-slot groups     */
/*        FT,MD,BK,TP (stride 0x4e), sides FT,BK,RT,LF,TP,UN; bound as      */
/*        "V<n> <GRP> <SIDE>.map" on that group's part slot                 */
/*   +0x15d front-bumper slot -> "V1/V3/V5 FB FT.map" on slot FB            */
/*   +0x16a back-bumper slot  -> "V1/V3/V5 BB BK.map" on slot BB            */
/*   +0x41b 12 debris .map names (FUN_004a6020 — not used yet)             */
/*   +0x4b7 body .map -> "V1 BO DY.map" on EVERY part (FUN_004bafa0)        */
/* ----------------------------------------------------------------------- */

#define VTF_SLOT 13

static int slot_is_null(const uint8_t *p) { return strncasecmp((const char *)p, "null", 4) == 0; }

static void add_bind(VehicleModel *v, int cap, int slot, const char *ph, const uint8_t *real)
{
    if (slot_is_null(real) || v->bind_count >= cap) return;
    VehicleSkinBind *b = &v->binds[v->bind_count++];
    b->part_slot = slot;
    snprintf(b->placeholder, sizeof b->placeholder, "%s", ph);
    memcpy(b->real, real, VTF_SLOT);              /* NUL-terminated within 13 */
    b->real[VTF_SLOT] = '\0';
}

static void vehicle_apply_skin(VehicleModel *v, const char *vtf_name)
{
    size_t sz = 0;
    /* PRESENTATION: the .vtf only maps placeholder face names to textures,
     * so a mod may supply new paint jobs. (.vcf/.vdf/.wdf stay base-only —
     * they define placement, mounts and handling.) */
    uint8_t *d = asset_present_file(vtf_name, &sz);
    if (!d) { fprintf(stderr, "[veh] skin %s not found\n", vtf_name); return; }
    long c = find_chunk(d, 0, (long)sz, "VTFC");
    if (c < 0 || c + 0x4b7 + VTF_SLOT > (long)sz) {
        fprintf(stderr, "[veh] skin %s: no/short VTFC chunk\n", vtf_name);
        vfs_free(d);
        return;
    }
    const uint8_t *k = d + c;
    static const char *grp[4]  = { "FT", "MD", "BK", "TP" };
    static const int   gslot[4] = { VEH_SLOT_FT, VEH_SLOT_MD, VEH_SLOT_BK, VEH_SLOT_TP };
    static const char *side[6] = { "FT", "BK", "RT", "LF", "TP", "UN" };
    static const int   vnum[3] = { 1, 3, 5 };

    int cap = 3 * 4 * 6 + 3 + 3 + 1;
    v->binds = calloc((size_t)cap, sizeof *v->binds);
    v->bind_count = 0;
    char ph[16];
    for (int st = 0; st < 3; st++)
        for (int g = 0; g < 4; g++)
            for (int sd = 0; sd < 6; sd++) {
                snprintf(ph, sizeof ph, "V%d %s %s.map", vnum[st], grp[g], side[sd]);
                add_bind(v, cap, gslot[g], ph,
                         k + 0x25 + st * 0x152 + g * 0x4e + sd * VTF_SLOT);
            }
    for (int st = 0; st < 3; st++) {
        snprintf(ph, sizeof ph, "V%d FB FT.map", vnum[st]);
        add_bind(v, cap, VEH_SLOT_FB, ph, k + 0x15d);
        snprintf(ph, sizeof ph, "V%d BB BK.map", vnum[st]);
        add_bind(v, cap, VEH_SLOT_BB, ph, k + 0x16a);
    }
    add_bind(v, cap, -1, "V1 BO DY.map", k + 0x4b7);
    vfs_free(d);
    fprintf(stderr, "[veh] skin %s: %d bindings\n", vtf_name, v->bind_count);
}

const char *vehicle_face_texture(const VehicleModel *v, int part, const char *face_name)
{
    if (!v || !face_name || part < 0 || part >= v->count) return face_name;
    int slot = v->part_slot ? v->part_slot[part] : -1;
    for (int i = 0; i < v->bind_count; i++) {
        const VehicleSkinBind *b = &v->binds[i];
        if ((b->part_slot < 0 || b->part_slot == slot) &&
            strcasecmp(b->placeholder, face_name) == 0)
            return b->real;
    }
    return face_name;
}

/* vgeo_part_register (FUN_004b8ff0): part type -> model slot. */
static int type_to_slot(int type)
{
    switch (type) {
    case 0x41: return VEH_SLOT_FT;
    case 0x42: return VEH_SLOT_MD;
    case 0x43: return VEH_SLOT_BK;
    case 0x44: return VEH_SLOT_TP;
    case 0x45: return VEH_SLOT_FB;
    case 0x46: return VEH_SLOT_BB;
    default:   return -1;
    }
}

/* Wheels: wheel loader FUN_004b92f0 parses the .wdf (chunk table 0x505548:
 * WDFC 004b9580 / WGEO 004b9690 / EXIT). WGEO payload = [u32 count] + 16
 * groups of count x 0x64 records (part_groups_split 004b3110, 0x10 groups),
 * built as two 8-group sets (4 x 2 grid each, vdf_parts_build 004b3140): set A
 * -> first wheel object, set B -> second. The loader then gives wheel A the
 * WLOC record [axle_first] and wheel B [axle_first+1] (matrix + position).
 * We take each set's group 0 (records [0,count) and [8*count,9*count)), the
 * same rule as VGEO group (0,0). On vdstag: A = WA11RWL3 (right, WLOC x>0),
 * B = WA11LWL3 (left). Record offset (+-0.841,0.328,0) recentres the
 * off-centre tyre mesh on its hub. */
int vehicle_attach_wheels(VehicleModel *v, const char *wdf_name, int axle_first)
{
    if (!v || !wdf_name || axle_first < 0 || axle_first > 4) return 0;
    if (slot_is_null((const uint8_t *)wdf_name)) return 1;

    size_t sz = 0;
    uint8_t *d = vfs_read_file(wdf_name, &sz);
    if (!d) { fprintf(stderr, "[veh] cannot open %s\n", wdf_name); return 0; }
    long wg = find_chunk(d, 0, (long)sz, "WGEO");
    if (wg < 0) {
        fprintf(stderr, "[veh] %s: no WGEO chunk\n", wdf_name);
        vfs_free(d);
        return 0;
    }
    uint32_t wlen  = rd_u32(d + wg + 4);
    uint32_t count = rd_u32(d + wg + 8);
    if (count == 0 || count > 64 || (long)(16u * count) * REC_SIZE != (long)wlen - 12 ||
        wg + (long)wlen > (long)sz) {
        fprintf(stderr, "[veh] %s: bad WGEO grid (count=%u len=%u)\n", wdf_name, count, wlen);
        vfs_free(d);
        return 0;
    }
    const uint8_t *recs = d + wg + 0xc;
    Part *parts = calloc(count, sizeof(Part));
    int before = v->mesh_count;
    for (int w = 0; w < 2; w++) {                       /* set A, set B */
        const uint8_t *grp = recs + (long)w * 8 * count * REC_SIZE;
        for (uint32_t i = 0; i < count; i++) read_part(grp + (long)i * REC_SIZE, &parts[i]);
        int m = axle_first + w;
        append_parts(v, parts, (int)count, v->wloc_R[m], v->wloc_t[m], wdf_name);
    }
    free(parts);
    vfs_free(d);
    fprintf(stderr, "[veh] %s: axle %d wheels -> %d meshed\n",
            wdf_name, axle_first / 2, v->mesh_count - before);
    return 1;
}

VehicleModel *vehicle_load_vcf(const char *vcf_name)
{
    /* VCFC handler (FUN_004b8660, chunk table 0x5052a8) also gets the chunk
     * start: +0x18 vdf name[13], +0x25 vtf name[13]. */
    size_t sz = 0;
    uint8_t *d = vfs_read_file(vcf_name, &sz);
    if (!d) { fprintf(stderr, "[veh] cannot open %s\n", vcf_name); return NULL; }
    long c = find_chunk(d, 0, (long)sz, "VCFC");
    if (c < 0 || c + 0x58 + VTF_SLOT > (long)sz) {
        fprintf(stderr, "[veh] %s: no/short VCFC chunk\n", vcf_name);
        vfs_free(d);
        return NULL;
    }
    char vdf[VTF_SLOT + 1], vtf[VTF_SLOT + 1];
    memcpy(vdf, d + c + 0x18, VTF_SLOT); vdf[VTF_SLOT] = '\0';
    memcpy(vtf, d + c + 0x25, VTF_SLOT); vtf[VTF_SLOT] = '\0';
    /* +0x3e / +0x4b / +0x58: wheel defs for axles 0/1/2 (FUN_004b92f0 with
     * WLOC index 0/2/4). */
    char wdf[3][VTF_SLOT + 1];
    for (int a = 0; a < 3; a++) {
        memcpy(wdf[a], d + c + 0x3e + a * VTF_SLOT, VTF_SLOT);
        wdf[a][VTF_SLOT] = '\0';
    }
    vfs_free(d);
    fprintf(stderr, "[veh] %s -> %s + %s, wheels %s / %s / %s\n",
            vcf_name, vdf, vtf, wdf[0], wdf[1], wdf[2]);
    VehicleModel *v = vehicle_load(vdf, slot_is_null((const uint8_t *)vtf) ? NULL : vtf);
    if (v)
        for (int a = 0; a < 3; a++) vehicle_attach_wheels(v, wdf[a], a * 2);
    return v;
}

VehicleModel *vehicle_load(const char *vdf_name, const char *vtf_name)
{
    size_t sz = 0;
    uint8_t *d = vfs_read_file(vdf_name, &sz);
    if (!d) { fprintf(stderr, "[veh] cannot open %s\n", vdf_name); return NULL; }

    long vgeo = find_chunk(d, 0, (long)sz, "VGEO");
    if (vgeo < 0) {
        fprintf(stderr, "[veh] %s: no VGEO chunk\n", vdf_name);
        vfs_free(d);
        return NULL;
    }
    uint32_t vlen  = rd_u32(d + vgeo + 4);
    uint32_t count = rd_u32(d + vgeo + 8);       /* parts per group */
    long grid = vgeo + 0xc;
    if (count == 0 || count > 256 ||
        (long)(28u * count) * REC_SIZE != (long)vlen - 12) {
        fprintf(stderr, "[veh] %s: bad VGEO grid (count=%u len=%u)\n",
                vdf_name, count, vlen);
        vfs_free(d);
        return NULL;
    }

    /* Group (0,0) = pristine damage state, highest LOD. */
    Part *parts = calloc(count, sizeof(Part));
    int np = 0;
    for (uint32_t i = 0; i < count; i++)
        read_part(d + grid + (long)i * REC_SIZE, &parts[np++]);

    VehicleModel *v = calloc(1, sizeof *v);

    /* WLOC (wloc_load FUN_004b92b0): 6 x 0x38 records copied verbatim to the
     * vehicle model +0x168. Per record: +0x00 present flag, +0x04 3x3,
     * +0x28 position (vehicle frame); the wheel loader FUN_004b92f0 copies
     * +0x04.. into the wheel entity's matrix and +0x28.. into its position.
     * Matrix read column-major like the OBJ/part records ??? (identity on
     * every vdstag record, so the order is unverified). */
    long wl = find_chunk(d, 0, (long)sz, "WLOC");
    if (wl >= 0 && wl + 8 + 6 * 0x38 <= (long)sz) {
        for (int w = 0; w < 6; w++) {
            const uint8_t *r = d + wl + 8 + w * 0x38;
            v->wloc_present[w] = rd_u32(r) != 0;
            for (int rr = 0; rr < 3; rr++)
                for (int cc = 0; cc < 3; cc++)
                    v->wloc_R[w][rr*3+cc] = rd_f32(r + 0x04 + 4*(cc*3+rr));
            for (int k = 0; k < 3; k++) v->wloc_t[w][k] = rd_f32(r + 0x28 + 4*k);
        }
    }
    vfs_free(d);

    append_parts(v, parts, np, NULL, NULL, vdf_name);
    free(parts);

    fprintf(stderr, "[veh] %s: %d parts, %d meshed\n",
            vdf_name, v->count, v->mesh_count);
    if (vtf_name) vehicle_apply_skin(v, vtf_name);
    return v;
}

void vehicle_free(VehicleModel *v)
{
    if (!v) return;
    for (int i = 0; i < v->count; i++)
        if (v->parts[i].mesh) geo_cache_release((void *)v->parts[i].mesh);
    free(v->parts);
    free(v->part_slot);
    free(v->binds);
    free(v);
}
