/*
 * sceneview.c — world scene viewer (see sceneview.h).
 *
 * Renders a mission's placed objects at their real world transforms through an
 * orbit camera with a perspective projection. CPU wireframe into the 8-bit
 * framebuffer; the Vulkan backend does the palette blit.
 */

#include "sceneview.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"
#include "engine/input.h"
#include "engine/font.h"
#include "engine/scene.h"
#include "engine/geomesh.h"
#include "render/render.h"

#define FB_W 640
#define FB_H 480
#define TARGET_FRAME_MS (1000.0 / 60.0)

#define IDX_BG    0
#define IDX_GRID  4
#define IDX_TEXT  3
#define IDX_DIM   5
#define IDX_MARK  2
/* a few wire colours so adjacent objects are distinguishable */
#define IDX_W0    6
#define IDX_NW    8   /* number of wire colours, starting at IDX_W0 */
#define IDX_TER0  14  /* terrain height bands (low/mid/high)         */

/*
 * Height-byte -> world-Y scale. UNCALIBRATED: the .TER height byte (0..250)
 * maps to world Y by an unknown factor — it can't be pinned from the objects
 * (they aren't ground-snapped: the bridge floats, spawns default to Y0) and the
 * runtime sampler isn't reachable by xref. 0.5 gives sensible relief vs the
 * object scale; treat as a tunable until the terrain sampler is found.
 */
#define HEIGHT_SCALE  0.5f
#define TERRAIN_STEP  4    /* render every Nth fine cell (128 -> 32 grid lines) */

static void build_palette(Rgb8 *pal)
{
    memset(pal, 0, sizeof(Rgb8) * 256);
    pal[IDX_BG]   = (Rgb8){ 10,  12,  20  };
    pal[IDX_MARK] = (Rgb8){ 255, 230, 80  };
    pal[IDX_TEXT] = (Rgb8){ 235, 235, 235 };
    pal[IDX_GRID] = (Rgb8){ 38,  44,  58  };
    pal[IDX_DIM]  = (Rgb8){ 120, 120, 130 };
    /* wire colour ramp */
    pal[IDX_W0+0] = (Rgb8){ 90,  220, 130 };
    pal[IDX_W0+1] = (Rgb8){ 120, 180, 255 };
    pal[IDX_W0+2] = (Rgb8){ 255, 150, 120 };
    pal[IDX_W0+3] = (Rgb8){ 220, 130, 230 };
    pal[IDX_W0+4] = (Rgb8){ 230, 220, 110 };
    pal[IDX_W0+5] = (Rgb8){ 110, 230, 220 };
    pal[IDX_W0+6] = (Rgb8){ 180, 230, 130 };
    pal[IDX_W0+7] = (Rgb8){ 230, 180, 150 };
    /* terrain height bands: low (dark) -> high (light) */
    pal[IDX_TER0+0] = (Rgb8){ 40,  70,  55  };
    pal[IDX_TER0+1] = (Rgb8){ 90,  100, 70  };
    pal[IDX_TER0+2] = (Rgb8){ 150, 140, 110 };
}

static void draw_line(uint8_t *fb, int x0, int y0, int x1, int y1, uint8_t col)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if ((unsigned)x0 < FB_W && (unsigned)y0 < FB_H)
            fb[(size_t)y0 * FB_W + x0] = col;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ----------------------------------------------------------------------- */
/* Camera                                                                    */
/* ----------------------------------------------------------------------- */

typedef struct {
    float eye[3];
    float right[3], up[3], fwd[3];
    float focal;
} Cam;

static void vsub(const float a[3], const float b[3], float o[3]) { for(int i=0;i<3;i++) o[i]=a[i]-b[i]; }
static float vdot(const float a[3], const float b[3]) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
static void vcross(const float a[3], const float b[3], float o[3]) {
    o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0];
}
static void vnorm(float v[3]) {
    float l = sqrtf(vdot(v,v)); if (l>1e-6f){ v[0]/=l; v[1]/=l; v[2]/=l; }
}

static void cam_orbit(Cam *c, const float center[3], float az, float el, float dist)
{
    float ce = cosf(el), se = sinf(el), ca = cosf(az), sa = sinf(az);
    c->eye[0] = center[0] + dist * ce * ca;
    c->eye[1] = center[1] + dist * se;
    c->eye[2] = center[2] + dist * ce * sa;

    float wup[3] = {0,1,0};
    vsub(center, c->eye, c->fwd); vnorm(c->fwd);
    /* Left-handed world: right = up x fwd. */
    vcross(wup, c->fwd, c->right); vnorm(c->right);
    vcross(c->fwd, c->right, c->up); vnorm(c->up);
    c->focal = (FB_H * 0.5f) / tanf(0.5f * 1.05f);   /* ~60 deg vertical FOV */
}

/* World point -> screen. Returns 0 if behind the near plane. */
static int project(const Cam *c, const float w[3], float *sx, float *sy)
{
    float rel[3]; vsub(w, c->eye, rel);
    float cz = vdot(rel, c->fwd);
    if (cz < 1.0f) return 0;
    float cx = vdot(rel, c->right);
    float cy = vdot(rel, c->up);
    *sx = FB_W * 0.5f + c->focal * cx / cz;
    *sy = FB_H * 0.5f - c->focal * cy / cz;
    return 1;
}

/* ----------------------------------------------------------------------- */
/* Ground grid for spatial reference (XZ plane at scene center height)       */
/* ----------------------------------------------------------------------- */

static void draw_grid(uint8_t *fb, const Cam *c, const float center[3], float radius)
{
    float step = radius / 5.0f; if (step < 10.0f) step = 10.0f;
    int   n    = 6;
    float y    = center[1];
    float x0 = center[0] - n*step, x1 = center[0] + n*step;
    float z0 = center[2] - n*step, z1 = center[2] + n*step;
    for (int i = -n; i <= n; i++) {
        float sx0,sy0,sx1,sy1;
        float a[3] = { center[0]+i*step, y, z0 }, b[3] = { center[0]+i*step, y, z1 };
        if (project(c,a,&sx0,&sy0) && project(c,b,&sx1,&sy1))
            draw_line(fb,(int)sx0,(int)sy0,(int)sx1,(int)sy1,IDX_GRID);
        float p[3] = { x0, y, center[2]+i*step }, q[3] = { x1, y, center[2]+i*step };
        if (project(c,p,&sx0,&sy0) && project(c,q,&sx1,&sy1))
            draw_line(fb,(int)sx0,(int)sy0,(int)sx1,(int)sy1,IDX_GRID);
    }
}

/* ----------------------------------------------------------------------- */
/* Terrain patch — downsampled height-mesh wireframe                         */
/* ----------------------------------------------------------------------- */

/* h is the 12-bit cell height (0..4095); band low/mid/high for the wireframe. */
static uint8_t terrain_band(int h)
{
    return (uint8_t)(IDX_TER0 + (h < 150 ? 0 : h < 400 ? 1 : 2));
}

/* Orientation hypotheses for how the stored 128x128 heightfield maps to world
 * (x,z) — used to test the within-patch alignment we couldn't confirm by xref. */
static int height_index(const TerrainPatch *p, int gx, int gz, int orient)
{
    int n = p->dim;
    switch (orient & 3) {
        default:
        case 0: return gz * n + gx;                  /* normal     */
        case 1: return (n-1-gz) * n + gx;            /* flip Z      */
        case 2: return gz * n + (n-1-gx);            /* flip X      */
        case 3: return gx * n + gz;                  /* transpose   */
    }
}
static const char *orient_name(int o) {
    static const char *nm[4] = {"normal","flipZ","flipX","transpose"};
    return nm[o & 3];
}

static void draw_terrain(uint8_t *fb, const Cam *c, const TerrainPatch *p,
                         float hscale, int orient)
{
    int n = p->dim, step = TERRAIN_STEP;
    /* one sampled row at a time + the previous row, to draw the grid mesh */
    static float rx[64], ry[64], px_[64], py_[64];
    static uint8_t rv[64], pv[64];
    int prev_valid = 0;

    for (int gz = 0; gz < n; gz += step) {
        int cols = 0;
        for (int gx = 0; gx < n; gx += step, cols++) {
            int h = p->heights[height_index(p, gx, gz, orient)];
            float w[3] = { p->origin[0] + gx * p->cell,
                           h * (0.1f * hscale),   /* 12-bit height * 0.1 (FUN_004953e0) */
                           p->origin[1] + gz * p->cell };
            float sx, sy;
            uint8_t vis = (uint8_t)project(c, w, &sx, &sy);
            rx[cols] = sx; ry[cols] = sy; rv[cols] = vis;
            uint8_t col = terrain_band(h);
            if (cols > 0 && vis && rv[cols-1])              /* edge to left */
                draw_line(fb, (int)rx[cols-1],(int)ry[cols-1], (int)sx,(int)sy, col);
            if (prev_valid && vis && pv[cols])              /* edge to prev row */
                draw_line(fb, (int)px_[cols],(int)py_[cols], (int)sx,(int)sy, col);
        }
        memcpy(px_, rx, sizeof(float)*cols);
        memcpy(py_, ry, sizeof(float)*cols);
        memcpy(pv,  rv, cols);
        prev_valid = 1;
    }
}

/* ----------------------------------------------------------------------- */
/* Draw one scene item                                                       */
/* ----------------------------------------------------------------------- */

static float s_sx[100000], s_sy[100000];
static uint8_t s_vis[100000];

static void draw_item(uint8_t *fb, const Cam *c, const SceneItem *it, uint8_t col)
{
    if (!it->mesh) {
        /* marker: a small upright cross at its world position */
        float s = 12.0f;
        float ax[3] = { it->t[0]-s, it->t[1], it->t[2] };
        float bx[3] = { it->t[0]+s, it->t[1], it->t[2] };
        float ay[3] = { it->t[0], it->t[1], it->t[2] };
        float by[3] = { it->t[0], it->t[1]+2*s, it->t[2] };
        float az[3] = { it->t[0], it->t[1], it->t[2]-s };
        float bz[3] = { it->t[0], it->t[1], it->t[2]+s };
        float p0,p1,q0,q1;
        if (project(c,ax,&p0,&p1)&&project(c,bx,&q0,&q1)) draw_line(fb,(int)p0,(int)p1,(int)q0,(int)q1,IDX_MARK);
        if (project(c,ay,&p0,&p1)&&project(c,by,&q0,&q1)) draw_line(fb,(int)p0,(int)p1,(int)q0,(int)q1,IDX_MARK);
        if (project(c,az,&p0,&p1)&&project(c,bz,&q0,&q1)) draw_line(fb,(int)p0,(int)p1,(int)q0,(int)q1,IDX_MARK);
        return;
    }

    const GeoMesh *m = it->mesh;
    int nv = m->num_verts;
    for (int i = 0; i < nv; i++) {
        const float *v = &m->verts[i*3];
        float w[3];
        for (int r = 0; r < 3; r++)
            w[r] = it->R[r*3+0]*v[0] + it->R[r*3+1]*v[1] + it->R[r*3+2]*v[2] + it->t[r];
        s_vis[i] = (uint8_t)project(c, w, &s_sx[i], &s_sy[i]);
    }
    for (int f = 0; f < m->num_faces; f++) {
        int first = m->face_first[f], n = m->face_count[f];
        for (int k = 0; k < n; k++) {
            int a = m->indices[first + k];
            int b = m->indices[first + (k+1) % n];
            if (a<0||a>=nv||b<0||b>=nv) continue;
            if (s_vis[a] && s_vis[b])
                draw_line(fb, (int)s_sx[a],(int)s_sy[a], (int)s_sx[b],(int)s_sy[b], col);
        }
    }
}

/* ----------------------------------------------------------------------- */

void sceneview_run(const char *mission_path)
{
    static uint8_t fb[FB_W * FB_H];
    Rgb8 pal[256];
    build_palette(pal);

    Scene *scene = scene_load_mission(mission_path);
    if (!scene) {
        fprintf(stderr, "[sceneview] failed to load %s\n", mission_path);
        return;
    }

    Font *font = font_load("base6x7.fnt");
    input_init();

    float az = 0.7f, el = 0.45f;
    float dist = scene->radius * 2.6f;
    if (dist < 50.0f) dist = 50.0f;
    bool spin = true;

    /* Vertical scale is known (height12 * 0.1, FUN_004953e0); HS is a debug
     * multiplier on top — 1.0 is exact, SPACE cycles it. */
    static const float HS[] = { 1.0f, 0.85f, 1.25f, 1.5f, 0.5f };
    int   hs_idx = 0;            /* -> 1.0 (exact) */
    int   orient = 0;           /* within-patch heightfield orientation */

    fprintf(stdout, "[sceneview] %d items. LEFT/RIGHT orbit, UP/DOWN zoom, "
                    "SPACE height-scale, P terrain-orient, ESC quit.\n", scene->count);

    /* Discard input buffered during the load so it doesn't fire actions. */
    platform_flush_events();

    uint64_t last = platform_get_ticks();
    while (1) {
        uint64_t fstart = platform_get_ticks();
        double dt = (double)(fstart - last) / 1000.0; last = fstart;
        if (dt > 0.1) dt = 0.1;

        InputState in; input_poll(&in);
        if (in.quit) break;
        if (in.pressed[INPUT_BTN_FIRE])
            hs_idx = (hs_idx + 1) % (int)(sizeof HS / sizeof HS[0]);   /* cycle height scale */
        if (in.pressed[INPUT_BTN_PAUSE]) orient = (orient + 1) & 3;     /* cycle orientation  */
        if (in.held[INPUT_BTN_LEFT])  { az -= (float)dt * 0.9f; spin = false; }
        if (in.held[INPUT_BTN_RIGHT]) { az += (float)dt * 0.9f; spin = false; }
        if (in.held[INPUT_BTN_UP])    dist *= (float)(1.0 - 1.2*dt);
        if (in.held[INPUT_BTN_DOWN])  dist *= (float)(1.0 + 1.2*dt);
        if (dist < scene->radius * 0.3f) dist = scene->radius * 0.3f;
        if (dist > scene->radius * 8.0f) dist = scene->radius * 8.0f;
        if (spin) az += (float)dt * 0.25f;

        Cam cam; cam_orbit(&cam, scene->center, az, el, dist);

        memset(fb, IDX_BG, sizeof fb);
        draw_grid(fb, &cam, scene->center, scene->radius);
        for (int i = 0; i < scene->patch_count; i++)
            draw_terrain(fb, &cam, &scene->patches[i], HS[hs_idx], orient);
        for (int i = 0; i < scene->count; i++) {
            const SceneItem *it = &scene->items[i];
            uint8_t col = (uint8_t)(IDX_W0 + (i % IDX_NW));
            draw_item(fb, &cam, it, col);
        }

        if (font) {
            char line[160];
            snprintf(line, sizeof(line),
                     "%s   %d obj  %d terrain   hscale %.2f   orient %s",
                     mission_path, scene->count, scene->patch_count,
                     HS[hs_idx], orient_name(orient));
            font_draw(font, line, fb, FB_W, FB_H, 8, 8, IDX_TEXT);
            font_draw(font, "LEFT/RIGHT ORBIT  UP/DOWN ZOOM  SPACE HSCALE  P ORIENT  ESC QUIT",
                      fb, FB_W, FB_H, 8, FB_H - 16, IDX_DIM);
        }

        render_begin_frame();
        render_set_palette(pal);
        render_blit_indexed(fb, FB_W, FB_H);
        render_end_frame();

        double el_ms = (double)(platform_get_ticks() - fstart);
        if (el_ms < TARGET_FRAME_MS) platform_sleep((uint32_t)(TARGET_FRAME_MS - el_ms));
    }

    scene_free(scene);
    if (font) font_free(font);
}
