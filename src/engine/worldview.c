/*
 * worldview.c — solid 3D scene viewer with a free-fly camera (see worldview.h).
 *
 * Builds geometry from the mission Scene (objects, terrain, roads) and renders
 * it through either backend — the software rasterizer (softraster.c) or the
 * Vulkan pass (render_3d_*) — sharing one camera, mirroring the original's
 * software/Glide split. Drive mode runs the vehicle simulation (vehsim.c)
 * with the original's track camera.
 */

#include "worldview.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"
#include "engine/input.h"
#include "engine/font.h"
#include "engine/scene.h"
#include "engine/geomesh.h"
#include "engine/vfs.h"        /* vfs_read_file (palette load) */
#include "engine/vehicle.h"
#include "engine/mods.h"         /* asset_present_file (palette) */
#include "engine/lighting.h"     /* time-of-day global light */
#include "engine/keybind.h"      /* original input.map / gamekey.map bindings */
#include "engine/trackcam.h"     /* original external track camera */
#include "engine/vehsim.h"       /* the original vehicle simulation */
#include "engine/pix.h"          /* vfs_lod (.lum: lighting is not moddable) */
#include "engine/texture.h"
#include "engine/fidelity.h"      /* --legacy: faithful paths */
#include "render/render.h"
#include "render/softraster.h"

#define FB_W 1280
#define FB_H 960
#define TARGET_FRAME_MS (1000.0 / 60.0)
#define TERRAIN_STEP 2     /* build every Nth fine cell into the solid mesh */

/*
 * Faces and terrain carry their own palette indices (decoded .map texels, or a
 * 1x1 flat index for untextured faces), so the viewer no longer lays out shade
 * ramps — it only needs sky/ground fill indices and two HUD-text entries.
 *   IDX_TEXT/DIM : HUD text (present in the mission palette, see load_act_palette)
 */
#define IDX_TEXT  200
#define IDX_DIM   201

/* ----------------------------------------------------------------------- */
/* Small vec helpers                                                         */
/* ----------------------------------------------------------------------- */

static void vsub(const float a[3], const float b[3], float o[3]) { for(int i=0;i<3;i++) o[i]=a[i]-b[i]; }
static float vdot(const float a[3], const float b[3]) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
static void vcross(const float a[3], const float b[3], float o[3]) {
    o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0];
}
static void vnorm(float v[3]) {
    float l = sqrtf(vdot(v,v)); if (l>1e-6f){ v[0]/=l; v[1]/=l; v[2]/=l; }
}
static void mat3_mul(const float a[9], const float b[9], float o[9]) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            o[r*3+c] = a[r*3+0]*b[0*3+c] + a[r*3+1]*b[1*3+c] + a[r*3+2]*b[2*3+c];
}
static void mat3_vec(const float m[9], const float v[3], float o[3]) {
    for (int r = 0; r < 3; r++)
        o[r] = m[r*3+0]*v[0] + m[r*3+1]*v[1] + m[r*3+2]*v[2];
}

/* ----------------------------------------------------------------------- */
/* Matrices (column-major, Vulkan clip space: Y down, depth 0..1)            */
/* ----------------------------------------------------------------------- */

static void mat_perspective(float m[16], float fovy, float aspect,
                            float znear, float zfar)
{
    float f = 1.0f / tanf(0.5f * fovy);
    memset(m, 0, sizeof(float) * 16);
    m[0]  = f / aspect;
    m[5]  = -f;                              /* flip Y for Vulkan        */
    m[10] = zfar / (znear - zfar);
    m[11] = -1.0f;
    m[14] = (zfar * znear) / (znear - zfar);
}

static void mat_lookat_up(float m[16], const float eye[3], const float fwd[3], const float upv[3])
{
    float wup[3] = { upv[0], upv[1], upv[2] };
    float f[3] = { fwd[0], fwd[1], fwd[2] }; vnorm(f);
    float s[3]; vcross(f, wup, s); vnorm(s);
    float u[3]; vcross(s, f, u);

    m[0]=s[0]; m[4]=s[1]; m[8] =s[2];  m[12]=-vdot(s,eye);
    m[1]=u[0]; m[5]=u[1]; m[9] =u[2];  m[13]=-vdot(u,eye);
    m[2]=-f[0];m[6]=-f[1];m[10]=-f[2]; m[14]= vdot(f,eye);
    m[3]=0;    m[7]=0;    m[11]=0;     m[15]=1;
}


/* SceneItem world transform (R row-major 3x3 + t) -> column-major model mat4. */
/* Model matrix drawn CAMERA-RELATIVE (hardware path): translation = t - eye,
 * subtracted in double so the GPU only sees small numbers. Mission
 * coordinates reach ~49000, where a float resolves only ~4 mm; transforming
 * absolute positions on the GPU made coplanar road/terrain fight near the
 * camera. The view matrix is built with the eye at the origin. */
static void rel_model(float m[16], const float R[9], const float t[3], const float eye[3])
{
    static const float I3[9] = { 1,0,0, 0,1,0, 0,0,1 };
    if (!R) R = I3;
    m[0]=R[0]; m[1]=R[3]; m[2] =R[6]; m[3]=0;
    m[4]=R[1]; m[5]=R[4]; m[6] =R[7]; m[7]=0;
    m[8]=R[2]; m[9]=R[5]; m[10]=R[8]; m[11]=0;
    m[12]=(float)((double)t[0] - (double)eye[0]);
    m[13]=(float)((double)t[1] - (double)eye[1]);
    m[14]=(float)((double)t[2] - (double)eye[2]);
    m[15]=1;
}

/* ----------------------------------------------------------------------- */
/* Mesh building — triangulate faces + bake flat normals                     */
/* ----------------------------------------------------------------------- */

/* GPU mesh vertex: interleaved [px,py,pz, u,v, nx,ny,nz] (RENDER_VERT_FLOATS;
 * outward normals for the global light). Terrain uses planar UV in world x/z.
 * The software path reads only pos+uv. */
#define MV RENDER_VERT_FLOATS
#define TERRAIN_TILE 96.0f   /* world units per terrain-texture repeat */
/* Vertical scale: world Y = height12 * 0.1. `hscale` is a debug multiplier on
 * this (1.0 = exact). */
#define TERRAIN_Y_SCALE 0.1f

/* Height (12-bit, unscaled) at a GLOBAL fine-grid point, searching all patches.
 * Adjacent patches are contiguous on the global grid (patch at coarse (row,col)
 * covers points [col*128, col*128+127]), so cross-patch lookups resolve the
 * neighbour's border points, as the original samples one global zone grid.
 * -1 if no patch covers it. */
static float cell_h12(const Scene *s, long gx, long gz)
{
    for (int i = 0; i < s->patch_count; i++) {
        const TerrainPatch *p = &s->patches[i];
        long lx = gx - (long)lroundf(p->origin[0] / p->cell);
        long lz = gz - (long)lroundf(p->origin[1] / p->cell);
        if (lx < 0 || lz < 0 || lx >= p->dim || lz >= p->dim) continue;
        return (float)p->heights[lz * p->dim + lx];
    }
    return -1.0f;
}

static float sample_terrain_y(const Scene *s, float x, float z, float hscale);

/* Outward terrain normal at GLOBAL grid point (gx,gz), exactly as the
 * original's D3D terrain path: central differences of the
 * 12-bit heights one cell either side, scaled 0.1/cell, n = (-dx, 1, -dz)
 * normalised. (Missing neighbours fall back to the centre height; the
 * original reads 0 there, which only matters at the world edge.) */
static void terrain_grid_normal(const Scene *s, long gx, long gz, float hscale, float n[3])
{
    float cell = s->patches[0].cell;
    float h0 = cell_h12(s, gx, gz);
    float hl = cell_h12(s, gx - 1, gz), hr = cell_h12(s, gx + 1, gz);
    float hd = cell_h12(s, gx, gz - 1), hu = cell_h12(s, gx, gz + 1);
    if (hl < 0) hl = h0;
    if (hr < 0) hr = h0;
    if (hd < 0) hd = h0;
    if (hu < 0) hu = h0;
    float k  = TERRAIN_Y_SCALE * hscale / (2.0f * cell);
    float dx = (hr - hl) * k, dz = (hu - hd) * k;
    float l  = sqrtf(dx * dx + dz * dz + 1.0f);
    n[0] = -dx / l; n[1] = 1.0f / l; n[2] = -dz / l;
}

/* Terrain triangle soup for one patch, every `step` fine cells. The GPU path
 * uses step 1 (full resolution, as the original); the software path uses
 * TERRAIN_STEP to stay fast. */
static float *build_terrain_verts(const Scene *s, const TerrainPatch *p,
                                  float hscale, int step, int patch_local,
                                  uint32_t *out_count)
{
    /* patch_local: positions relative to the patch origin (hardware path,
     * drawn camera-relative); otherwise world positions (software path). */
    float ox = patch_local ? p->origin[0] : 0.0f, oz = patch_local ? p->origin[1] : 0.0f;
    int n = p->dim;
    long c0 = lroundf(p->origin[0] / p->cell);
    long r0 = lroundf(p->origin[1] / p->cell);
    int cells = (n + step - 1) / step;
    if (cells < 1) return NULL;

    uint32_t vmax = (uint32_t)cells * cells * 6;
    float *v = malloc((size_t)vmax * MV * sizeof(float));
    if (!v) return NULL;

    /* Quads run to the patch EDGE; far corners on the boundary row/column pull
     * heights from the adjacent patch via the global grid, closing the seam.
     * Where there is no neighbour (world edge) the quad is skipped. */
    float *w = v;
    for (int gz = 0; gz < n; gz += step) {
        for (int gx = 0; gx < n; gx += step) {
            int cx[4] = { gx, gx+step, gx+step, gx }, cz[4] = { gz, gz, gz+step, gz+step };
            float c[4][3];
            long  g[4][2];
            int ok = 1;
            for (int i = 0; i < 4; i++) {
                float h = cell_h12(s, c0 + cx[i], r0 + cz[i]);
                if (h < 0.0f) { ok = 0; break; }
                c[i][0] = p->origin[0] + cx[i] * p->cell;
                c[i][1] = h * (TERRAIN_Y_SCALE * hscale);
                c[i][2] = p->origin[1] + cz[i] * p->cell;
                g[i][0] = c0 + cx[i]; g[i][1] = r0 + cz[i];
            }
            if (!ok) continue;
            int quad[6] = { 0,1,2, 0,2,3 };
            for (int t = 0; t < 6; t++) {
                const float *a = c[quad[t]];
                float nrm[3];
                terrain_grid_normal(s, g[quad[t]][0], g[quad[t]][1], hscale, nrm);
                *w++ = a[0] - ox; *w++ = a[1]; *w++ = a[2] - oz;
                *w++ = a[0] / TERRAIN_TILE; *w++ = a[2] / TERRAIN_TILE;
                *w++ = nrm[0]; *w++ = nrm[1]; *w++ = nrm[2];
            }
        }
    }
    *out_count = (uint32_t)(w - v) / MV;
    return v;
}

/* Terrain world-Y under (x,z): bilinear over the 4 surrounding global grid
 * points, scaled by 0.1 as the original, including across patch boundaries. Missing neighbours fall
 * back to the base corner (the viewer has no default zone slab); 0 if (x,z)
 * is outside all patches. */
static float sample_terrain_y(const Scene *s, float x, float z, float hscale)
{
    if (s->patch_count == 0) return 0.0f;
    float cell = s->patches[0].cell;
    float cxf = x / cell, czf = z / cell;
    long fx = (long)floorf(cxf), fz = (long)floorf(czf);
    float tx = cxf - fx, tz = czf - fz;
    float h00 = cell_h12(s, fx, fz);
    if (h00 < 0.0f) return 0.0f;
    float h10 = cell_h12(s, fx + 1, fz);     if (h10 < 0.0f) h10 = h00;
    float h01 = cell_h12(s, fx, fz + 1);     if (h01 < 0.0f) h01 = h00;
    float h11 = cell_h12(s, fx + 1, fz + 1); if (h11 < 0.0f) h11 = h00;
    float hx0 = h00 + (h10 - h00) * tx;
    float hx1 = h01 + (h11 - h01) * tx;
    return (hx0 + (hx1 - hx0) * tz) * (TERRAIN_Y_SCALE * hscale);
}

/* Ground query for the track camera. */
typedef struct { const Scene *scene; float hscale; } TerrainGroundCtx;
static float terrain_ground_cb(void *ud, float x, float z)
{
    const TerrainGroundCtx *g = ud;
    return sample_terrain_y(g->scene, x, z, g->hscale);
}

/* Surface type (0..7) of the global cell (gx, gz); 5 outside the map. */
static int cell_surface(const Scene *s, long gx, long gz)
{
    for (int i = 0; i < s->patch_count; i++) {
        const TerrainPatch *p = &s->patches[i];
        long lx = gx - (long)lroundf(p->origin[0] / p->cell);
        long lz = gz - (long)lroundf(p->origin[1] / p->cell);
        if (lx < 0 || lz < 0 || lx >= p->dim || lz >= p->dim) continue;
        return p->surface ? p->surface[lz * p->dim + lx] : 0;
    }
    return 5;
}

/* Vehicle ground query: bilinear height, the face normal of the heightfield
 * triangle under (x, z), and the cell's surface type. Terrain only for now
 * (see docs/REVERSING.md "Ground contact query"). */
static float vehsim_ground_cb(void *ud, float x, float y, float z, float n[3], int *surf)
{
    (void)y;
    const TerrainGroundCtx *g = ud;
    const Scene *s = g->scene;
    float h = sample_terrain_y(s, x, z, g->hscale);
    if (s->patch_count == 0) { n[0] = 0; n[1] = 1; n[2] = 0; *surf = 5; return h; }
    float cell = s->patches[0].cell;
    float cx = x / cell, cz = z / cell;
    long fx = (long)floorf(cx), fz = (long)floorf(cz);
    float tx = cx - fx, tz = cz - fz;
    float h00 = cell_h12(s, fx, fz), h10 = cell_h12(s, fx + 1, fz);
    float h01 = cell_h12(s, fx, fz + 1), h11 = cell_h12(s, fx + 1, fz + 1);
    if (h10 < 0) h10 = h00;
    if (h01 < 0) h01 = h00;
    if (h11 < 0) h11 = h00;
    float dX, dZ;
    if (tz <= tx) { dX = h10 - h00; dZ = h11 - h10; }
    else          { dX = h11 - h01; dZ = h01 - h00; }
    float k = (1.0f / cell) * TERRAIN_Y_SCALE * g->hscale;
    dX *= k; dZ *= k;
    float r = 1.0f / sqrtf(dX * dX + dZ * dZ + 1.0f);
    n[0] = -dX * r; n[1] = r; n[2] = -dZ * r;
    *surf = cell_surface(s, fx, fz);
    return h;
}

/* Height on terrain mesh triangle `h` (0: corners 00,10,11; 1: 00,11,01) of
 * the `step`-cell quad (cx,cz), at local (tx,tz) — the triangle's own plane,
 * so a point on a shared edge gets the same height from either side. */
static float terrain_tri_y(const Scene *s, long cx, long cz, int h, float tx, float tz,
                           float hscale, int step)
{
    long gx = cx * step, gz = cz * step;
    float h00 = cell_h12(s, gx, gz);
    if (h00 < 0.0f) return 0.0f;
    float h10 = cell_h12(s, gx + step, gz);        if (h10 < 0.0f) h10 = h00;
    float h01 = cell_h12(s, gx, gz + step);        if (h01 < 0.0f) h01 = h00;
    float h11 = cell_h12(s, gx + step, gz + step); if (h11 < 0.0f) h11 = h00;
    float y = (h == 0) ? h00 + tx * (h10 - h00) + tz * (h11 - h10)
                       : h00 + tz * (h01 - h00) + tx * (h11 - h01);
    return y * (TERRAIN_Y_SCALE * hscale);
}

/* ----------------------------------------------------------------------- */
/* Roads — the original's road polygons (docs/REVERSING.md "RDEF / road      */
/* segments", "Frame timer + road height").                                 */
/* ----------------------------------------------------------------------- */

/* Default road textures; a mission's "<mission>.rtm" replaces them line by
 * line. */
static const char *const ROAD_TEX_DEFAULT[30] = {
    "r2anr_11.map","r2anr_21.map","r2anr_31.map","r2anr_41.map","r2anr_51.map",
    "r2anr_61.map","r2anr_71.map","r2anr_81.map","r2anr_91.map","r2anr_01.map",
    "r2ayr_11.map","r2ayr_21.map","r2ayr_31.map","r2ayr_41.map","r2ayr_51.map",
    "r2ayr_61.map","r2ayr_71.map","r2ayr_81.map","r2ayr_91.map","r2ayr_01.map",
    "r2dnr_17.map","r2dnr_27.map","r2dnr_37.map","r2dnr_47.map","r2dnr_57.map",
    "r2wnr_19.map","r2wnr_29.map","r2wnr_39.map","r2wnr_49.map","r2wnr_59.map",
};
/* Texture index per quad (3 sets x 20): set = road type % 3,
 * entry = (quad-in-block % 10) + (quad nearer than ROAD_NEAR_Z ? 0 : 10). */
static const int ROAD_TEX_SET[3][20] = {
    { 0,10, 1,11, 2,12, 3,13, 4,14, 5,15, 6,16, 7,17, 8,18, 9,19 },
    {20,21,22,23,24,20,21,22,23,24,20,21,22,23,24,20,21,22,23,24 },
    {25,26,27,28,29,25,26,27,28,29,25,26,27,28,29,25,26,27,28,29 },
};
#define ROAD_NEAR_Z   60.0f    /* camera depth of the quad centre                 */
#define ROAD_FAR_Z   450.0f    /* quads beyond are not drawn (legacy)             */
#define ROAD_SW_LIFT  0.05f    /* software path only (no depth bias there)        */

typedef struct {
    float   *sw;  uint32_t sw_vc;    /* on the software path's step-2 terrain  */
    float   *hw;  uint32_t hw_vc;    /* on the full-resolution terrain         */
    RenderMesh *gpu;
    int      tex[2];                 /* texture index: [0] near, [1] far       */
    float    centre[3];              /* (edgeB_i + edgeA_i+1)/2, as the original */
    float    org[2];                 /* hw verts are relative to (org[0], 0, org[1]) */
} RoadQuad;

/* Clip convex polygon `in` (XZ in [0],[2]) to the half-plane left of a->b
 * (for a counter-clockwise clip triangle, in XZ). Points carry 5 floats:
 * x, -, z, u, v. Returns the vertex count of `out`. */
static int clip_half(const float (*in)[5], int n, const float *a, const float *b,
                     float sign, float (*out)[5])
{
    int m = 0;
    for (int k = 0; k < n; k++) {
        const float *p = in[k], *q = in[(k + 1) % n];
        float dp = sign * ((b[0]-a[0])*(p[2]-a[2]) - (b[2]-a[2])*(p[0]-a[0]));
        float dq = sign * ((b[0]-a[0])*(q[2]-a[2]) - (b[2]-a[2])*(q[0]-a[0]));
        if (dp >= 0) memcpy(out[m++], p, sizeof(float) * 5);
        if ((dp >= 0) != (dq >= 0)) {
            float t = dp / (dp - dq);
            for (int j = 0; j < 5; j++) out[m][j] = p[j] + (q[j] - p[j]) * t;
            m++;
        }
    }
    return m;
}

/* Append triangle (x,z,u,v corners) clipped exactly to the terrain mesh
 * triangles (`step`-cell quads split on the (0,0)-(1,1) diagonal, as
 * build_terrain_verts). Every output triangle lies in a terrain triangle's
 * plane, so the road is coplanar with the ground it covers. */
static void road_tri_clip(const Scene *s, float hscale, int step, float lift,
                          const float org[2], const float tri[3][5],
                          float **buf, size_t *used, size_t *cap)
{
    float q = s->patches[0].cell * (float)step;
    float x0 = fminf(tri[0][0], fminf(tri[1][0], tri[2][0]));
    float x1 = fmaxf(tri[0][0], fmaxf(tri[1][0], tri[2][0]));
    float z0 = fminf(tri[0][2], fminf(tri[1][2], tri[2][2]));
    float z1 = fmaxf(tri[0][2], fmaxf(tri[1][2], tri[2][2]));
    float area = (tri[1][0]-tri[0][0])*(tri[2][2]-tri[0][2]) - (tri[1][2]-tri[0][2])*(tri[2][0]-tri[0][0]);
    if (fabsf(area) < 1e-6f) return;
    for (long cz = (long)floorf(z0 / q); cz * q < z1; cz++)
        for (long cx = (long)floorf(x0 / q); cx * q < x1; cx++) {
            float c00[3] = { cx*q, 0, cz*q },     c10[3] = { (cx+1)*q, 0, cz*q };
            float c11[3] = { (cx+1)*q, 0, (cz+1)*q }, c01[3] = { cx*q, 0, (cz+1)*q };
            const float *tt[2][3] = { { c00, c10, c11 }, { c00, c11, c01 } };
            for (int h = 0; h < 2; h++) {
                float A[12][5], B[12][5];
                int n = 3;
                memcpy(A, tri, sizeof(float) * 15);
                const float *const *t = tt[h];
                float tsign = ((t[1][0]-t[0][0])*(t[2][2]-t[0][2]) - (t[1][2]-t[0][2])*(t[2][0]-t[0][0])) > 0 ? 1.0f : -1.0f;
                n = clip_half((const float (*)[5])A, n, t[0], t[1], tsign, B); if (n < 3) continue;
                n = clip_half((const float (*)[5])B, n, t[1], t[2], tsign, A); if (n < 3) continue;
                n = clip_half((const float (*)[5])A, n, t[2], t[0], tsign, B); if (n < 3) continue;
                size_t need = *used + (size_t)(n - 2) * 3 * MV;
                if (need > *cap) {
                    size_t nc = *cap ? *cap * 2 : 4096;
                    while (nc < need) nc *= 2;
                    float *nb = realloc(*buf, nc * sizeof(float));
                    if (!nb) return;
                    *buf = nb; *cap = nc;
                }
                for (int k = 1; k + 1 < n; k++) {
                    const float *v3[3] = { B[0], B[k], B[k+1] };
                    for (int j = 0; j < 3; j++) {
                        float *w = *buf + *used;
                        float px = v3[j][0], pz = v3[j][2];
                        w[0] = px - org[0];
                        w[1] = terrain_tri_y(s, cx, cz, h, px / q - (float)cx, pz / q - (float)cz,
                                             hscale, step) + lift;
                        w[2] = pz - org[1];
                        w[3] = v3[j][3]; w[4] = v3[j][4];
                        w[5] = 0.0f; w[6] = 1.0f; w[7] = 0.0f;   /* lit as flat-up (L.y) */
                        *used += MV;
                    }
                }
            }
        }
}

/* Split one road (RSEG) into the original's blocks:
 * `per` = quad count halved (rounding up) until <= 24; full blocks of `per`
 * quads while more than 4 quads remain after them, then one last block with
 * the rest (only if it has more than 1 quad). Calls fn(first_rib, quads). */
typedef void (*RoadBlockFn)(void *ud, int first, int quads);
static void road_blocks(int ribs, RoadBlockFn fn, void *ud)
{
    int quads = ribs - 1;
    if (quads < 1) return;
    int per = quads;
    while (per > 24) per = (per & 1) + (per >> 1);
    int rem = quads - per, done = 0;
    if (rem > 4) {
        int left = rem;
        do { fn(ud, done, per); done += per; left -= per; } while (left > 4);
    }
    int last = per + (rem - done);
    if (last > 1) fn(ud, done, last);
}

typedef struct {
    const Scene *s; float hscale; int type, base;
    RoadQuad *q; int n, cap;
} RoadBuild;

static void road_block_cb(void *ud, int first, int quads)
{
    RoadBuild *b = ud;
    for (int k = 0; k < quads; k++) {
        if (b->n == b->cap) {
            int nc = b->cap ? b->cap * 2 : 256;
            RoadQuad *nq = realloc(b->q, sizeof(RoadQuad) * (size_t)nc);
            if (!nq) return;
            b->q = nq; b->cap = nc;
        }
        const RoadRib *r0 = &b->s->road[b->base + first + k];
        const RoadRib *r1 = r0 + 1;
        /* Polygon corners in the original's order ({1,0,2,3} over the rib
         * verts A0,B0,A1,B1) with its UVs:
         * B_i (0,0), A_i (1,0), A_i+1 (1,1), B_i+1 (0,1): one texture repeat
         * per quad, u across, v along. */
        float P[4][5] = {
            { r0->rx, 0, r0->rz, 0, 0 }, { r0->lx, 0, r0->lz, 1, 0 },
            { r1->lx, 0, r1->lz, 1, 1 }, { r1->rx, 0, r1->rz, 0, 1 },
        };
        RoadQuad *q = &b->q[b->n++];
        memset(q, 0, sizeof *q);
        int set = ((b->type % 3) + 3) % 3, e = k % 10;
        q->tex[0] = ROAD_TEX_SET[set][e];
        q->tex[1] = ROAD_TEX_SET[set][e + 10];
        q->centre[0] = (r0->rx + r1->lx) * 0.5f;
        q->centre[2] = (r0->rz + r1->lz) * 0.5f;
        q->centre[1] = (sample_terrain_y(b->s, r0->rx, r0->rz, b->hscale) +
                        sample_terrain_y(b->s, r1->lx, r1->lz, b->hscale)) * 0.5f;
        /* Split the 4-vertex polygon as a fan from its first vertex (B_i). */
        float t0[3][5], t1[3][5];
        memcpy(t0[0], P[0], sizeof P[0]); memcpy(t0[1], P[1], sizeof P[0]); memcpy(t0[2], P[2], sizeof P[0]);
        memcpy(t1[0], P[0], sizeof P[0]); memcpy(t1[1], P[2], sizeof P[0]); memcpy(t1[2], P[3], sizeof P[0]);
        /* Hardware verts relative to a nearby origin (drawn camera-relative:
         * world coordinates here are ~49000, where floats resolve only ~4 mm). */
        q->org[0] = floorf(q->centre[0]); q->org[1] = floorf(q->centre[2]);
        static const float zero[2] = { 0, 0 };
        size_t used = 0, cap = 0; float *buf = NULL;
        road_tri_clip(b->s, b->hscale, 1, 0.0f, q->org, (const float (*)[5])t0, &buf, &used, &cap);
        road_tri_clip(b->s, b->hscale, 1, 0.0f, q->org, (const float (*)[5])t1, &buf, &used, &cap);
        q->hw = buf; q->hw_vc = (uint32_t)(used / MV);
        used = 0; cap = 0; buf = NULL;
        road_tri_clip(b->s, b->hscale, TERRAIN_STEP, ROAD_SW_LIFT, zero, (const float (*)[5])t0, &buf, &used, &cap);
        road_tri_clip(b->s, b->hscale, TERRAIN_STEP, ROAD_SW_LIFT, zero, (const float (*)[5])t1, &buf, &used, &cap);
        q->sw = buf; q->sw_vc = (uint32_t)(used / MV);
    }
}

static RoadQuad *build_road_quads(const Scene *s, float hscale, int *out_n)
{
    RoadBuild b = { s, hscale, 0, 0, NULL, 0, 0 };
    *out_n = 0;
    if (s->patch_count == 0) return NULL;
    for (int i = 0; i < s->road_seg_count; i++) {
        b.type = s->road_segs[i].type;
        b.base = s->road_segs[i].first;
        road_blocks(s->road_segs[i].count, road_block_cb, &b);
    }
    for (int i = 0; i < b.n; i++)
        b.q[i].gpu = b.q[i].hw_vc ? render_mesh_create(b.q[i].hw, b.q[i].hw_vc) : NULL;
    *out_n = b.n;
    return b.q;
}

static void free_road_quads(RoadQuad *q, int n)
{
    for (int i = 0; i < n; i++) {
        free(q[i].sw); free(q[i].hw);
        if (q[i].gpu) render_mesh_destroy(q[i].gpu);
    }
    free(q);
}

/* Per frame, as the original's road draw: the quad centre's
 * camera depth picks the near (< 60) or far texture. The original also skips
 * quads deeper than 450; that cut is kept for --legacy only, because our view
 * distance is longer than the original's (docs/DEVIATIONS.md). Returns the
 * texture index, or -1 to skip the quad. */
static int road_quad_tex(const RoadQuad *q, const float eye[3], const float fwd[3])
{
    float z = (q->centre[0] - eye[0]) * fwd[0] + (q->centre[1] - eye[1]) * fwd[1] +
              (q->centre[2] - eye[2]) * fwd[2];
    if (fidelity_legacy() && z > ROAD_FAR_Z) return -1;
    return q->tex[z < ROAD_NEAR_Z ? 0 : 1];
}

/* Road texture names: defaults, then "<mission>.rtm" line by line (each name
 * = the line's first 15 non-space chars; up to 30 lines). */
static void road_texture_names(const char *mission_path, char names[30][16])
{
    for (int i = 0; i < 30; i++) snprintf(names[i], 16, "%s", ROAD_TEX_DEFAULT[i]);
    const char *b = strrchr(mission_path, '/');
    b = b ? b + 1 : mission_path;
    char rtm[64];
    snprintf(rtm, sizeof rtm, "%s", b);
    char *dot = strrchr(rtm, '.');
    if (dot) *dot = 0;
    strncat(rtm, ".rtm", sizeof rtm - strlen(rtm) - 1);
    size_t sz = 0;
    uint8_t *d = vfs_read_file(rtm, &sz);
    if (!d) return;
    size_t o = 0;
    for (int i = 0; i < 30 && o < sz; i++) {
        int k = 0;
        while (o < sz && d[o] != '\n' && d[o] != '\r' && d[o] != ' ' && d[o] != '\t' && k < 15)
            names[i][k++] = (char)d[o++];
        names[i][k] = 0;
        while (o < sz && d[o] != '\n') o++;
        if (o < sz) o++;
    }
    vfs_free(d);
    fprintf(stdout, "[worldview] road textures from %s\n", rtm);
}

/* ----------------------------------------------------------------------- */
/* Drive-mode car — arcade physics + terrain collision (test harness)        */
/* ----------------------------------------------------------------------- */

/*
 * Feel constants. These are HARNESS values (units = world units ≈ metres,
 * matching the Stag's 5-unit length), NOT reversed from the original — the
 * real per-frame vehicle sim (compnent.cdf ENGN/BRAK/SUSP tables) is future
 * work. Gravity 9.8 matches the engine's own ballistic constant (flamer).
 */
#define CAR_ACCEL      12.0f   /* m/s^2 throttle                            */
#define CAR_ACCEL_BOOST 22.0f  /* m/s^2 with Shift                          */
#define CAR_BRAKE      25.0f   /* m/s^2 braking (S while moving forward)    */
#define CAR_REV_ACCEL   8.0f   /* m/s^2 reversing                           */
#define CAR_DRAG        0.25f  /* /s   lumped aero+rolling (vmax=accel/drag)*/
#define CAR_SPEED_MAX  80.0f
#define CAR_SPEED_MIN -12.0f
#define CAR_TURN_RADIUS 5.0f   /* m    low-speed steering-lock radius       */
#define CAR_GRIP_LAT    8.0f   /* m/s^2 lateral grip cap (yawrate = g/v)    */
#define CAR_GRAVITY     9.8f
#define CAR_WALL_STEP   1.2f   /* m    ground rise in one step = a wall     */
#define CAR_WHEELBASE   3.2f   /* Stag: FWLL z 2.24 - BWLL z -0.95          */
#define CAR_HALFTRACK   0.9f
#define CAR_LEAN_RATE  10.0f   /* /s   orientation smoothing toward terrain */

typedef struct {
    float x, y, z;
    float yaw;          /* heading; model +Z (BDYF nose) faces along it */
    float speed;        /* signed, along heading                        */
    float vy;           /* vertical velocity (airborne)                 */
    int   grounded;
    float R[9];         /* orientation (row-major), smoothed            */
} DriveCar;

static void car_init(DriveCar *c, const Scene *s, float hscale)
{
    memset(c, 0, sizeof *c);
    c->x = s->center[0];
    c->z = s->center[2];
    c->y = sample_terrain_y(s, c->x, c->z, hscale);
    c->grounded = 1;
    c->R[0] = 0; c->R[1] = 0; c->R[2] = 1;
    c->R[3] = 0; c->R[4] = 1; c->R[5] = 0;
    c->R[6] = -1; c->R[7] = 0; c->R[8] = 0;
}

/* Drive input: the bound actions (input.map throttle_up/down, steer_left/
 * right — keypad arrows by default; the grey arrows belong to the camera),
 * plus WASD as a dev convenience. */
typedef struct {
    int throttle_up, throttle_down, steer_left, steer_right;
    int throttle, steer, ebrake, start_engine, shift_up, shift_down, reverse;
} DriveActions;


/* ----------------------------------------------------------------------- */
/* Palettes                                                                  */
/* ----------------------------------------------------------------------- */

/* Nearest palette index to an RGB triple (for sky / ground / untextured faces). */
static uint8_t nearest_index(const Rgb8 *pal, int r, int g, int b)
{
    int best = 0;
    long bestd = (long)1 << 60;
    for (int i = 0; i < 256; i++) {
        long dr = pal[i].r - r, dg = pal[i].g - g, db = pal[i].b - b;
        long d = dr*dr + dg*dg + db*db;
        if (d < bestd) { bestd = d; best = i; }
    }
    return (uint8_t)best;
}

/* Load the terrain palette named in the mission's WDEF (e.g. "t15.act", 256x3
 * RGB). Falls back to the shared Nitro palette, then grayscale. The software
 * texel indices and the framebuffer share this palette. */
static void load_act_palette(const char *act_name, Rgb8 *pal)
{
    char actname[28];
    size_t sz = 0;
    void *buf = NULL;
    if (act_name && act_name[0]) {
        snprintf(actname, sizeof actname, "%s", act_name);
        buf = asset_present_file(actname, &sz);
    }
    if (!buf || sz < 768) {
        vfs_free(buf); buf = NULL; sz = 0;
        strncpy(actname, "superpal.act", sizeof actname);
        buf = asset_present_file(actname, &sz);
    }
    if (buf && sz >= 768) {
        const uint8_t *p = buf;
        for (int i = 0; i < 256; i++)
            pal[i] = (Rgb8){ p[i*3], p[i*3+1], p[i*3+2] };
        fprintf(stdout, "[worldview] palette %s loaded\n", actname);
    } else {
        for (int i = 0; i < 256; i++) pal[i] = (Rgb8){ (uint8_t)i, (uint8_t)i, (uint8_t)i };
        fprintf(stdout, "[worldview] no .act (wanted '%s') — grayscale fallback\n",
                act_name ? act_name : "");
    }
    free(buf);
    /* HUD text indices (overwrites 2 palette entries — acceptable for a viewer). */
    pal[IDX_TEXT] = (Rgb8){ 235, 235, 235 };
    pal[IDX_DIM]  = (Rgb8){ 150, 150, 160 };
}

/* Sky/ground fill indices, resolved against the loaded palette at startup. */
static uint8_t s_sky_idx, s_terrain_idx;
/* A present ground texture (planar-mapped onto our synthetic terrain) + a debug
 * checker used where a face's texture is absent from this archive — both let us
 * verify real texel sampling + UV mapping without the missing object textures. */
static const Texture *s_terrain_tex;
static Texture        s_checker;

static void build_checker(const Rgb8 *pal)
{
    int W = 64;
    s_checker.w = W; s_checker.h = W;
    s_checker.pixels = malloc((size_t)W * W);
    if (!s_checker.pixels) return;
    uint8_t a = nearest_index(pal, 205, 70, 205);   /* magenta */
    uint8_t b = nearest_index(pal,  25, 25,  25);   /* near-black */
    for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++)
            s_checker.pixels[y * W + x] = ((x / 8 + y / 8) & 1) ? a : b;
}

/* Face texture remap: a vehicle part's face names are skin placeholders
 * ("V1 FT RT.MAP") resolved through the .vtf bindings. NULL = use the face
 * name as-is (scene objects). */
typedef struct { const VehicleModel *veh; int part; } TexRemap;

static const Texture *face_texture(const GeoMesh *m, int f, const TexRemap *rm)
{
    const char *name = &m->face_tex[f * 16];
    if (rm) name = vehicle_face_texture(rm->veh, rm->part, name);
    return texture_get(name);
}

/* Software: draw one scene item's mesh, textured per face (untextured faces use
 * the nearest palette index to the face colour bytes). */
static void draw_item_sw(SoftTarget *t, const SceneItem *it, const Rgb8 *pal,
                         const TexRemap *rm)
{
    const GeoMesh *m = it->mesh;
    if (!m) return;
    for (int f = 0; f < m->num_faces; f++) {
        const Texture *tex = face_texture(m, f, rm);
        uint8_t flat = 0;
        if (!tex) {
            /* Texture absent from this archive: show the UV-mapped debug checker
             * so the mapping is still verifiable (a real texel needs the missing
             * .vqm/.map). Falls back to a flat face colour if even that failed. */
            if (s_checker.pixels) {
                tex = &s_checker;
            } else {
                const uint8_t *fc = &m->face_color[f * 3];
                flat = nearest_index(pal, fc[0], fc[1], fc[2]);
            }
        }
        int first = m->face_first[f], nfv = m->face_count[f];
        for (int k = 1; k + 1 < nfv; k++) {
            int e[3] = { first, first + k, first + k + 1 };
            float wp[3][3], uvv[3][2];
            for (int j = 0; j < 3; j++) {
                const float *v = &m->verts[m->indices[e[j]] * 3];
                for (int r = 0; r < 3; r++)
                    wp[j][r] = it->R[r*3+0]*v[0] + it->R[r*3+1]*v[1] + it->R[r*3+2]*v[2] + it->t[r];
                uvv[j][0] = m->uv[e[j]*2];
                uvv[j][1] = m->uv[e[j]*2 + 1];
            }
            soft_tri(t, wp, uvv, tex ? tex->pixels : NULL,
                     tex ? tex->w : 0, tex ? tex->h : 0, flat);
        }
    }
}

/* Software: draw a terrain patch's world-space triangle soup, textured with a
 * planar UV (world x,z) if `tex` is present, else flat-filled with `flat`. */
static void draw_terrain_sw(SoftTarget *t, const float *verts, uint32_t vc,
                            const Texture *tex, uint8_t flat)
{
    for (uint32_t i = 0; i + 3 <= vc; i += 3) {
        float wp[3][3], uv[3][2];
        for (int j = 0; j < 3; j++) {
            const float *p = &verts[(i + j) * MV];   /* pos3 + uv2, stride MV */
            wp[j][0] = p[0]; wp[j][1] = p[1]; wp[j][2] = p[2];
            uv[j][0] = p[3]; uv[j][1] = p[4];
        }
        soft_tri(t, wp, uv, tex ? tex->pixels : NULL,
                 tex ? tex->w : 0, tex ? tex->h : 0, flat);
    }
}

/* ----------------------------------------------------------------------- */
/* GPU texture cache (hardware path) — dedup each source texture so it uploads */
/* once. A source is either a decoded Texture* or a flat palette index (1x1).  */
/* ----------------------------------------------------------------------- */

typedef struct { const Texture *tex; int flat; RenderTexture *gpu; } GpuTexEntry;
static GpuTexEntry s_gtex[1024];
static int         s_gtex_count;

static RenderTexture *gpu_tex_for(const Texture *t)
{
    for (int i = 0; i < s_gtex_count; i++)
        if (s_gtex[i].flat < 0 && s_gtex[i].tex == t) return s_gtex[i].gpu;
    RenderTexture *g = render_texture_create(t->pixels, t->w, t->h);
    if (s_gtex_count < (int)(sizeof s_gtex / sizeof s_gtex[0]))
        s_gtex[s_gtex_count++] = (GpuTexEntry){ t, -1, g };
    return g;
}

static RenderTexture *gpu_tex_for_flat(uint8_t idx)
{
    for (int i = 0; i < s_gtex_count; i++)
        if (s_gtex[i].flat == (int)idx) return s_gtex[i].gpu;
    RenderTexture *g = render_texture_create(&idx, 1, 1);
    if (s_gtex_count < (int)(sizeof s_gtex / sizeof s_gtex[0]))
        s_gtex[s_gtex_count++] = (GpuTexEntry){ NULL, (int)idx, g };
    return g;
}

/* Resolve a face to its GPU texture, exactly mirroring draw_item_sw: the decoded
 * texture, else the UV debug checker, else a 1x1 of the face's flat colour. */
static RenderTexture *resolve_face_gpu_tex(const GeoMesh *m, int f, const Rgb8 *pal,
                                           const TexRemap *rm)
{
    const Texture *t = face_texture(m, f, rm);
    if (t) return gpu_tex_for(t);
    if (s_checker.pixels) return gpu_tex_for(&s_checker);
    const uint8_t *fc = &m->face_color[f * 3];
    return gpu_tex_for_flat(nearest_index(pal, fc[0], fc[1], fc[2]));
}

/* ----------------------------------------------------------------------- */
/* GPU mesh cache — dedup by GeoMesh pointer; each mesh becomes one or more   */
/* batches (a vertex buffer + a texture), grouping faces by resolved texture. */
/* ----------------------------------------------------------------------- */

typedef struct { RenderMesh *gpu; const RenderTexture *tex; } Batch;

typedef struct {
    const GeoMesh *key;
    TexRemap       remap;      /* part of the key: same mesh, different skin */
    int            has_remap;
    Batch         *batches;
    int            batch_count;
} MeshEntry;

/* Build the per-texture GPU batches for one mesh. */
static void build_mesh_batches(MeshEntry *e, const GeoMesh *m, const Rgb8 *pal,
                               const TexRemap *rm)
{
    e->batches = NULL;
    e->batch_count = 0;
    if (m->num_faces == 0) return;

    const RenderTexture **ftex = malloc(sizeof(*ftex) * m->num_faces);
    for (int f = 0; f < m->num_faces; f++)
        ftex[f] = resolve_face_gpu_tex(m, f, pal, rm);

    /* Distinct textures used by this mesh. */
    const RenderTexture **distinct = malloc(sizeof(*distinct) * m->num_faces);
    int nd = 0;
    for (int f = 0; f < m->num_faces; f++) {
        int seen = 0;
        for (int i = 0; i < nd; i++) if (distinct[i] == ftex[f]) { seen = 1; break; }
        if (!seen) distinct[nd++] = ftex[f];
    }

    e->batches = calloc((size_t)(nd ? nd : 1), sizeof(Batch));
    for (int g = 0; g < nd; g++) {
        const RenderTexture *gt = distinct[g];
        long tris = 0;
        for (int f = 0; f < m->num_faces; f++)
            if (ftex[f] == gt && m->face_count[f] >= 3) tris += m->face_count[f] - 2;
        if (tris == 0) continue;

        uint32_t vcount = (uint32_t)tris * 3;
        float *v = malloc((size_t)vcount * MV * sizeof(float));
        float *w = v;
        for (int f = 0; f < m->num_faces; f++) {
            if (ftex[f] != gt) continue;
            int n = m->face_count[f], first = m->face_first[f];
            if (n < 3) continue;
            for (int k = 1; k + 1 < n; k++) {
                int ei[3] = { first, first + k, first + k + 1 };
                for (int j = 0; j < 3; j++) {
                    const float *p  = &m->verts[m->indices[ei[j]] * 3];
                    const float *nv = &m->normals[m->indices[ei[j]] * 3];
                    *w++ = p[0]; *w++ = p[1]; *w++ = p[2];
                    *w++ = m->uv[ei[j] * 2]; *w++ = m->uv[ei[j] * 2 + 1];
                    *w++ = nv[0]; *w++ = nv[1]; *w++ = nv[2];
                }
            }
        }
        Batch *b = &e->batches[e->batch_count++];
        b->tex = gt;
        b->gpu = render_mesh_create(v, vcount);
        free(v);
    }
    free(ftex);
    free(distinct);
}

static MeshEntry *acquire_mesh(MeshEntry *map, int *count, const GeoMesh *gm,
                               const Rgb8 *pal, const TexRemap *rm)
{
    for (int i = 0; i < *count; i++)
        if (map[i].key == gm && map[i].has_remap == (rm != NULL) &&
            (!rm || (map[i].remap.veh == rm->veh && map[i].remap.part == rm->part)))
            return &map[i];

    MeshEntry *e = &map[(*count)++];
    e->key = gm;
    e->has_remap = rm != NULL;
    if (rm) e->remap = *rm;
    build_mesh_batches(e, gm, pal, rm);
    return e;
}

/* ----------------------------------------------------------------------- */

void worldview_run(const char *mission_path, RenderMode mode)
{
    Scene *scene = scene_load_mission(mission_path);
    if (!scene) {
        fprintf(stderr, "[worldview] failed to load %s\n", mission_path);
        return;
    }

    /* Player car: the Dover Stag chassis (drive mode; also parked when flying).
     * Parts are vehicle-root-relative; the world transform is composed per
     * frame from the drive-mode position/heading. */
    VehicleModel *veh = vehicle_load_vcf("vdstag1.vcf");   /* vdstag.vdf + stag1.vtf */

    /* Terrain — both representations, rebuilt when the height scale changes. */
    /* HS is a debug multiplier on TERRAIN_Y_SCALE: 1.0 is exact; P cycles it. */
    static const float HS[] = { 1.0f, 0.85f, 1.25f, 1.5f, 0.5f };
    int hs_idx = 0;
    float      **ter_cpu = calloc((size_t)scene->patch_count + 1, sizeof(*ter_cpu));
    uint32_t    *ter_vc  = calloc((size_t)scene->patch_count + 1, sizeof(*ter_vc));
    RenderMesh **ter_gpu = calloc((size_t)scene->patch_count + 1, sizeof(*ter_gpu));
    int terrain_dirty = 1;
    /* Road ribbon (drapes on terrain, so rebuilt with it on a height-scale change). */
    RoadQuad   *roads   = NULL;   /* the original's road quads */
    int         road_n  = 0;

    Font *font = font_load("base6x7.fnt");
    input_init();

    /* The 8-bit framebuffer and the mesh-pass textures share this palette; the
     * blit overlay keys out index 0 by value, so it needs no separate palette. */
    Rgb8 pal_sw[256];
    load_act_palette(scene->palette_name, pal_sw);   /* WDEF terrain palette (t15.act) */
    texture_set_palette((const uint8_t *)pal_sw);    /* for quantizing .m16 textures */
    s_sky_idx     = nearest_index(pal_sw, 42, 52, 74);   /* dusk sky    */
    s_terrain_idx = nearest_index(pal_sw, 96, 84, 60);   /* brown ground */
    build_checker(pal_sw);

    /* Global light from the mission's time of day (WRLD hour) + its .lum shade
     * table (ambient floor/tint). Base archives only — lighting isn't moddable.
     * L steps the hour by 3 (one phase) to preview every phase. */
    size_t lum_sz = 0;
    uint8_t *lum = scene->lum_name[0] ? vfs_lod(scene->lum_name, &lum_sz) : NULL;
    int light_hour = scene->hour;
    WorldLighting wl;
    lighting_build(&wl, light_hour, (const uint8_t *)pal_sw, lum, lum_sz);

    /* The mission's authored ground texture (WDEF WRLD name, e.g. tt161sd_.map),
     * planar-mapped onto the heightfield. Resolved through texture_get's pak
     * fallback since it lives in a .pak, not a loose file. */
    s_terrain_tex = scene->ground_tex[0] ? texture_get(scene->ground_tex) : NULL;
    if (s_terrain_tex) fprintf(stdout, "[worldview] ground texture %s -> %dx%d\n",
                               scene->ground_tex, s_terrain_tex->w, s_terrain_tex->h);
    else fprintf(stdout, "[worldview] ground texture '%s' unavailable — flat fill\n",
                 scene->ground_tex);

    /* Terrain's GPU texture (shared by every patch). */
    const RenderTexture *ter_render_tex = s_terrain_tex
        ? gpu_tex_for(s_terrain_tex) : gpu_tex_for_flat(s_terrain_idx);

    /* Road: flat dark asphalt for now (a road .map is a follow-up). */
    /* Road textures: the original's 30 (or the mission's .rtm list). A
     * missing one falls back to flat asphalt (the original aborts:
     * "Road texture not found"). */
    uint8_t road_idx = nearest_index(pal_sw, 34, 34, 40);
    const RenderTexture *road_flat_gpu = gpu_tex_for_flat(road_idx);
    const Texture       *road_tex[30];
    const RenderTexture *road_gpu_tex[30];
    {
        char names[30][16];
        road_texture_names(mission_path, names);
        for (int i = 0; i < 30; i++) {
            road_tex[i] = texture_get(names[i]);
            if (road_tex[i] && !road_tex[i]->pixels) road_tex[i] = NULL;
            if (!road_tex[i]) fprintf(stderr, "[worldview] road texture %s not found\n", names[i]);
            road_gpu_tex[i] = road_tex[i] ? gpu_tex_for(road_tex[i]) : road_flat_gpu;
        }
    }

    /* Per-object meshes (deduped) → per-texture GPU batches; needs the palette
     * resolved above, so this runs after the texture/palette setup. */
    int veh_parts = veh ? veh->count : 0;
    MeshEntry *mesh_map = calloc((size_t)scene->count + veh_parts + 1, sizeof(*mesh_map));
    int mesh_map_count = 0;
    MeshEntry **item = calloc((size_t)scene->count, sizeof(*item));
    for (int i = 0; i < scene->count; i++)
        if (scene->items[i].mesh)
            item[i] = acquire_mesh(mesh_map, &mesh_map_count, scene->items[i].mesh, pal_sw, NULL);
    MeshEntry **veh_item = calloc((size_t)(veh_parts ? veh_parts : 1), sizeof(*veh_item));
    for (int i = 0; i < veh_parts; i++)
        if (veh->parts[i].mesh) {
            TexRemap rm = { veh, i };
            veh_item[i] = acquire_mesh(mesh_map, &mesh_map_count, veh->parts[i].mesh, pal_sw, &rm);
        }

    /* Software framebuffer + depth. */
    static uint8_t fb[FB_W * FB_H];
    static float   zb[FB_W * FB_H];
    SoftTarget tgt = { FB_W, FB_H, fb, zb };

    /* Free-fly camera — start backed off from the centre, looking at it. */
    float eye[3] = { scene->center[0] - scene->radius,
                     scene->center[1] + scene->radius * 0.5f,
                     scene->center[2] };
    float to[3];  vsub(scene->center, eye, to);
    float yaw   = atan2f(to[2], to[0]);
    float pitch = atan2f(to[1], sqrtf(to[0]*to[0] + to[2]*to[2]));

    float base_speed = scene->radius * 0.8f;  if (base_speed < 20.0f) base_speed = 20.0f;
    /* Near plane is camera-relative, NOT scene-sized: the track camera sits
     * ~9 units from the car, and a scene-scaled near (N01: r 917 -> 9.2) cut
     * away everything in front of it — the ground across the bottom of the
     * screen. D32_SFLOAT depth keeps the far range usable at this ratio. */
    float znear = 0.25f;
    float zfar  = scene->radius * 40.0f;       if (zfar  < 5000.0f) zfar  = 5000.0f;

    /* Player car (drive mode): starts at the scene centre, on the ground;
     * SPACE toggles free-fly <-> drive; O toggles orbit (inspect the car) and
     * returns to the previous mode. veh_R/veh_t = its world transform. */
    DriveCar car;
    car_init(&car, scene, HS[hs_idx]);
    int drive = 0;

    /* Original key bindings (input.map + gamekey.map from the asset root). */
    keybind_load();
    DriveActions da = { keybind_action("throttle_up"), keybind_action("throttle_down"),
                        keybind_action("steer_left"),  keybind_action("steer_right"),
                        keybind_action("throttle"), keybind_action("steer"),
                        keybind_action("e_brake"), keybind_action("start_engine"),
                        keybind_action("shift_up"), keybind_action("shift_down"),
                        keybind_action("reverse_direction") };
    int da_prev[4] = { 0, 0, 0, 0 };
    int ax_pitch = keybind_action("track_pitch_delta");
    int ax_yaw   = keybind_action("track_yaw_delta");
    int ax_dist  = keybind_action("track_distance");
    int cmd_view_first = keybind_command_id("PRESET_VIEW_1");   /* 0x0e */
    int cmd_view_last  = keybind_command_id("PRESET_VIEW_12");  /* 0x19 */
    int view_type = 0x0f;          /* default external view (0043e210) */
    TrackCam tcam;
    trackcam_reset(&tcam);

    /* Camera target = the car's bounding-box centre + radius over its whole
     * part tree (entity_spawn 00453d50: bbox 0045a770, (min+max)*0.5; radius
     * 0045a5d0 = max vertex distance from the centre), in the car's frame. */
    float car_centre[3] = { 0, 1, 0 }, car_radius = 3.0f;
    float mn[3] = { -1, 0, -2 }, mx[3] = { 1, 1.5f, 2 };      /* body box */
    if (veh) {
        for (int k = 0; k < 3; k++) { mn[k] = 1e7f; mx[k] = -1e7f; }
        for (int pass = 0; pass < 2; pass++) {
            float r2max = 0;
            for (int i = 0; i < veh->count; i++) {
                const GeoMesh *m = veh->parts[i].mesh;
                if (!m) continue;
                for (int v = 0; v < m->num_verts; v++) {
                    float p[3];
                    mat3_vec(veh->parts[i].R, &m->verts[v * 3], p);
                    for (int k = 0; k < 3; k++) p[k] += veh->parts[i].t[k];
                    if (pass == 0) {
                        for (int k = 0; k < 3; k++) {
                            if (p[k] < mn[k]) mn[k] = p[k];
                            if (p[k] > mx[k]) mx[k] = p[k];
                        }
                    } else {
                        float d2 = 0;
                        for (int k = 0; k < 3; k++) d2 += (p[k] - car_centre[k]) * (p[k] - car_centre[k]);
                        if (d2 > r2max) r2max = d2;
                    }
                }
            }
            if (pass == 0) for (int k = 0; k < 3; k++) car_centre[k] = (mn[k] + mx[k]) * 0.5f;
            else car_radius = sqrtf(r2max);
        }
        fprintf(stdout, "[worldview] car camera target: centre %.2f %.2f %.2f, radius %.2f\n",
                car_centre[0], car_centre[1], car_centre[2], car_radius);
    }
    TerrainGroundCtx gctx = { scene, HS[hs_idx] };

    /* Vehicle simulation for the drive mode. */
    TerrainGroundCtx vctx = { scene, HS[hs_idx] };
    VsWorld vworld = { vehsim_ground_cb, &vctx, {0}, {0}, {0}, {0} };
    for (int i = 0; i < 8; i++) {
        /* Missions without a surface table use a common default. */
        vworld.grip[i]   = scene->have_surfaces ? scene->surf_grip[i]   : 1.9f;
        vworld.roll[i]   = scene->have_surfaces ? scene->surf_roll[i]   : 0.03f;
        vworld.bump[i]   = scene->have_surfaces ? scene->surf_bump[i]   : 0.0f;
        vworld.effect[i] = scene->have_surfaces ? scene->surf_effect[i] : 0;
    }
    VsParams vparams;
    VehSim sim;
    if (!vehsim_params_load(&vparams, "vdstag1.vcf", veh, mn, mx))
        fprintf(stderr, "[worldview] vehsim: vehicle data missing; defaults in use\n");
    {
        float f0[3] = { 1, 0, 0 };
        vehsim_spawn(&sim, &vparams, &vworld, car.x, car.z, f0);
    }
    float cam_up[3] = { 0, 1, 0 };
    char view_msg[64] = "";
    int orbit = 0;
    /* Orbit camera about the car: yaw/pitch around it, distance = zoom. */
    float orb_yaw = 0.0f, orb_pitch = 0.35f, orb_dist = 8.0f;
    float *veh_R = car.R;
    float  veh_t[3] = { car.x, car.y, car.z };

    fprintf(stdout, "[worldview] %d objects, %d terrain patches, start=%s. "
                    "SPACE drive/fly, O orbit car, WASD/arrows, Shift boost, P hscale, TAB SW/HW, ESC.\n",
            scene->count, scene->patch_count,
            mode == RENDER_SOFTWARE ? "software" : "hardware");

    /* Discard anything buffered while videos played / the scene loaded, so
     * stale presses don't fire viewer actions on the first frame. */
    platform_flush_events();

    uint64_t last = platform_get_ticks();
    while (1) {
        uint64_t fstart = platform_get_ticks();
        float dt = (float)(fstart - last) / 1000.0f; last = fstart;
        if (dt > 0.1f) dt = 0.1f;

        RawInputState in; input_poll_raw(&in);
        keybind_update(&in, dt);
        for (int cmd; (cmd = keybind_next_command()) != 0; ) {
            if (cmd >= cmd_view_first && cmd <= cmd_view_last) {
                /* PRESET_VIEW_n -> camera_set_view(type = command id). */
                view_type = cmd;
                if (view_type == 0x0f) snprintf(view_msg, sizeof view_msg, "VIEW 0x0f TRACK");
                else snprintf(view_msg, sizeof view_msg, "VIEW 0x%02x not implemented (track)", view_type);
            }
        }
        gctx.hscale = HS[hs_idx];
        cam_up[0] = 0; cam_up[1] = 1; cam_up[2] = 0;
        if (in.quit) break;
        if (in.pressed[PK_TAB] && !drive)   /* in drive mode TAB is reverse */
            mode = (mode == RENDER_SOFTWARE) ? RENDER_HARDWARE : RENDER_SOFTWARE;
        if (in.pressed[PK_O]) {
            orbit = !orbit;
            /* Start behind the car (same side the chase cam looks from). */
            if (orbit) orb_yaw = car.yaw + (float)M_PI;
        }
        if (in.pressed[PK_SPACE] && !orbit) drive = !drive;
        if (in.pressed[PK_L]) {
            light_hour = ((light_hour < 0 ? 12 : light_hour) + 3) % 24;
            lighting_build(&wl, light_hour, (const uint8_t *)pal_sw, lum, lum_sz);
        }
        if (in.pressed[PK_P]) { hs_idx = (hs_idx + 1) % (int)(sizeof HS / sizeof HS[0]); terrain_dirty = 1; }

        float fwd[3];
        if (orbit) {
            /* Orbit: car parked; arrows/AD rotate, WS pitch, QE zoom (Shift = fast). */
            float rot = 1.6f * dt * (in.held[PK_LSHIFT] ? 2.5f : 1.0f);
            if (in.held[PK_LEFT]  || in.held[PK_A]) orb_yaw   += rot;
            if (in.held[PK_RIGHT] || in.held[PK_D]) orb_yaw   -= rot;
            if (in.held[PK_UP]    || in.held[PK_W]) orb_pitch += rot;
            if (in.held[PK_DOWN]  || in.held[PK_S]) orb_pitch -= rot;
            if (orb_pitch >  1.5f) orb_pitch =  1.5f;
            if (orb_pitch < -0.3f) orb_pitch = -0.3f;
            float zoom = powf(in.held[PK_LSHIFT] ? 4.0f : 2.0f, dt);
            if (in.held[PK_Q]) orb_dist *= zoom;
            if (in.held[PK_E]) orb_dist /= zoom;
            if (orb_dist <  2.0f) orb_dist =  2.0f;
            if (orb_dist > 80.0f) orb_dist = 80.0f;

            float focus[3] = { car.x, car.y + 1.0f, car.z };
            float cp = cosf(orb_pitch);
            eye[0] = focus[0] + cosf(orb_yaw) * cp * orb_dist;
            eye[1] = focus[1] + sinf(orb_pitch) * orb_dist;
            eye[2] = focus[2] + sinf(orb_yaw) * cp * orb_dist;
            float cam_floor = sample_terrain_y(scene, eye[0], eye[2], HS[hs_idx]) + 0.4f;
            if (eye[1] < cam_floor) eye[1] = cam_floor;
            vsub(focus, eye, fwd);
            vnorm(fwd);
        } else if (drive) {
            /* Driver controls: analog axes or the digital keys (WASD too);
             * toggle actions act on the press. */
            VsControls vc = {0};
            vc.throttle_axis = (float)keybind_axis(da.throttle) * (1.0f / 65536.0f);
            vc.steer_axis    = (float)keybind_axis(da.steer)    * (1.0f / 65536.0f);
            vc.throttle_up   = keybind_button(da.throttle_up)   || in.held[PK_W];
            vc.throttle_down = keybind_button(da.throttle_down) || in.held[PK_S];
            vc.steer_left    = keybind_button(da.steer_left)    || in.held[PK_A];
            vc.steer_right   = keybind_button(da.steer_right)   || in.held[PK_D];
            vc.ebrake        = keybind_button(da.ebrake);
            int now[4] = { keybind_button(da.start_engine), keybind_button(da.shift_up),
                           keybind_button(da.shift_down), keybind_button(da.reverse) };
            vc.start_engine = now[0] && !da_prev[0];
            vc.shift_up     = now[1] && !da_prev[1];
            vc.shift_down   = now[2] && !da_prev[2];
            vc.reverse      = now[3] && !da_prev[3];
            memcpy(da_prev, now, sizeof now);
            vehsim_controls(&sim, &vc, dt);
            vehsim_frame(&sim, &vworld, dt);
            vehsim_matrix(&sim, car.R);
            car.x = (float)sim.pos[0]; car.y = (float)sim.pos[1]; car.z = (float)sim.pos[2];
            car.speed = sim.speed;
            car.grounded = !(sim.flags & 4);
            car.yaw = atan2f(sim.axes[2][0], sim.axes[2][2]);
            veh_t[0] = car.x; veh_t[1] = car.y; veh_t[2] = car.z;

            /* The original external track camera,
             * aimed at the car's bbox centre, driven by track_* bindings. */
            float lookat[3];
            mat3_vec(car.R, car_centre, lookat);
            lookat[0] += car.x; lookat[1] += car.y; lookat[2] += car.z;
            trackcam_update(&tcam, car.R, lookat, car_radius,
                            keybind_axis(ax_pitch), keybind_axis(ax_yaw), keybind_axis(ax_dist),
                            dt, terrain_ground_cb, &gctx, eye, fwd, cam_up);
        } else {
            /* Free-fly: arrows look, WASD/QE move, Shift boost. */
            float look = 1.4f * dt;
            if (in.held[PK_LEFT])  yaw   += look;
            if (in.held[PK_RIGHT]) yaw   -= look;
            if (in.held[PK_UP])    pitch += look;
            if (in.held[PK_DOWN])  pitch -= look;
            if (pitch >  1.5f) pitch =  1.5f;
            if (pitch < -1.5f) pitch = -1.5f;

            float cp = cosf(pitch), sp = sinf(pitch), cy = cosf(yaw), sy = sinf(yaw);
            fwd[0] = cp*cy; fwd[1] = sp; fwd[2] = cp*sy;
            float wup[3]   = { 0, 1, 0 };
            float right[3]; vcross(wup, fwd, right); vnorm(right);   /* left-handed */

            float spd = base_speed * dt * (in.held[PK_LSHIFT] ? 4.0f : 1.0f);
            if (in.held[PK_W]) for (int i=0;i<3;i++) eye[i] += fwd[i]   * spd;
            if (in.held[PK_S]) for (int i=0;i<3;i++) eye[i] -= fwd[i]   * spd;
            if (in.held[PK_D]) for (int i=0;i<3;i++) eye[i] += right[i] * spd;
            if (in.held[PK_A]) for (int i=0;i<3;i++) eye[i] -= right[i] * spd;
            if (in.held[PK_E]) eye[1] += spd;
            if (in.held[PK_Q]) eye[1] -= spd;
        }

        if (terrain_dirty) {
            for (int i = 0; i < scene->patch_count; i++) {
                free(ter_cpu[i]);
                if (ter_gpu[i]) render_mesh_destroy(ter_gpu[i]);
                ter_cpu[i] = build_terrain_verts(scene, &scene->patches[i], HS[hs_idx],
                                                 TERRAIN_STEP, 0, &ter_vc[i]);
                uint32_t gvc = 0;      /* GPU: full resolution (step 1) */
                float *gv = build_terrain_verts(scene, &scene->patches[i], HS[hs_idx], 1, 1, &gvc);
                ter_gpu[i] = gv ? render_mesh_create(gv, gvc) : NULL;
                free(gv);
            }
            free_road_quads(roads, road_n);
            roads = build_road_quads(scene, HS[hs_idx], &road_n);
            terrain_dirty = 0;
        }

        /* View + projection (shared by both backends). FOV is the original's:
         * 90 deg HORIZONTAL (camera_setup arg 0x3fc90fda = pi/2 at zoom 1.0;
         * cam[0x2c] = tan(fov/2) against the viewport half-WIDTH). Converted
         * to the vertical FOV our mat_perspective takes: 73.7 deg at 4:3.
         * (Original far plane is 150 / mission WDEF value; the dev viewer
         * deliberately keeps its long draw distance instead.) */
        float fovy = 2.0f * atanf(tanf(0.25f * (float)M_PI) * (float)FB_H / (float)FB_W);
        float view[16], proj[16];
        mat_lookat_up(view, eye, fwd, cam_up);
        mat_perspective(proj, fovy, (float)FB_W / FB_H, znear, zfar);
        /* The world is left-handed; mirror clip x. */
        proj[0] = -proj[0];

        render_begin_frame();

        if (mode == RENDER_HARDWARE) {
            /* Camera-relative: eye at the origin; every model translation is
             * (position - eye), see rel_model. */
            static const float origin3[3] = { 0, 0, 0 };
            float view_rel[16];
            mat_lookat_up(view_rel, origin3, fwd, cam_up);
            render_set_lighting(&wl.gpu);
            render_3d_begin(view_rel, proj);
            for (int i = 0; i < scene->patch_count; i++)
                if (ter_gpu[i]) {
                    const TerrainPatch *tp = &scene->patches[i];
                    float to[3] = { tp->origin[0], 0.0f, tp->origin[1] }, model[16];
                    rel_model(model, NULL, to, eye);
                    render_3d_draw(ter_gpu[i], model, ter_render_tex);
                }
            for (int i = 0; i < road_n; i++) {
                int t = road_quad_tex(&roads[i], eye, fwd);
                if (t >= 0 && roads[i].gpu) {
                    float ro[3] = { roads[i].org[0], 0.0f, roads[i].org[1] }, model[16];
                    rel_model(model, NULL, ro, eye);
                    render_3d_draw_decal(roads[i].gpu, model, road_gpu_tex[t]);
                }
            }
            for (int i = 0; i < scene->count; i++) {
                if (!item[i]) continue;
                float model[16]; rel_model(model, scene->items[i].R, scene->items[i].t, eye);
                for (int b = 0; b < item[i]->batch_count; b++)
                    render_3d_draw(item[i]->batches[b].gpu, model, item[i]->batches[b].tex);
            }
            for (int i = 0; i < veh_parts; i++) {
                if (!veh_item[i]) continue;
                float Rw[9], tw[3], model[16];
                mat3_mul(veh_R, veh->parts[i].R, Rw);
                mat3_vec(veh_R, veh->parts[i].t, tw);
                for (int k = 0; k < 3; k++) tw[k] += veh_t[k];
                rel_model(model, Rw, tw, eye);
                for (int b = 0; b < veh_item[i]->batch_count; b++)
                    render_3d_draw(veh_item[i]->batches[b].gpu, model, veh_item[i]->batches[b].tex);
            }
            memset(fb, 0, sizeof fb);   /* transparent HUD overlay */
        } else {
            soft_clear(&tgt, s_sky_idx);
            soft_set_viewproj(view, proj);
            for (int i = 0; i < scene->patch_count; i++)
                if (ter_cpu[i])
                    draw_terrain_sw(&tgt, ter_cpu[i], ter_vc[i], s_terrain_tex, s_terrain_idx);
            for (int i = 0; i < road_n; i++) {
                int t = road_quad_tex(&roads[i], eye, fwd);
                if (t >= 0 && roads[i].sw)
                    draw_terrain_sw(&tgt, roads[i].sw, roads[i].sw_vc, road_tex[t], road_idx);
            }
            for (int i = 0; i < scene->count; i++)
                draw_item_sw(&tgt, &scene->items[i], pal_sw, NULL);
            for (int i = 0; i < veh_parts; i++) {
                if (!veh->parts[i].mesh) continue;
                SceneItem part = veh->parts[i];      /* world = veh * part */
                mat3_mul(veh_R, veh->parts[i].R, part.R);
                mat3_vec(veh_R, veh->parts[i].t, part.t);
                for (int k = 0; k < 3; k++) part.t[k] += veh_t[k];
                TexRemap rm = { veh, i };
                draw_item_sw(&tgt, &part, pal_sw, &rm);
            }
        }

        /* HUD text (into fb either way: transparent over HW, atop the SW scene). */
        if (font) {
            char line[160];
            if (orbit)
                snprintf(line, sizeof line, "%s   %s   ORBIT  dist %.1f   (arrows/WASD rotate, Q/E zoom, O exit)",
                         mission_path, mode == RENDER_SOFTWARE ? "SOFTWARE" : "HARDWARE", orb_dist);
            else if (drive)
                snprintf(line, sizeof line, "%s   %s   DRIVE %c gear %d  %4.0f rpm  %3.0f mph%s%s   TRACK pitch %.0f yaw %.0f dist %.1fx  %s",
                         mission_path, mode == RENDER_SOFTWARE ? "SOFTWARE" : "HARDWARE",
                         "PRND21"[sim.selector % 6], sim.e.gear, sim.e.rpm, sim.e.speedo,
                         car.grounded ? "" : "  AIR", (sim.flags & 2) ? "  SKID" : "",
                         tcam.pitch * 57.29578f, tcam.yaw * 57.29578f, tcam.dist, view_msg);
            else
                snprintf(line, sizeof line, "%s   %s   FLY  eye %.0f %.0f %.0f   hscale %.2f",
                         mission_path, mode == RENDER_SOFTWARE ? "SOFTWARE" : "HARDWARE",
                         eye[0], eye[1], eye[2], HS[hs_idx]);
            font_draw(font, line, fb, FB_W, FB_H, 8, 8, IDX_TEXT);
            snprintf(line, sizeof line, "TIME %02d:00%s  phase %d  %s %.0f deg  I %.3f%s",
                     wl.hour < 0 ? 12 : wl.hour, light_hour == scene->hour ? "" : " (L preview)",
                     wl.phase, wl.is_day ? "SUN" : "MOON", wl.angle * 57.29578f, wl.intensity,
                     mode == RENDER_SOFTWARE ? "   (SW: unlit)" : "");
            font_draw(font, line, fb, FB_W, FB_H, 8, 20, IDX_TEXT);
            font_draw(font, drive
                      ? "DRIVE: KEYPAD ARROWS (input.map) or WASD   CAMERA: GREY ARROWS  PGUP/PGDN DIST  F2 TRACK   SPACE FLY  ESC"
                      : "SPACE DRIVE/FLY  O ORBIT  L HOUR+3  WASD/ARROWS  SHIFT BOOST  P HSCALE  TAB SW/HW  ESC",
                      fb, FB_W, FB_H, 8, FB_H - 16, IDX_DIM);
        }

        render_set_palette(pal_sw);
        render_blit_indexed(fb, FB_W, FB_H);
        render_end_frame();

        float el_ms = (float)(platform_get_ticks() - fstart);
        if (el_ms < TARGET_FRAME_MS) platform_sleep((uint32_t)(TARGET_FRAME_MS - el_ms));
    }

    for (int i = 0; i < mesh_map_count; i++) {
        for (int b = 0; b < mesh_map[i].batch_count; b++)
            if (mesh_map[i].batches[b].gpu) render_mesh_destroy(mesh_map[i].batches[b].gpu);
        free(mesh_map[i].batches);
    }
    for (int i = 0; i < scene->patch_count; i++) {
        free(ter_cpu[i]);
        if (ter_gpu[i]) render_mesh_destroy(ter_gpu[i]);
    }
    free_road_quads(roads, road_n);
    for (int i = 0; i < s_gtex_count; i++)
        if (s_gtex[i].gpu) render_texture_destroy(s_gtex[i].gpu);
    s_gtex_count = 0;
    free(mesh_map); free(item); free(veh_item); free(ter_cpu); free(ter_vc); free(ter_gpu);
    free(s_checker.pixels); s_checker.pixels = NULL;
    /* Leave the shared lighting UBO unlit for whatever runs next. */
    RenderLighting unlit = { .light_dir = {0,1,0}, .haze_base = 1.0f, .haze_min = 1.0f };
    for (int r = 0; r < RENDER_LIGHT_ROWS; r++)
        unlit.curve[r][0] = unlit.curve[r][1] = unlit.curve[r][2] = 1.0f;
    render_set_lighting(&unlit);
    free(lum);
    texture_cache_clear();
    if (font) font_free(font);
    vehicle_free(veh);
    scene_free(scene);
}
