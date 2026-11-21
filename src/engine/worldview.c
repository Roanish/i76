/*
 * worldview.c — solid 3D scene viewer with a free-fly camera (see worldview.h).
 *
 * Builds geometry from the mission Scene (objects + terrain) and renders it
 * through either backend — the software CPU rasterizer (softraster.c) or the
 * hardware Vulkan pass (render_3d_*) — sharing one camera so TAB gives a direct
 * A/B, mirroring the original game's software/Glide split. Both produce a
 * flat-shaded, depth-buffered, two-sided look for judging placement.
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
#include "engine/texture.h"
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

static void mat_lookat(float m[16], const float eye[3], const float fwd[3])
{
    float wup[3] = {0,1,0};
    float f[3] = { fwd[0], fwd[1], fwd[2] }; vnorm(f);
    float s[3]; vcross(f, wup, s); vnorm(s);
    float u[3]; vcross(s, f, u);

    m[0]=s[0]; m[4]=s[1]; m[8] =s[2];  m[12]=-vdot(s,eye);
    m[1]=u[0]; m[5]=u[1]; m[9] =u[2];  m[13]=-vdot(u,eye);
    m[2]=-f[0];m[6]=-f[1];m[10]=-f[2]; m[14]= vdot(f,eye);
    m[3]=0;    m[7]=0;    m[11]=0;     m[15]=1;
}

/* SceneItem world transform (R row-major 3x3 + t) -> column-major model mat4. */
static void item_model(float m[16], const float R[9], const float t[3])
{
    m[0]=R[0]; m[1]=R[3]; m[2] =R[6]; m[3]=0;
    m[4]=R[1]; m[5]=R[4]; m[6] =R[7]; m[7]=0;
    m[8]=R[2]; m[9]=R[5]; m[10]=R[8]; m[11]=0;
    m[12]=t[0]; m[13]=t[1]; m[14]=t[2]; m[15]=1;
}

/* ----------------------------------------------------------------------- */
/* Mesh building — triangulate faces + bake flat normals                     */
/* ----------------------------------------------------------------------- */

/* GPU terrain mesh: interleaved [px,py,pz, u,v] (planar UV in world x/z), drawn
 * by the hardware path. The software path reads the patch heightfield directly. */
#define MV 5   /* floats per GPU mesh vertex (pos3 + uv2); matches render_vk.c */
#define TERRAIN_TILE 96.0f   /* world units per terrain-texture repeat */
/* Vertical scale: world Y = height12 * 0.1 (nitro.exe terrain_height_sample,
 * FUN_004953e0). `hscale` is now a debug multiplier on this (1.0 = exact). */
#define TERRAIN_Y_SCALE 0.1f

/* Height (12-bit, unscaled) at a GLOBAL fine-grid point, searching all patches.
 * Adjacent patches are contiguous on the global grid (patch at coarse (row,col)
 * covers points [col*128, col*128+127]), so cross-patch lookups resolve the
 * neighbour's border points — the original samples one global zone grid the
 * same way (terrain_height_sample, FUN_004953e0). -1 if no patch covers it. */
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

static float *build_terrain_verts(const Scene *s, const TerrainPatch *p,
                                  float hscale, uint32_t *out_count)
{
    int n = p->dim, step = TERRAIN_STEP;
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
            int ok = 1;
            for (int i = 0; i < 4; i++) {
                float h = cell_h12(s, c0 + cx[i], r0 + cz[i]);
                if (h < 0.0f) { ok = 0; break; }
                c[i][0] = p->origin[0] + cx[i] * p->cell;
                c[i][1] = h * (TERRAIN_Y_SCALE * hscale);
                c[i][2] = p->origin[1] + cz[i] * p->cell;
            }
            if (!ok) continue;
            int quad[6] = { 0,1,2, 0,2,3 };
            for (int t = 0; t < 6; t++) {
                const float *a = c[quad[t]];
                *w++ = a[0]; *w++ = a[1]; *w++ = a[2];
                *w++ = a[0] / TERRAIN_TILE; *w++ = a[2] / TERRAIN_TILE;
            }
        }
    }
    *out_count = (uint32_t)(w - v) / MV;
    return v;
}

/* Terrain world-Y under (x,z): bilinear over the 4 surrounding global grid
 * points, scaled by 0.1 — faithful to nitro.exe terrain_height_sample
 * (FUN_004953e0), including across patch boundaries. Missing neighbours fall
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

/* Build the road ribbon as a [px,py,pz, u,v] triangle soup, draped on terrain.
 * Consecutive ribs form quads; a large XZ jump (junction / loop seam) splits the
 * strip so no quad spans the gap. Roads lift slightly above the ground. */
#define ROAD_Y_OFFSET 0.6f
#define ROAD_BREAK    60.0f
#define ROAD_TILE     16.0f   /* world units per road-texture repeat along length */

static float *build_road_verts(const Scene *s, float hscale, uint32_t *out_count)
{
    if (s->road_count < 2) return NULL;
    float *v = malloc((size_t)s->road_count * 6 * MV * sizeof(float));
    if (!v) return NULL;
    float *w = v;
    float ulen = 0.0f;
    for (int i = 0; i + 1 < s->road_count; i++) {
        const RoadRib *a = &s->road[i], *b = &s->road[i + 1];
        float dx = b->lx - a->lx, dz = b->lz - a->lz;
        float adv = sqrtf(dx*dx + dz*dz);
        if (adv > ROAD_BREAK) { ulen = 0; continue; }
        float al[3] = { a->lx, sample_terrain_y(s,a->lx,a->lz,hscale)+ROAD_Y_OFFSET, a->lz };
        float ar[3] = { a->rx, sample_terrain_y(s,a->rx,a->rz,hscale)+ROAD_Y_OFFSET, a->rz };
        float bl[3] = { b->lx, sample_terrain_y(s,b->lx,b->lz,hscale)+ROAD_Y_OFFSET, b->lz };
        float br[3] = { b->rx, sample_terrain_y(s,b->rx,b->rz,hscale)+ROAD_Y_OFFSET, b->rz };
        float u0 = ulen / ROAD_TILE, u1 = (ulen + adv) / ROAD_TILE;
        ulen += adv;
        float tri[6][MV] = {
            { al[0],al[1],al[2], u0,0 }, { ar[0],ar[1],ar[2], u0,1 }, { br[0],br[1],br[2], u1,1 },
            { al[0],al[1],al[2], u0,0 }, { br[0],br[1],br[2], u1,1 }, { bl[0],bl[1],bl[2], u1,0 },
        };
        for (int k = 0; k < 6; k++) for (int j = 0; j < MV; j++) *w++ = tri[k][j];
    }
    *out_count = (uint32_t)(w - v) / MV;
    return v;
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

static void car_tick(DriveCar *c, const Scene *s, const RawInputState *in,
                     float dt, float hscale)
{
    float steer    = (in->held[PK_D] || in->held[PK_RIGHT] ? 1.0f : 0.0f)
                   - (in->held[PK_A] || in->held[PK_LEFT]  ? 1.0f : 0.0f);
    int   throttle = in->held[PK_W] || in->held[PK_UP];
    int   brake    = in->held[PK_S] || in->held[PK_DOWN];

    /* Longitudinal: throttle / brake-then-reverse / drag. */
    if (throttle) c->speed += (in->held[PK_LSHIFT] ? CAR_ACCEL_BOOST : CAR_ACCEL) * dt;
    if (brake)    c->speed -= (c->speed > 0.5f ? CAR_BRAKE : CAR_REV_ACCEL) * dt;
    c->speed -= c->speed * CAR_DRAG * dt;
    if (c->speed > CAR_SPEED_MAX) c->speed = CAR_SPEED_MAX;
    if (c->speed < CAR_SPEED_MIN) c->speed = CAR_SPEED_MIN;

    /* Steering: yaw rate = v/R at low speed (steering lock), grip-capped
     * (a_lat <= CAR_GRIP_LAT => yawrate <= g/v) at high speed. Sign follows
     * the velocity so reversing steers like a real car. No steering in air. */
    float av = fabsf(c->speed);
    if (c->grounded && av > 0.05f) {
        float yr = av / CAR_TURN_RADIUS;
        float cap = CAR_GRIP_LAT / av;
        if (cap < yr) yr = cap;
        c->yaw += steer * yr * (c->speed < 0 ? -1.0f : 1.0f) * dt;
    }
    float dx = cosf(c->yaw), dz = sinf(c->yaw);

    /* Horizontal move, blocked by walls (a big ground step in one frame). */
    float nx = c->x + dx * c->speed * dt;
    float nz = c->z + dz * c->speed * dt;
    float g_ahead = sample_terrain_y(s, nx, nz, hscale);
    if (c->grounded && g_ahead - c->y > CAR_WALL_STEP) {
        c->speed = -c->speed * 0.1f;           /* thump into the cliff */
    } else {
        c->x = nx;
        c->z = nz;
    }

    /* Vertical: gravity, then the terrain is a hard floor. Cresting a ridge
     * leaves the ground (y stays, gravity arcs it down); slopes push the car
     * up via the clamp. */
    c->vy -= CAR_GRAVITY * dt;
    c->y += c->vy * dt;
    float ground = sample_terrain_y(s, c->x, c->z, hscale);
    if (c->y <= ground) {
        c->y = ground;
        if (c->vy < 0) c->vy = 0;
        c->grounded = 1;
    } else {
        c->grounded = (c->y - ground) < 0.05f;
    }

    /* Orientation: on the ground, lean the basis into the terrain slope
     * sampled across the wheelbase/track; airborne keeps the last attitude.
     * Basis columns = (right, up, fwd); smoothed to avoid step jitter. */
    if (c->grounded) {
        float hf = sample_terrain_y(s, c->x + dx * (CAR_WHEELBASE * 0.5f),
                                       c->z + dz * (CAR_WHEELBASE * 0.5f), hscale);
        float hb = sample_terrain_y(s, c->x - dx * (CAR_WHEELBASE * 0.5f),
                                       c->z - dz * (CAR_WHEELBASE * 0.5f), hscale);
        float hr = sample_terrain_y(s, c->x + dz * CAR_HALFTRACK,
                                       c->z - dx * CAR_HALFTRACK, hscale);
        float hl = sample_terrain_y(s, c->x - dz * CAR_HALFTRACK,
                                       c->z + dx * CAR_HALFTRACK, hscale);
        float f[3] = { dx * CAR_WHEELBASE, hf - hb, dz * CAR_WHEELBASE };
        float r[3] = { dz * CAR_HALFTRACK * 2, hr - hl, -dx * CAR_HALFTRACK * 2 };
        vnorm(f);
        float u[3]; vcross(f, r, u); vnorm(u);
        vcross(u, f, r); vnorm(r);
        float k = 1.0f - expf(-CAR_LEAN_RATE * dt);
        float tgt[9] = { r[0], u[0], f[0],
                         r[1], u[1], f[1],
                         r[2], u[2], f[2] };
        for (int i = 0; i < 9; i++) c->R[i] += (tgt[i] - c->R[i]) * k;
        /* Re-orthonormalise the smoothed basis (columns). */
        float cf[3] = { c->R[2], c->R[5], c->R[8] };
        float cr[3] = { c->R[0], c->R[3], c->R[6] };
        vnorm(cf);
        float cu[3]; vcross(cf, cr, cu); vnorm(cu);
        vcross(cu, cf, cr);
        c->R[0] = cr[0]; c->R[1] = cu[0]; c->R[2] = cf[0];
        c->R[3] = cr[1]; c->R[4] = cu[1]; c->R[5] = cf[1];
        c->R[6] = cr[2]; c->R[7] = cu[2]; c->R[8] = cf[2];
    }
}

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
        buf = vfs_read_file(actname, &sz);
    }
    if (!buf || sz < 768) {
        vfs_free(buf); buf = NULL; sz = 0;
        strncpy(actname, "superpal.act", sizeof actname);
        buf = vfs_read_file(actname, &sz);
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
                    const float *p = &m->verts[m->indices[ei[j]] * 3];
                    *w++ = p[0]; *w++ = p[1]; *w++ = p[2];
                    *w++ = m->uv[ei[j] * 2]; *w++ = m->uv[ei[j] * 2 + 1];
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
    /* The real vertical scale is now known (TERRAIN_Y_SCALE = 0.1 on the 12-bit
     * height; nitro.exe terrain_height_sample / FUN_004953e0). HS is a debug
     * multiplier on top — 1.0 is exact; P cycles it to eyeball the result. */
    static const float HS[] = { 1.0f, 0.85f, 1.25f, 1.5f, 0.5f };
    int hs_idx = 0;
    float      **ter_cpu = calloc((size_t)scene->patch_count + 1, sizeof(*ter_cpu));
    uint32_t    *ter_vc  = calloc((size_t)scene->patch_count + 1, sizeof(*ter_vc));
    RenderMesh **ter_gpu = calloc((size_t)scene->patch_count + 1, sizeof(*ter_gpu));
    int terrain_dirty = 1;
    /* Road ribbon (drapes on terrain, so rebuilt with it on a height-scale change). */
    float      *road_cpu = NULL;
    uint32_t    road_vc  = 0;
    RenderMesh *road_gpu = NULL;

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
    uint8_t road_idx = nearest_index(pal_sw, 34, 34, 40);
    const RenderTexture *road_render_tex = gpu_tex_for_flat(road_idx);

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
    float znear = scene->radius * 0.01f;       if (znear < 0.5f)    znear = 0.5f;
    float zfar  = scene->radius * 40.0f;       if (zfar  < 5000.0f) zfar  = 5000.0f;

    /* Player car (drive mode): starts at the scene centre, on the ground;
     * SPACE toggles free-fly <-> drive. veh_R/veh_t = its world transform. */
    DriveCar car;
    car_init(&car, scene, HS[hs_idx]);
    int drive = 0;
    float *veh_R = car.R;
    float  veh_t[3] = { car.x, car.y, car.z };

    fprintf(stdout, "[worldview] %d objects, %d terrain patches, start=%s. "
                    "SPACE drive/fly, WASD/arrows, Shift boost, P hscale, TAB SW/HW, ESC.\n",
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
        if (in.quit) break;
        if (in.pressed[PK_TAB])
            mode = (mode == RENDER_SOFTWARE) ? RENDER_HARDWARE : RENDER_SOFTWARE;
        if (in.pressed[PK_SPACE]) drive = !drive;
        if (in.pressed[PK_P]) { hs_idx = (hs_idx + 1) % (int)(sizeof HS / sizeof HS[0]); terrain_dirty = 1; }

        float fwd[3];
        if (drive) {
            car_tick(&car, scene, &in, dt, HS[hs_idx]);
            veh_t[0] = car.x; veh_t[1] = car.y; veh_t[2] = car.z;

            /* Track (chase) camera — defaults reversed from nitro.exe camera
             * init: track_distance 5.5 (0x40b00000), track_pitch pi/6
             * (0x3f060a92). Rigid horizontal-heading mount, aimed at the car;
             * clamped just above the terrain so it can't dip underground. */
            const float cam_dist = 5.5f, cam_pitch = 0.5235988f;
            float dx = cosf(car.yaw), dz = sinf(car.yaw);
            eye[0] = car.x - dx * cam_dist * cosf(cam_pitch);
            eye[1] = car.y + cam_dist * sinf(cam_pitch);
            eye[2] = car.z - dz * cam_dist * cosf(cam_pitch);
            float cam_floor = sample_terrain_y(scene, eye[0], eye[2], HS[hs_idx]) + 0.4f;
            if (eye[1] < cam_floor) eye[1] = cam_floor;
            fwd[0] = car.x - eye[0];
            fwd[1] = (car.y + 1.0f) - eye[1];
            fwd[2] = car.z - eye[2];
            vnorm(fwd);
        } else {
            /* Free-fly: arrows look, WASD/QE move, Shift boost. */
            float look = 1.4f * dt;
            if (in.held[PK_LEFT])  yaw   -= look;
            if (in.held[PK_RIGHT]) yaw   += look;
            if (in.held[PK_UP])    pitch += look;
            if (in.held[PK_DOWN])  pitch -= look;
            if (pitch >  1.5f) pitch =  1.5f;
            if (pitch < -1.5f) pitch = -1.5f;

            float cp = cosf(pitch), sp = sinf(pitch), cy = cosf(yaw), sy = sinf(yaw);
            fwd[0] = cp*cy; fwd[1] = sp; fwd[2] = cp*sy;
            float wup[3]   = { 0, 1, 0 };
            float right[3]; vcross(fwd, wup, right); vnorm(right);

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
                ter_cpu[i] = build_terrain_verts(scene, &scene->patches[i], HS[hs_idx], &ter_vc[i]);
                ter_gpu[i] = ter_cpu[i] ? render_mesh_create(ter_cpu[i], ter_vc[i]) : NULL;
            }
            free(road_cpu);
            if (road_gpu) render_mesh_destroy(road_gpu);
            road_cpu = build_road_verts(scene, HS[hs_idx], &road_vc);
            road_gpu = road_cpu ? render_mesh_create(road_cpu, road_vc) : NULL;
            terrain_dirty = 0;
        }

        /* View + projection (shared by both backends). FOV is the original's:
         * 90 deg HORIZONTAL (camera_setup arg 0x3fc90fda = pi/2 at zoom 1.0;
         * cam[0x2c] = tan(fov/2) against the viewport half-WIDTH). Converted
         * to the vertical FOV our mat_perspective takes: 73.7 deg at 4:3.
         * (Original far plane is 150 / mission WDEF value; the dev viewer
         * deliberately keeps its long draw distance instead.) */
        float fovy = 2.0f * atanf(tanf(0.25f * (float)M_PI) * (float)FB_H / (float)FB_W);
        float view[16], proj[16], identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        mat_lookat(view, eye, fwd);
        mat_perspective(proj, fovy, (float)FB_W / FB_H, znear, zfar);

        render_begin_frame();

        if (mode == RENDER_HARDWARE) {
            render_3d_begin(view, proj);
            for (int i = 0; i < scene->patch_count; i++)
                if (ter_gpu[i])
                    render_3d_draw(ter_gpu[i], identity, ter_render_tex);
            if (road_gpu) render_3d_draw(road_gpu, identity, road_render_tex);
            for (int i = 0; i < scene->count; i++) {
                if (!item[i]) continue;
                float model[16]; item_model(model, scene->items[i].R, scene->items[i].t);
                for (int b = 0; b < item[i]->batch_count; b++)
                    render_3d_draw(item[i]->batches[b].gpu, model, item[i]->batches[b].tex);
            }
            for (int i = 0; i < veh_parts; i++) {
                if (!veh_item[i]) continue;
                float Rw[9], tw[3], model[16];
                mat3_mul(veh_R, veh->parts[i].R, Rw);
                mat3_vec(veh_R, veh->parts[i].t, tw);
                for (int k = 0; k < 3; k++) tw[k] += veh_t[k];
                item_model(model, Rw, tw);
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
            if (road_cpu)
                draw_terrain_sw(&tgt, road_cpu, road_vc, NULL, road_idx);
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
            if (drive)
                snprintf(line, sizeof line, "%s   %s   DRIVE  %3.0f mph%s",
                         mission_path, mode == RENDER_SOFTWARE ? "SOFTWARE" : "HARDWARE",
                         fabsf(car.speed) * 2.237f, car.grounded ? "" : "  AIR");
            else
                snprintf(line, sizeof line, "%s   %s   FLY  eye %.0f %.0f %.0f   hscale %.2f",
                         mission_path, mode == RENDER_SOFTWARE ? "SOFTWARE" : "HARDWARE",
                         eye[0], eye[1], eye[2], HS[hs_idx]);
            font_draw(font, line, fb, FB_W, FB_H, 8, 8, IDX_TEXT);
            font_draw(font, "SPACE DRIVE/FLY  WASD/ARROWS  SHIFT BOOST  P HSCALE  TAB SW/HW  ESC",
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
    free(road_cpu);
    if (road_gpu) render_mesh_destroy(road_gpu);
    for (int i = 0; i < s_gtex_count; i++)
        if (s_gtex[i].gpu) render_texture_destroy(s_gtex[i].gpu);
    s_gtex_count = 0;
    free(mesh_map); free(item); free(veh_item); free(ter_cpu); free(ter_vc); free(ter_gpu);
    free(s_checker.pixels); s_checker.pixels = NULL;
    texture_cache_clear();
    if (font) font_free(font);
    vehicle_free(veh);
    scene_free(scene);
}
