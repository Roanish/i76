/*
 * trackcam.c — the original external track camera (see trackcam.h).
 *
 * The original composes row-vector matrices,
 * camera-local (0,0,-d) * Rx(pitch) * Ry(yaw) * R(quat) + lookat; here the
 * same chain is written column-vector (reverse order, transposed rotations).
 * Constants:
 *   pitch clamp [pi/180, pi/2], axis scale pi/180 per frame, dist clamp [1,10],
 *   dist cap 150, ease window s in [0, 0.95], snap gap 0.005,
 *   ground: test y-0.1 < h+0.5, set y = h+0.6.
 */

#include "engine/trackcam.h"

#include <math.h>
#include <string.h>

#define DEG   0.017453292f      /* pi/180 per axis unit per frame */
#define PITCH_MIN 0.017453292f
#define PITCH_MAX 1.5707963f
#define DIST_MIN  1.0f
#define DIST_MAX  10.0f
#define DIST_CAP  150.0f
#define EASE_MAX  0.95f
#define SNAP_GAP  0.005f
#define YAW_WRAP_TRIGGER 6283.1855f
#define TWO_PI    6.2831855f

/* ---- small quaternion helpers (x, y, z, w) ------------------------------ */

static void quat_from_mat(const float m[9], float q[4])
{
    /* m row-major, column-vector rotation (p' = m p). */
    float tr = m[0] + m[4] + m[8];
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (m[7] - m[5]) / s;
        q[1] = (m[2] - m[6]) / s;
        q[2] = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        float s = sqrtf(1.0f + m[0] - m[4] - m[8]) * 2.0f;
        q[3] = (m[7] - m[5]) / s;
        q[0] = 0.25f * s;
        q[1] = (m[1] + m[3]) / s;
        q[2] = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        float s = sqrtf(1.0f + m[4] - m[0] - m[8]) * 2.0f;
        q[3] = (m[2] - m[6]) / s;
        q[0] = (m[1] + m[3]) / s;
        q[1] = 0.25f * s;
        q[2] = (m[5] + m[7]) / s;
    } else {
        float s = sqrtf(1.0f + m[8] - m[0] - m[4]) * 2.0f;
        q[3] = (m[3] - m[1]) / s;
        q[0] = (m[2] + m[6]) / s;
        q[1] = (m[5] + m[7]) / s;
        q[2] = 0.25f * s;
    }
}

static void mat_from_quat(const float q[4], float m[9])
{
    /* Same form as the original's quaternion-to-matrix. */
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2*(y*y + z*z); m[1] = 2*(x*y - w*z);     m[2] = 2*(x*z + w*y);
    m[3] = 2*(x*y + w*z);     m[4] = 1 - 2*(x*x + z*z); m[5] = 2*(y*z - w*x);
    m[6] = 2*(x*z - w*y);     m[7] = 2*(y*z + w*x);     m[8] = 1 - 2*(x*x + y*y);
}

static void quat_slerp(const float a[4], const float b[4], float t, float o[4])
{
    float d = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    float wa, wb;
    if (d > 0.9995f) {                 /* nearly parallel: lerp */
        wa = 1.0f - t; wb = t;
    } else {
        if (d < -1.0f) d = -1.0f;
        float th = acosf(d), sn = sinf(th);
        wa = sinf((1.0f - t) * th) / sn;
        wb = sinf(t * th) / sn;
    }
    float l = 0;
    for (int i = 0; i < 4; i++) { o[i] = wa * a[i] + wb * b[i]; l += o[i] * o[i]; }
    l = sqrtf(l);
    if (l > 0) for (int i = 0; i < 4; i++) o[i] /= l;
}

static void mat3_mul(const float a[9], const float b[9], float o[9])
{
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            o[r*3+c] = a[r*3+0]*b[0*3+c] + a[r*3+1]*b[1*3+c] + a[r*3+2]*b[2*3+c];
}
static void mat3_vec(const float m[9], const float v[3], float o[3])
{
    for (int r = 0; r < 3; r++) o[r] = m[r*3+0]*v[0] + m[r*3+1]*v[1] + m[r*3+2]*v[2];
}

/* ------------------------------------------------------------------------- */

void trackcam_reset(TrackCam *c)
{
    memset(c, 0, sizeof *c);
    c->pitch_t = c->pitch = 0.5235988f;   /* 0x3f060a92, camera init */
    c->dist_t  = c->dist  = 5.5f;         /* 0x40b00000              */
}

/* Ease toward target by s, unless the step is out of range or tiny (snap). */
static float ease(float cur, float tgt, float s)
{
    if (s >= 0.0f && s <= EASE_MAX && fabsf(tgt - cur) >= SNAP_GAP)
        return cur + s * (tgt - cur);
    return tgt;
}

void trackcam_update(TrackCam *c, const float target_R[9], const float lookat[3],
                     float radius, int axis_pitch, int axis_yaw, int axis_dist,
                     float dt, TrackGroundFn ground, void *ud,
                     float eye[3], float fwd[3], float up[3])
{
    float s = dt + dt;

    /* Targets from the inputs. */
    c->pitch_t += (float)axis_pitch * DEG;
    if (c->pitch_t < PITCH_MIN) c->pitch_t = PITCH_MIN;
    if (c->pitch_t > PITCH_MAX) c->pitch_t = PITCH_MAX;
    c->yaw_t += (float)axis_yaw * DEG;
    if (fabsf(c->yaw_t) > YAW_WRAP_TRIGGER) {
        c->yaw_t = c->yaw - roundf(c->yaw / TWO_PI) * TWO_PI;
        if (c->yaw_t < 0.0f) c->yaw_t += TWO_PI;
    }
    c->dist_t = (float)axis_dist;
    if (c->dist_t < DIST_MIN) c->dist_t = DIST_MIN;
    if (c->dist_t > DIST_MAX) c->dist_t = DIST_MAX;

    c->dist  = ease(c->dist,  c->dist_t,  s);
    c->pitch = ease(c->pitch, c->pitch_t, s);
    c->yaw   = ease(c->yaw,   c->yaw_t,   s);

    /* Lagged orientation: slerp toward the target's, keeping the target quat
     * in the previous target's hemisphere. */
    float qt[4];
    quat_from_mat(target_R, qt);
    if (!c->init) {
        memcpy(c->q, qt, sizeof qt);
        memcpy(c->qprev, qt, sizeof qt);
        c->init = 1;
    }
    float hd = qt[0]*c->qprev[0] + qt[1]*c->qprev[1] + qt[2]*c->qprev[2] + qt[3]*c->qprev[3];
    if (hd < 0.0f) for (int i = 0; i < 4; i++) qt[i] = -qt[i];
    memcpy(c->qprev, qt, sizeof qt);
    float qn[4];
    quat_slerp(c->q, qt, s, qn);
    memcpy(c->q, qn, sizeof qn);

    /* R = R_lag * Ry(yaw) * Rx(pitch) (column-vector form of the original
     * row-vector chain Rx * Ry * R_lag). */
    float Rl[9];
    mat_from_quat(c->q, Rl);
    float cy = cosf(c->yaw), sy = sinf(c->yaw), cp = cosf(c->pitch), sp = sinf(c->pitch);
    float Ry[9] = { cy, 0, -sy,   0, 1, 0,   sy, 0, cy };
    float Rx[9] = { 1, 0, 0,   0, cp, -sp,   0, sp, cp };
    float A[9], R[9];
    mat3_mul(Rl, Ry, A);
    mat3_mul(A, Rx, R);

    float d = c->dist * radius;
    if (d > DIST_CAP) d = DIST_CAP;
    float back[3] = { 0, 0, -d }, off[3];
    mat3_vec(R, back, off);
    for (int k = 0; k < 3; k++) eye[k] = lookat[k] + off[k];

    float z[3] = { 0, 0, 1 }, y[3] = { 0, 1, 0 };
    mat3_vec(R, z, fwd);
    mat3_vec(R, y, up);

    /* Ground clearance under the camera point only (no line-of-sight test). */
    if (ground) {
        float h = ground(ud, eye[0], eye[2]);
        if (eye[1] - 0.1f < h + 0.5f) eye[1] = h + 0.6f;
    }
}
