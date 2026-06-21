/*
 * softraster.c — CPU triangle rasterizer (see softraster.h).
 *
 * Pipeline per triangle: model->clip via the cached MVP, clip against the
 * camera (w) plane, perspective divide + viewport, then an edge-function
 * (barycentric) scan with a per-pixel depth test. Flat lambert shading (abs
 * N·L, two-sided) selects an entry in the material's palette ramp.
 */

#include "softraster.h"

#include <math.h>
#include <string.h>

/* ----------------------------------------------------------------------- */
/* Small math                                                                */
/* ----------------------------------------------------------------------- */

static float s_vp[16];   /* proj * view, column-major */

static void mat4_mul(float out[16], const float a[16], const float b[16])
{
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++)
                s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
    memcpy(out, r, sizeof r);
}

/* out[4] = m(col-major) * v(x,y,z,1) */
static void mat4_pt(float out[4], const float m[16], const float v[3])
{
    for (int row = 0; row < 4; row++)
        out[row] = m[row]*v[0] + m[4+row]*v[1] + m[8+row]*v[2] + m[12+row];
}

void soft_set_viewproj(const float view[16], const float proj[16])
{
    mat4_mul(s_vp, proj, view);
}

void soft_clear(SoftTarget *t, uint8_t bg)
{
    memset(t->color, bg, (size_t)t->w * t->h);
    int n = t->w * t->h;
    for (int i = 0; i < n; i++) t->depth[i] = 1e30f;
}

/* ----------------------------------------------------------------------- */
/* Rasterize one (already screen-space) triangle                             */
/* ----------------------------------------------------------------------- */

/* Screen vertex: pixel x,y and ndc depth z (0..1). */
typedef struct { float x, y, z; } SVert;

static float edge(const SVert *a, const SVert *b, float px, float py)
{
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

static void raster_tri(SoftTarget *t, const SVert *v0, const SVert *v1,
                       const SVert *v2, uint8_t col)
{
    float area = edge(v0, v1, v2->x, v2->y);
    if (fabsf(area) < 1e-6f) return;
    float inv = 1.0f / area;

    int minx = (int)floorf(fminf(v0->x, fminf(v1->x, v2->x)));
    int maxx = (int)ceilf (fmaxf(v0->x, fmaxf(v1->x, v2->x)));
    int miny = (int)floorf(fminf(v0->y, fminf(v1->y, v2->y)));
    int maxy = (int)ceilf (fmaxf(v0->y, fmaxf(v1->y, v2->y)));
    if (minx < 0) minx = 0;
    if (maxx > t->w) maxx = t->w;
    if (miny < 0) miny = 0;
    if (maxy > t->h) maxy = t->h;

    for (int py = miny; py < maxy; py++) {
        float fy = py + 0.5f;
        for (int px = minx; px < maxx; px++) {
            float fx = px + 0.5f;
            float w0 = edge(v1, v2, fx, fy);
            float w1 = edge(v2, v0, fx, fy);
            float w2 = edge(v0, v1, fx, fy);
            /* Inside if all edges share the sign of the triangle area
             * (winding-agnostic — culling is intentionally off). */
            int pos = (w0 >= 0 && w1 >= 0 && w2 >= 0);
            int neg = (w0 <= 0 && w1 <= 0 && w2 <= 0);
            if (!pos && !neg) continue;

            float b0 = w0 * inv, b1 = w1 * inv, b2 = w2 * inv;
            float z  = b0 * v0->z + b1 * v1->z + b2 * v2->z;
            int   i  = py * t->w + px;
            if (z < t->depth[i]) {
                t->depth[i] = z;
                t->color[i] = col;
            }
        }
    }
}

/* ----------------------------------------------------------------------- */
/* Clip a triangle against the camera plane (clip.w >= EPS), fan-raster       */
/* ----------------------------------------------------------------------- */

#define W_EPS 1e-4f

static void emit_clipped(SoftTarget *t, float clip[3][4], uint8_t col)
{
    /* Sutherland-Hodgman against w >= W_EPS — up to 4 output vertices. */
    float out[4][4];
    int   nout = 0;
    for (int i = 0; i < 3; i++) {
        const float *a = clip[i];
        const float *b = clip[(i + 1) % 3];
        int ain = a[3] >= W_EPS, bin = b[3] >= W_EPS;
        if (ain) memcpy(out[nout++], a, sizeof(float) * 4);
        if (ain != bin) {
            float tt = (W_EPS - a[3]) / (b[3] - a[3]);
            for (int k = 0; k < 4; k++) out[nout][k] = a[k] + tt * (b[k] - a[k]);
            nout++;
        }
    }
    if (nout < 3) return;

    /* Perspective divide + viewport for each clipped vertex. */
    SVert sv[4];
    for (int i = 0; i < nout; i++) {
        float iw = 1.0f / out[i][3];
        float nx = out[i][0] * iw, ny = out[i][1] * iw, nz = out[i][2] * iw;
        sv[i].x = (nx * 0.5f + 0.5f) * t->w;
        sv[i].y = (ny * 0.5f + 0.5f) * t->h;
        sv[i].z = nz;
    }
    for (int i = 1; i + 1 < nout; i++)
        raster_tri(t, &sv[0], &sv[i], &sv[i + 1], col);
}

/* ----------------------------------------------------------------------- */

/* ----------------------------------------------------------------------- */
/* Textured triangle (perspective-correct UV, world-space in)                */
/* ----------------------------------------------------------------------- */

static float edgef(float ax, float ay, float bx, float by, float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* Screen vertex carrying perspective-correct attributes (u/w, v/w, 1/w). */
typedef struct { float x, y, z, uw, vw, iw; } SVTex;

static void raster_tri_tex(SoftTarget *t, const SVTex *a, const SVTex *b,
                           const SVTex *c, const uint8_t *tex, int tw, int th,
                           uint8_t flat)
{
    float area = edgef(a->x, a->y, b->x, b->y, c->x, c->y);
    if (fabsf(area) < 1e-6f) return;
    float inv = 1.0f / area;

    int minx = (int)floorf(fminf(a->x, fminf(b->x, c->x)));
    int maxx = (int)ceilf (fmaxf(a->x, fmaxf(b->x, c->x)));
    int miny = (int)floorf(fminf(a->y, fminf(b->y, c->y)));
    int maxy = (int)ceilf (fmaxf(a->y, fmaxf(b->y, c->y)));
    if (minx < 0) minx = 0;
    if (maxx > t->w) maxx = t->w;
    if (miny < 0) miny = 0;
    if (maxy > t->h) maxy = t->h;

    for (int py = miny; py < maxy; py++) {
        float fy = py + 0.5f;
        for (int px = minx; px < maxx; px++) {
            float fx = px + 0.5f;
            float wa = edgef(b->x, b->y, c->x, c->y, fx, fy);
            float wb = edgef(c->x, c->y, a->x, a->y, fx, fy);
            float wc = edgef(a->x, a->y, b->x, b->y, fx, fy);
            int pos = (wa >= 0 && wb >= 0 && wc >= 0);
            int neg = (wa <= 0 && wb <= 0 && wc <= 0);
            if (!pos && !neg) continue;

            float b0 = wa * inv, b1 = wb * inv, b2 = wc * inv;
            float z = b0 * a->z + b1 * b->z + b2 * c->z;
            int i = py * t->w + px;
            if (z >= t->depth[i]) continue;

            uint8_t col = flat;
            if (tex) {
                float iw = b0 * a->iw + b1 * b->iw + b2 * c->iw;
                float w  = (iw != 0.0f) ? 1.0f / iw : 0.0f;
                float u  = (b0 * a->uw + b1 * b->uw + b2 * c->uw) * w;
                float v  = (b0 * a->vw + b1 * b->vw + b2 * c->vw) * w;
                int iu = (int)floorf(u * tw) % tw; if (iu < 0) iu += tw;
                int iv = (int)floorf(v * th) % th; if (iv < 0) iv += th;
                col = tex[iv * tw + iu];
            }
            t->depth[i] = z;
            t->color[i] = col;
        }
    }
}

void soft_tri(SoftTarget *t, float wp[3][3], float uv[3][2],
              const uint8_t *tex, int tw, int th, uint8_t flat_idx)
{
    /* clip-space vertices + UV */
    struct { float clip[4], u, v; } in[3], out[4];
    for (int i = 0; i < 3; i++) {
        mat4_pt(in[i].clip, s_vp, wp[i]);
        in[i].u = uv ? uv[i][0] : 0.0f;
        in[i].v = uv ? uv[i][1] : 0.0f;
    }

    /* Sutherland-Hodgman against the camera plane (w >= W_EPS), carrying UV. */
    int nout = 0;
    for (int i = 0; i < 3; i++) {
        int j = (i + 1) % 3;
        int ain = in[i].clip[3] >= W_EPS, bin = in[j].clip[3] >= W_EPS;
        if (ain) out[nout++] = in[i];
        if (ain != bin) {
            float tt = (W_EPS - in[i].clip[3]) / (in[j].clip[3] - in[i].clip[3]);
            for (int k = 0; k < 4; k++)
                out[nout].clip[k] = in[i].clip[k] + tt * (in[j].clip[k] - in[i].clip[k]);
            out[nout].u = in[i].u + tt * (in[j].u - in[i].u);
            out[nout].v = in[i].v + tt * (in[j].v - in[i].v);
            nout++;
        }
    }
    if (nout < 3) return;

    SVTex sv[4];
    for (int i = 0; i < nout; i++) {
        float iw = 1.0f / out[i].clip[3];
        sv[i].x  = (out[i].clip[0] * iw * 0.5f + 0.5f) * t->w;
        sv[i].y  = (out[i].clip[1] * iw * 0.5f + 0.5f) * t->h;
        sv[i].z  = out[i].clip[2] * iw;
        sv[i].iw = iw;
        sv[i].uw = out[i].u * iw;
        sv[i].vw = out[i].v * iw;
    }
    for (int i = 1; i + 1 < nout; i++)
        raster_tri_tex(t, &sv[0], &sv[i], &sv[i + 1], tex, tw, th, flat_idx);
}

void soft_draw(SoftTarget *t, const float *verts, uint32_t vertex_count,
               const float model[16], uint8_t ramp_base, uint8_t levels)
{
    if (!verts || vertex_count < 3) return;

    float mvp[16];
    mat4_mul(mvp, s_vp, model);

    const float L[3] = { 0.35f, 0.85f, 0.40f };
    float ll = sqrtf(L[0]*L[0] + L[1]*L[1] + L[2]*L[2]);
    float Ln[3] = { L[0]/ll, L[1]/ll, L[2]/ll };

    for (uint32_t tri = 0; tri + 3 <= vertex_count; tri += 3) {
        const float *p0 = &verts[(tri + 0) * 6];
        const float *p1 = &verts[(tri + 1) * 6];
        const float *p2 = &verts[(tri + 2) * 6];

        /* Flat shade from the (constant per-tri) object-space normal, rotated
         * into world space by the model's upper-left 3x3. */
        const float *n = p0 + 3;
        float wn[3] = {
            model[0]*n[0] + model[4]*n[1] + model[8] *n[2],
            model[1]*n[0] + model[5]*n[1] + model[9] *n[2],
            model[2]*n[0] + model[6]*n[1] + model[10]*n[2],
        };
        float wl = sqrtf(wn[0]*wn[0] + wn[1]*wn[1] + wn[2]*wn[2]);
        float d  = 0.0f;
        if (wl > 1e-6f)
            d = fabsf((wn[0]*Ln[0] + wn[1]*Ln[1] + wn[2]*Ln[2]) / wl);
        int level = (int)(d * (levels - 1) + 0.5f);
        if (level < 0) level = 0;
        if (level > levels - 1) level = levels - 1;
        uint8_t col = (uint8_t)(ramp_base + level);

        float clip[3][4];
        mat4_pt(clip[0], mvp, p0);
        mat4_pt(clip[1], mvp, p1);
        mat4_pt(clip[2], mvp, p2);
        emit_clipped(t, clip, col);
    }
}
