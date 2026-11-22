/*
 * vehsim.c — vehicle simulation, faithful to the original game.
 * Reference: docs/REVERSING.md "Vehicle physics".
 */

#include "engine/vehsim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/vfs.h"

#define PI_F 3.14159265f

/* ---- small vector helpers ------------------------------------------------ */

static float dot3(const float a[3], const float b[3]) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
static void  cross3(const float a[3], const float b[3], float o[3])
{
    float x = a[1]*b[2] - a[2]*b[1], y = a[2]*b[0] - a[0]*b[2], z = a[0]*b[1] - a[1]*b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
static float len3(const float a[3]) { return sqrtf(dot3(a, a)); }

/* Move toward tgt by fraction s; snap when s is out of [0, 0.95] or the gap
 * is below 0.005. */
static float ease(float cur, float tgt, float s)
{
    if (s >= 0.0f && s <= 0.95f && fabsf(tgt - cur) >= 0.005f) return cur + s * (tgt - cur);
    return tgt;
}

/* Body point (local) -> world, using the row axes. */
static void body_to_world(const VehSim *v, const float l[3], float o[3])
{
    for (int k = 0; k < 3; k++)
        o[k] = (float)v->pos[k] + l[0]*v->axes[0][k] + l[1]*v->axes[1][k] + l[2]*v->axes[2][k];
}

/* Rotate the body axes by -w dt (R = Rz Rx Ry, row-vector form). */
static void orient(float ax[3][3], const float w[3], float dt)
{
    float a = -dt * w[0], b = -dt * w[1], c = -dt * w[2];
    float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b), cc = cosf(c), sc = sinf(c);
    float Ry[3][3] = { { cb, 0, -sb }, { 0, 1, 0 }, { sb, 0, cb } };
    float Rx[3][3] = { { 1, 0, 0 }, { 0, ca, sa }, { 0, -sa, ca } };
    float Rz[3][3] = { { cc, sc, 0 }, { -sc, cc, 0 }, { 0, 0, 1 } };
    float T[3][3], R[3][3], N[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
        T[i][j] = Rx[i][0]*Ry[0][j] + Rx[i][1]*Ry[1][j] + Rx[i][2]*Ry[2][j];
    }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
        R[i][j] = Rz[i][0]*T[0][j] + Rz[i][1]*T[1][j] + Rz[i][2]*T[2][j];
    }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
        N[i][j] = R[i][0]*ax[0][j] + R[i][1]*ax[1][j] + R[i][2]*ax[2][j];
    }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) ax[i][j] = N[i][j];
}

/* ---- suspension filter (second-order, unit DC gain) ---------------------- */

static void filt_init(VsFilter *f, float r, float period)
{
    if (period < 4.0f) period = 4.0f;
    f->a1 = (double)(cosf(6.2831855f / period) * r * -2.0f);
    f->a2 = (double)r * (double)r;
    f->y1 = f->y2 = 0.0;
    double y = 0.0;
    for (int i = 0; i < 200; i++) {          /* unit step response */
        double y2 = f->y2;
        f->y2 = f->y1;
        y = (1.0 - f->y1 * f->a1) - y2 * f->a2;
        f->y1 = y;
    }
    f->g = (double)(1.0f / (float)y);
    f->y1 = f->y2 = 0.0;
}
static float filt_step(VsFilter *f, float x)
{
    double y2 = f->y2;
    f->y2 = f->y1;
    double y = ((double)x * f->g - f->a1 * f->y1) - f->a2 * y2;
    f->y1 = y;
    return (float)y;
}

/* ---- data loading -------------------------------------------------------- */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static float rdf(const uint8_t *p) { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }

/* First chunk with `tag` in [off, end) of a BWD2 chunk stream (tag, len). */
static long chunk(const uint8_t *d, long off, long end, const char *tag)
{
    while (off + 8 <= end) {
        uint32_t len = rd32(d + off + 4);
        if (memcmp(d + off, tag, 4) == 0) return off;
        if (len < 8) return -1;
        off += len;
    }
    return -1;
}

/* engsnd.dat maps an engine number to its compnent.cdf ENGN record; also
 * returns the engine's normal ignition time. */
static int engine_record(int engine_num, float *ign_time)
{
    size_t sz = 0;
    char *d = vfs_read_file("engsnd.dat", &sz);
    if (!d) return -1;
    int rec = -1;
    size_t o = 0;
    while (o < sz) {
        char line[256]; size_t k = 0;
        while (o < sz && d[o] != '\n' && k < sizeof line - 1) line[k++] = d[o++];
        line[k] = 0;
        if (o < sz) o++;
        if (line[0] == '#' || line[0] == 0) continue;
        int num, comp, rev; char esnd[32], horn[32], ign[32]; float t;
        if (sscanf(line, "%d %d %d %31s %31s %31s %f", &num, &comp, &rev, esnd, horn, ign, &t) >= 7
            && num == engine_num) { rec = comp; if (ign_time) *ign_time = t; break; }
    }
    vfs_free(d);
    return rec;
}

/* compnent.cdf: payload of the n-th ENGN / BRAK / SUSP record. */
static const uint8_t *cdf_record(const uint8_t *d, size_t sz, const char *tag, int n)
{
    for (long o = 8; o + 8 <= (long)sz; ) {
        uint32_t len = rd32(d + o + 4);
        if (len < 8) break;
        if (memcmp(d + o, tag, 4) == 0 && n-- == 0) return d + o + 8;
        o += len;
    }
    return NULL;
}

bool vehsim_params_load(VsParams *p, const char *vcf_name, const VehicleModel *vm,
                        const float box_min[3], const float box_max[3])
{
    memset(p, 0, sizeof *p);
    p->brake = 1.0f;            /* not set by any data file */
    p->drive_scale = 1.0f;
    p->susp_grip = 1.0f;
    p->player = true;
    memcpy(p->box_min, box_min, sizeof p->box_min);
    memcpy(p->box_max, box_max, sizeof p->box_max);

    size_t sz = 0;
    uint8_t *d = vfs_read_file(vcf_name, &sz);
    if (!d) { fprintf(stderr, "[vehsim] cannot open %s\n", vcf_name); return false; }
    long c = chunk(d, 8, (long)sz, "VCFC");
    if (c < 0 || c + 0x65 > (long)sz) { vfs_free(d); return false; }
    char vdf[14] = {0}, wdf[3][14];
    memcpy(vdf, d + c + 0x18, 13);
    int eng_num = (int)rd32(d + c + 0x32), susp_n = (int)rd32(d + c + 0x36), brake_n = (int)rd32(d + c + 0x3a);
    for (int a = 0; a < 3; a++) { memcpy(wdf[a], d + c + 0x3e + a * 13, 13); wdf[a][13] = 0; }
    vfs_free(d);

    /* VDFC: name, base mass, drag. */
    d = vfs_read_file(vdf, &sz);
    if (!d) { fprintf(stderr, "[vehsim] cannot open %s\n", vdf); return false; }
    c = chunk(d, 8, (long)sz, "VDFC");
    if (c < 0) { vfs_free(d); return false; }
    const uint8_t *vp = d + c + 8;
    memcpy(p->name, vp, 20); p->name[20] = 0;
    p->mass = rdf(vp + 0x30);
    p->drag = rdf(vp + 0x38);
    vfs_free(d);

    /* Engine, brake and suspension records add their weights to the mass. */
    float ign = 1.0f;
    int erec = engine_record(eng_num, &ign);
    p->ign_time = ign;
    d = vfs_read_file("compnent.cdf", &sz);
    if (!d) { fprintf(stderr, "[vehsim] cannot open compnent.cdf\n"); return false; }
    const uint8_t *r;
    if ((r = cdf_record(d, sz, "ENGN", erec)) != NULL) { p->engine_P = rdf(r + 4); p->mass += rdf(r + 8); }
    if ((r = cdf_record(d, sz, "BRAK", brake_n)) != NULL) { p->mass += rdf(r + 8); }
    if ((r = cdf_record(d, sz, "SUSP", susp_n)) != NULL) { p->susp_grip = rdf(r + 4); p->mass += rdf(r + 12); }
    vfs_free(d);
    if (p->engine_P <= 0.0f) p->engine_P = 300000.0f;   /* engine default */

    /* Wheels: grip and weight per axle (two wheels each). */
    for (int a = 0; a < 3; a++) {
        if (strncmp(wdf[a], "null", 4) == 0 || !wdf[a][0]) continue;
        d = vfs_read_file(wdf[a], &sz);
        if (!d) continue;
        c = chunk(d, 8, (long)sz, "WDFC");
        float mu = 1.0f, wt = 0.0f;
        if (c >= 0) { mu = rdf(d + c + 8 + 0x30); wt = rdf(d + c + 8 + 0x2c); }
        vfs_free(d);
        p->mass += wt + wt;
        for (int k = a * 2; k < a * 2 + 2; k++) {
            p->wheel_present[k] = vm ? vm->wloc_present[k] : 0;
            p->wheel_mu[k] = mu;
            p->wheel_roll[k] = 1.0f;
            if (vm) memcpy(p->wheel_pos[k], vm->wloc_t[k], sizeof p->wheel_pos[k]);
        }
    }
    fprintf(stderr, "[vehsim] %s: mass %.0f drag %.4g engine %d->rec %d P %.0f susp grip %.2f\n",
            p->name, p->mass, p->drag, eng_num, erec, p->engine_P, p->susp_grip);
    return true;
}

/* ---- spawn ----------------------------------------------------------------- */

static void ground_at(const VsWorld *w, float x, float y, float z, float *h, float n[3], int *surf)
{
    float nn[3] = { 0, 1, 0 }; int s = 2;
    *h = w->ground ? w->ground(w->ud, x, y, z, nn, &s) : 0.0f;
    if (n) memcpy(n, nn, sizeof nn);
    if (surf) *surf = s;
}

static float surf_grip(const VehSim *v, const VsWorld *w)
{
    float g = w->grip[v->surface & 7];
    if (g > 0.2f && (v->flags & 0x400)) g = 0.0f;
    return g;
}

static void wheel_world(const VehSim *v, int k, float o[3])
{
    float l[3] = { v->p.wheel_pos[k][0], v->w[k].rest_y + v->w[k].offset, v->p.wheel_pos[k][2] };
    body_to_world(v, l, o);
}

void vehsim_spawn(VehSim *v, const VsParams *p, const VsWorld *w,
                  double x, double z, const float fwd[3])
{
    memset(v, 0, sizeof *v);
    v->p = *p;
    float h; int s;
    ground_at(w, (float)x, 1e6f, (float)z, &h, v->normal, &s);
    v->pos[0] = x; v->pos[1] = h; v->pos[2] = z;
    float up[3] = { 0, 1, 0 }, right[3];
    cross3(up, fwd, right);
    float rl = len3(right);
    for (int k = 0; k < 3; k++) right[k] /= rl;
    memcpy(v->axes[0], right, sizeof right);
    memcpy(v->axes[1], up, sizeof up);
    cross3(right, up, v->axes[2]);
    v->dir = 1.0f;
    v->selector = 3;                       /* D */
    v->flags = 0x40 | 0x1001;
    if (p->player) v->flags |= 0x10000;
    v->g_n = 9.8f;
    v->surface = s;

    /* Wheels: radius and rest height from the mount; travel +-radius/4. */
    for (int k = 0; k < VS_WHEELS; k++) {
        VsWheel *wh = &v->w[k];
        wh->rest_y = wh->radius = p->wheel_present[k] ? p->wheel_pos[k][1] : 0.566f;
        wh->lim_hi = wh->radius * 0.25f;
        wh->lim_lo = wh->radius * -0.25f;
        filt_init(&wh->filt, 0.7f, 15.0f);
        if (p->wheel_present[k]) {
            float wp[3]; wheel_world(v, k, wp);
            float gh; ground_at(w, wp[0], wp[1], wp[2], &gh, NULL, NULL);
            wh->ground = wh->bump_h = gh;
        }
    }

    /* Track and wheelbase from the wheel mounts. */
    float dx = p->wheel_pos[1][0] - p->wheel_pos[0][0], dz = p->wheel_pos[1][2] - p->wheel_pos[0][2];
    float dy = p->wheel_pos[1][1] - p->wheel_pos[0][1];
    v->track = sqrtf(dx*dx + dy*dy + dz*dz);
    dx = p->wheel_pos[1][0] - p->wheel_pos[5][0]; dy = p->wheel_pos[1][1] - p->wheel_pos[5][1];
    dz = p->wheel_pos[1][2] - p->wheel_pos[5][2];
    v->wheelbase = sqrtf(dx*dx + dy*dy + dz*dz);
    if (v->wheelbase < 0.01f) v->wheelbase = 0.01f;
    v->steer_k  = 1.0f / (v->wheelbase + v->wheelbase);
    v->pivot_z  = p->wheel_pos[5][2];                    /* rear axle */
    v->yaw_ease = 2.65618f / v->wheelbase;

    /* Body box corners at mid height; roof height for upside-down contact. */
    float ym = (p->box_min[1] + p->box_max[1]) * 0.5f;
    float bx[4][3] = { { p->box_min[0], ym, p->box_min[2] }, { p->box_max[0], ym, p->box_min[2] },
                       { p->box_max[0], ym, p->box_max[2] }, { p->box_min[0], ym, p->box_max[2] } };
    memcpy(v->box, bx, sizeof bx);
    v->roof_y = p->box_max[1];

    /* Tilt pivot for each combination of wheels off the ground. */
    float L2 = v->wheelbase * 0.5f, T2 = v->track * 0.5f;
    static const signed char tab[16][2] = {
        {0,0},{0,0},{0,0},{0,1},{0,0},{-1,0},{0,0},{-1,1},
        {0,0},{0,0},{1,0},{1,1},{0,-1},{-1,-1},{1,-1},{0,0} };
    for (int m = 0; m < 16; m++) { v->pivots[m][0] = tab[m][0] * T2; v->pivots[m][1] = tab[m][1] * L2; }

    /* Engine idling in first. */
    v->e.gear = 1; v->e.health = 1.0f; v->e.rpm = 1050.0f;
}

/* ---- controls -------------------------------------------------------------- */

void vehsim_controls(VehSim *v, const VsControls *c, float dt)
{
    bool tu = c->throttle_up, td = c->throttle_down, sl = c->steer_left, sr = c->steer_right;
    if (!td && !tu && !sr && !sl) {
        v->throttle = c->throttle_axis;
        v->steer    = c->steer_axis;
        for (int i = 0; i < 4; i++) v->hold[i] = 0.0f;
    } else {
        /* Digital keys ramp per frame. */
        if (!tu) v->hold[0] = 0.0f;
        else {
            v->hold[0] += dt; if (v->hold[0] > 3.0f) v->hold[0] = 3.0f;
            v->throttle -= (1.0f - v->hold[0] * (1.0f / 3.0f)) * -0.4f;
        }
        if (!td) v->hold[1] = 0.0f;
        else {
            v->hold[1] += dt; if (v->hold[1] > 3.0f) v->hold[1] = 3.0f;
            v->throttle -= (1.0f - v->hold[1] * (1.0f / 3.0f)) * 0.5f;
        }
        if (!sr) v->hold[2] = 0.0f;
        else {
            v->hold[2] += dt; if (v->hold[2] > 3.0f) v->hold[2] = 3.0f;
            v->steer = sqrtf(v->hold[2] * (1.0f / 3.0f)) * -1.0f;
        }
        if (!sl) v->hold[3] = 0.0f;
        else {
            v->hold[3] += dt; if (v->hold[3] > 3.0f) v->hold[3] = 3.0f;
            v->steer = sqrtf(v->hold[3] * (1.0f / 3.0f));
        }
        if (!td && !tu) v->throttle = 0.0f;
        if (!sl && !sr) v->steer = 0.0f;
    }
    if (v->throttle > 0.9999f) v->throttle = 0.9999f;
    if (v->throttle < -0.9999f) v->throttle = -0.9999f;
    if (v->steer > 0.9999f) v->steer = 0.9999f;
    if (v->steer < -0.9999f) v->steer = -0.9999f;
    v->ebrake = c->ebrake;
    if (c->start_engine) {            /* toggles the engine */
        if (v->flags & 1) v->flags &= ~1u;
        else if (!(v->flags & 9)) { v->flags |= 8; v->start_t = v->p.ign_time; }
    }
    if (c->shift_up && v->selector < 5) v->selector++;
    if (c->shift_down && v->selector > 0) v->selector--;
    v->dir = (v->selector == 1) ? -1.0f : 1.0f;
    if (c->reverse) {                 /* reverse_direction: D <-> R */
        v->dir = -v->dir;
        v->selector = (v->dir < 0.0f) ? 1 : 3;
    }
}

/* ---- engine and automatic gearbox ----------------------------------------- */

static const float GEAR_RATIO[5] = { 3.0f, 0.0f, 1.67f, 0.96f, 0.67f };   /* R, idle, 1-3 */

static int shift_window(float T, float kmh, float t0, float k0, float t1, float k1)
{
    if (t0 <= T && T <= t1 && k0 <= kmh && kmh <= k1)
        return (((t1 - t0) * (kmh - k1)) / (k1 - k0)) * t1 < T ? 1 : 0;
    return -1;
}

static int downshift(float T, float speed, int g, int sel)
{
    float kmh = fabsf(speed * 3.6f);
    if (sel == 5 && g > 2) return 2;
    if (sel == 4 && g > 3) return 3;
    if (g == 2) { if (T < 0.002f && kmh < 0.5f) return 1; }
    else if (g == 3) {
        if ((0.875f < T && kmh < 60.0f) ||
            (0.3125f <= T && T <= 0.875f && 12.0f <= kmh && kmh <= 60.0f &&
             (kmh - 60.0f) * 0.5625f * 0.02083333f * 0.875f < T) ||
            kmh < 12.0f) return 2;
    } else if (g == 4) {
        if ((0.87f < T && kmh < 98.0f) ||
            (0.25f <= T && T <= 0.875f && 31.0f <= kmh && kmh <= 98.0f &&
             (kmh - 98.0f) * 0.625f * 0.01492537f * 0.875f < T) ||
            kmh < 31.0f) return 3;
    }
    return g;
}

/* Uses the frame dt, not the sub-step dt. */
static void engine_update(VehSim *v, float frame_dt)
{
    VsEngine *E = &v->e;
    float T = v->throttle, speed = v->speed;   /* only |speed| is used */
    int   sel = v->selector, rev = v->dir < 0.0f;
    float sp = fabsf(v->speed);
    float w  = sp * 0.5973836f * 60.0f;
    float rpm, speedo = sp;
    int g;
    if (!rev) { g = E->gear; if (g == 0) { E->gear = 1; g = 1; } }
    else g = 0;
    if (sel == 2 || sel == 0) { g = 1; E->gear = 1; }
    float s2 = frame_dt + frame_dt;

    if (!(v->flags & 1)) {
        rpm = ease(E->rpm, 0.0f, s2);
    } else if (!(v->flags & 6) && sel != 2 && sel != 0) {
        int ng = 0;
        if (g != 0) {
            float kmh = fabsf(speed * 3.6f);
            g = E->gear;
            if (g == 1) { ng = (T <= 0.002f) ? 1 : 2; }
            else if (g == 2) {
                int stay = sel == 5 ||
                    (((0.0625f <= T) || (kmh <= 25.0f)) &&
                     ((T < 0.0625f) || (0.875f < T) || (kmh < 25.0f) || (62.0f < kmh) ||
                      ((kmh - 62.0f) * 0.8125f * 0.02702703f * 0.875f < T)) &&
                     kmh <= 62.0f);
                ng = stay ? 2 : 3;
            } else if (g != 3 || sel == 5 || sel == 4 ||
                       (((0.0625f <= T) || (kmh <= 40.0f)) &&
                        shift_window(T, kmh, 0.0625f, 40.0f, 0.875f, 106.0f) != 0 &&
                        kmh <= 106.0f)) {
                ng = g;
            } else ng = 4;
            ng = downshift(T, speed, ng, sel);
        }
        E->gear = ng;
        rpm = (ng != 1) ? GEAR_RATIO[ng] * w * 3.538f + 850.0f : 1050.0f;
        speedo = w * 1.673966f * 0.01666667f * 2.236936f;
        E->speedo = speedo;
        E->rpm = rpm;
        goto torque;
    } else if (T >= 0.002f) {
        rpm = ease(E->rpm, T * 4950.0f + 1050.0f, s2);
        if (rpm > 6000.0f) rpm = 6000.0f;
    } else {
        rpm = ease(E->rpm, 1050.0f, s2);
    }
    E->speedo = ease(E->speedo, speedo, s2);
    E->rpm = rpm;
torque:
    {
        float d = E->rpm - 3500.0f;
        float k = v->p.engine_P * 8.163266e-08f;
        E->torque = (v->p.engine_P - k * d * d) * E->health;
    }
}

/* ---- drive / brake force ---------------------------------------------------- */

static float drive_force(VehSim *v, const VsWorld *w, float T, int flipped, const float fwd[3],
                         unsigned flags, int *slide)
{
    if (T > 0.0f && (!(flags & 1) || v->selector == 2)) return 0.0f;
    float s = v->speed;
    float gn = v->g_n * 0.25f * surf_grip(v, w);
    float vf = dot3(v->vel, fwd);
    int back = vf < 0.0f, skid;
    if (!flipped) skid = flags & 2; else { T = -1.0f; skid = 1; }
    float f = 0.0f;
    if (T > -1e-4f) {
        if (T >= 1e-4f) {
            float lim = (v->p.wheel_mu[4] + v->p.wheel_mu[5]) * gn;        /* rear wheels */
            float tq = !skid ? v->e.torque : v->e.health * v->p.engine_P;
            f = tq * (1.0f / v->p.mass) * v->p.drive_scale;
            if (flags & 0x800) f *= 5.0f;
            if (s > 1.0f) f *= v->inv_speed;
            if (f < 1e-4f) f = 1e-4f;
            f *= T;
            if (lim < f) { f = lim; *slide = 1; }
            f *= v->dir;
        }
    } else {
        float lim = -(v->p.wheel_mu[0] + v->p.wheel_mu[1] + v->p.wheel_mu[4] + v->p.wheel_mu[5]) * gn;
        if (s > 1.0f) {
            f = v->p.brake * T * 8.0f;
            if (skid) { lim = v->inv_speed * vf * lim; if (back) lim = -lim; }
            if (v->ebrake) lim *= 0.5f;
            if (!flipped) { if (f < lim) { f = lim; *slide = 1; } }
            else f = lim + lim;
            if (back) return -f;
        }
    }
    return f;
}

/* ---- steering --------------------------------------------------------------- */

static float steering(VehSim *v, const VsWorld *w, float s_signed, int *slide, float *grip_frac)
{
    float st = v->steer;
    float mu = v->p.wheel_mu[0] + v->p.wheel_mu[1] + v->p.wheel_mu[4] + v->p.wheel_mu[5];
    float G  = v->p.susp_grip * v->g_n * surf_grip(v, w) * mu * 0.25f;
    float Gl = (v->flags & 0x10000) ? G : mu * 7.84f;
    float G2 = G + G;
    if (st > 1.0f) st = 1.0f;
    if (st < -1.0f) st = -1.0f;
    float r;
    if (s_signed * s_signed * v->steer_k <= Gl) r = st * s_signed * v->steer_k;
    else r = (st * Gl) / s_signed;
    float a = fabsf(r * s_signed);
    if (a <= G2) {
        if (G < a) *grip_frac = (a - G2) / (G - G2);
        return r;
    }
    *slide = 1;
    return r;
}

/* ---- ground dynamics -------------------------------------------------------- */

/* Returns the step flags; updates pos/axes/vel/omega/accel/speed. */
static unsigned ground_step(VehSim *v, const VsWorld *w, float dt)
{
    unsigned flags = v->flags;
    float *right = v->axes[0], *up = v->axes[1], *fwd = v->axes[2];
    float zero[3] = { 0, 0, 0 };
    memcpy(v->accel, zero, sizeof zero);

    if ((flags & 4) || !(flags & 0x200)) {
        /* Ballistic prediction to test for leaving the ground. */
        float p[3] = { (float)v->pos[0], (float)v->pos[1], (float)v->pos[2] };
        float vv[3] = { v->vel[0], v->vel[1], v->vel[2] };
        p[0] += vv[0] * dt;
        p[2] += vv[2] * dt;
        p[1] += (vv[1] - dt * -9.8f * -0.5f) * dt;
        vv[1] += dt * -9.8f;
        float h; ground_at(w, p[0], p[1], p[2], &h, NULL, NULL);
        float py = p[1];
        if (up[1] <= 0.0f) py = p[1] + up[1] * v->roof_y;   /* roof when upside down */
        float d = py - h;
        if (((flags & 4) && v->air_dy < d) || 0.5f < d) {
            v->air_dy = d;
            for (int k = 0; k < 3; k++) v->pos[k] = p[k];
            memcpy(v->vel, vv, sizeof vv);
            v->speed = len3(vv);
            if (-0.15707964f < v->omega[0]) v->omega[0] = ease(v->omega[0], -0.15707964f, dt);
            orient(v->axes, v->omega, dt);
            return (flags & 0xffffddfdu) | 4;
        }
        v->air_dy = d;
        if (flags & 4) {                        /* landing */
            v->pos[0] = p[0]; v->pos[1] = h; v->pos[2] = p[2];
            memcpy(v->vel, vv, sizeof vv);
            v->speed = len3(vv);
            flags = (flags & ~4u) | 0x200;
            float lat = dot3(right, vv);
            if (v->p.player && !(-1.0f <= lat && lat <= 1.0f)) flags |= 2;
            return flags;
        }
    }

    float *n = v->normal;
    float T = v->throttle;
    int flipped;
    if (0.1f <= dot3(up, n)) {
        flipped = 0;
        if (!(v->ebrake == 0 && v->selector != 0)) T = -1.0f;
    } else { flipped = 1; T = -1.0f; }
    if (-1e-4f < T && v->selector == 2) T = 0.0f;
    float vel0[3] = { v->vel[0], v->vel[1], v->vel[2] };
    float s_signed = v->speed;
    if (dot3(fwd, vel0) < 0.0f) s_signed = -s_signed;

    float brake_blend = 1.0f;
    int slide = 0, skid;
    if (-1.0f <= s_signed * v->dir || T <= 1e-4f) {
        if (v->ebrake == 0 || v->speed <= 2.0f) skid = flags & 2;
        else { slide = 1; skid = 1; if (!(flags & 2)) brake_blend = s_signed * 0.1f; }
    } else { skid = 1; slide = 1; flags |= 2; }

    float a_drive = drive_force(v, w, T, flipped, fwd, flags, &slide);
    if (slide) skid = 1;
    float grip_frac = 1.0f, yaw = 0.0f;
    flags &= ~0x2000u;
    if (!skid && !flipped) {
        yaw = steering(v, w, s_signed, &slide, &grip_frac);
        if (slide) skid = 1;
    }

    /* Drag + rolling resistance along v (s >= 1). */
    float a[3] = { 0, 0, 0 };
    float gq = v->g_n * 0.25f;
    if (v->speed >= 1.0f) {
        float roll = v->p.wheel_roll[1] + v->p.wheel_roll[0] + v->p.wheel_roll[5] + v->p.wheel_roll[4];
        float k = (v->p.drag * v->speed * v->speed * -0.1f - w->roll[v->surface & 7] * roll * gq)
                  * v->inv_speed;
        for (int i = 0; i < 3; i++) a[i] = v->vel[i] * k;
    }
    /* Gravity component along the ground. */
    float gdotn = (-9.8f * n[1]) / dot3(n, n);
    float g_t[3] = { -gdotn * n[0], -9.8f - gdotn * n[1], -gdotn * n[2] };
    float gq2 = gq * surf_grip(v, w);
    float G_tot = (v->p.wheel_mu[4] + v->p.wheel_mu[5] + v->p.wheel_mu[1] + v->p.wheel_mu[0]) * gq2;
    int slid = skid;
    if (dot3(g_t, g_t) > G_tot * G_tot) { slide = 1; slid = 1; }
    float add[3];
    if (flipped || (slid && v->speed >= 1.0f)) {
        memcpy(add, g_t, sizeof add);
        /* Sliding: resist the sideways part of the velocity. */
        float f = -(dot3(v->vel, right) * v->inv_speed * G_tot);
        for (int i = 0; i < 3; i++) a[i] += right[i] * f;
    } else {
        float gf = dot3(g_t, fwd);
        for (int i = 0; i < 3; i++) add[i] = fwd[i] * gf;
    }
    if (slide) skid = 1;

    double pos[3] = { v->pos[0], v->pos[1], v->pos[2] };
    double pos0[3] = { pos[0], pos[1], pos[2] };
    float vn[3];
    float half = dt * dt * 0.5f;
    if (!skid) {
        float f;
        if (1.0f <= grip_frac) { for (int i = 0; i < 3; i++) vn[i] = fwd[i] * s_signed; f = yaw; }
        else { memcpy(vn, v->vel, sizeof vn); flags |= 0x2000; f = grip_frac * yaw; }
        f = -(f * s_signed);
        for (int i = 0; i < 3; i++) a[i] = right[i] * f + a[i];
        if (T < 0.0f) for (int i = 0; i < 3; i++) a[i] = fwd[i] * a_drive + a[i];
        /* pos += v dt + a dt^2 / 2, v += a dt */
        for (int i = 0; i < 3; i++) {
            pos[i] = (double)((vn[i] - a[i] * dt * -0.5f) * dt + (float)pos[i]);
            vn[i] += a[i] * dt;
        }
        if (dot3(vn, vel0) < 0.0f) { vn[0] = vn[1] = vn[2] = 0; memcpy(pos, pos0, sizeof pos); }
        if (T > 0.0f) for (int i = 0; i < 3; i++) add[i] = fwd[i] * a_drive + add[i];
        for (int i = 0; i < 3; i++) {
            pos[i] = (double)(half * add[i] + (float)pos[i]);
            vn[i] += add[i] * dt;
            v->accel[i] = add[i] + a[i];
        }
    } else {
        flags |= 2;
        memcpy(vn, v->vel, sizeof vn);
        for (int i = 0; i < 3; i++) {
            pos[i] = (double)((vn[i] - a[i] * dt * -0.5f) * dt + (float)pos[i]);
            vn[i] += a[i] * dt;
        }
        if (dot3(vn, vel0) < 0.0f) { vn[0] = vn[1] = vn[2] = 0; memcpy(pos, pos0, sizeof pos); }
        for (int i = 0; i < 3; i++) {
            float ad = fwd[i] * a_drive + add[i];
            pos[i] = (double)(half * ad + (float)pos[i]);
            vn[i] += ad * dt;
            v->accel[i] = ad + a[i];
        }
        /* Sliding: yaw rate from the steering, eased. */
        yaw = v->steer * v->dir * PI_F;
        if (v->speed < 1.0f) yaw *= v->speed;
        float k = v->yaw_ease * dt;
        if (0.0f <= k && k <= 0.95f && fabsf(yaw - v->omega[1]) >= 0.005f)
            yaw = k * (yaw - v->omega[1]) + v->omega[1];
        yaw *= brake_blend;
    }

    /* Yaw about the rear axle. */
    double pivot[3];
    for (int i = 0; i < 3; i++) pivot[i] = pos[i] + (double)(v->pivot_z * fwd[i]);
    float th = -(yaw * dt), c = cosf(th), sn = sinf(th);
    float nr[3], nf[3];
    for (int i = 0; i < 3; i++) { nr[i] = c * right[i] - sn * fwd[i]; nf[i] = sn * right[i] + c * fwd[i]; }
    memcpy(v->axes[0], nr, sizeof nr);
    memcpy(v->axes[2], nf, sizeof nf);
    for (int i = 0; i < 3; i++) v->pos[i] = pivot[i] + (double)(-v->pivot_z) * (double)nf[i];

    /* Keep the velocity in the ground plane: (n x v) x n. */
    float t1[3], vp[3];
    cross3(n, vn, t1);
    cross3(t1, n, vp);
    float sp = len3(vp);
    if (dot3(nf, vp) < 0.0f) sp = -sp;
    if (T < 0.0f && -1.0f < sp && sp < 1.0f) {
        vp[0] = vp[1] = vp[2] = 0; sp = 0;
        flags &= ~0x2002u; skid = 0;
    }
    if (skid && !slide) {
        float lat = dot3(v->axes[0], vp);
        if (-3.0f < lat && lat < 3.0f) flags &= ~0x2002u;
    }
    memcpy(v->vel, vp, sizeof vp);
    v->speed = sp;
    v->omega[1] = yaw;
    return flags;
}

/* ---- suspension per wheel --------------------------------------------------- */

static const float BUMP[64] = {   /* ground bump pattern, 8 x 8 */
    0.1f,0,0.1f,0,0,0.1f,0,0.1f, 0,0,0,0,0.1f,0,0,0, 0,0,0.1f,0,0,0.1f,0.1f,0, 0.1f,0,0,0.1f,0,0,0,0,
    0,0,0.1f,0,0,0.1f,0,0.1f, 0,0.1f,0,0,0.1f,0,0,0, 0,0,0,0,0,0,0.1f,0, 0,0.1f,0,0.1f,0,0,0,0.1f };

static void suspension(VehSim *v, const VsWorld *w, int k, int air, float input)
{
    VsWheel *wh = &v->w[k];
    float wp[3]; wheel_world(v, k, wp);
    float h; ground_at(w, wp[0], wp[1], wp[2], &h, NULL, NULL);
    wh->ground = h;
    float bump = w->bump[v->surface & 7];
    float bh = bump * BUMP[((int)wp[0] & 7) + ((int)wp[2] & 7) * 8] + h;
    float dv = bh - wh->bump_h;
    float x = air ? wh->lim_lo : input - (dv - wh->bump_v) * -5.0f;
    wh->bottomed = air;
    if (x < wh->lim_lo) wh->bottomed = 1;
    wh->bump_h = bh;
    wh->bump_v = dv;
    float y = filt_step(&wh->filt, x);
    if (y < wh->lim_lo) y = wh->lim_lo;
    else if (y > wh->lim_hi) y = wh->lim_hi;
    wh->offset = y;
}

/* ---- settle and tilt --------------------------------------------------------- */

static void post_step(VehSim *v, const VsWorld *w, unsigned keep, float dt)
{
    float *right = v->axes[0], *up = v->axes[1], *fwd = v->axes[2];
    /* weight transfer: -0.008 x longitudinal / lateral accel */
    float af = dot3(v->accel, fwd) / dot3(fwd, fwd);
    float along[3] = { af * fwd[0], af * fwd[1], af * fwd[2] };
    float perp[3] = { v->accel[0] - along[0], v->accel[1] - along[1], v->accel[2] - along[2] };
    float lon = dot3(along, fwd) * -0.008f;
    float lat = dot3(perp, right) * -0.008f;
    /* Order: front-left, front-right, rear-left, rear-right. */
    suspension(v, w, 1, 0, lon - lat);
    suspension(v, w, 0, 0, lat + lon);
    suspension(v, w, 5, 0, -lon - lat);
    suspension(v, w, 4, 0, lat - lon);
    if (v->p.wheel_present[3]) suspension(v, w, 3, 0, 0.0f);
    if (v->p.wheel_present[2]) suspension(v, w, 2, 0, 0.0f);

    static const int W[4] = { 1, 0, 5, 4 };
    float bot[4];
    if (up[1] <= 0.1f) {          /* on its side / roof: body box corners */
        static const int C[4] = { 3, 2, 0, 1 };
        for (int i = 0; i < 4; i++) {
            float l[3] = { v->box[C[i]][0], v->roof_y, v->box[C[i]][2] }, o[3];
            body_to_world(v, l, o); bot[i] = o[1];
        }
    } else {
        for (int i = 0; i < 4; i++) {
            float o[3]; wheel_world(v, W[i], o); bot[i] = o[1] - v->w[W[i]].radius;
        }
    }
    float lift = v->w[W[0]].ground - bot[0];
    for (int i = 1; i < 4; i++) if (lift < v->w[W[i]].ground - bot[i]) lift = v->w[W[i]].ground - bot[i];

    float pitch = 0.0f, roll = 0.0f; int mask = 0;
    if (1e-4f < (bot[0] + lift) - v->w[W[0]].ground) { roll -= PI_F * 0.5f; pitch = -PI_F; mask = 8; }
    if (1e-4f < (bot[1] + lift) - v->w[W[1]].ground) { pitch -= PI_F; roll += PI_F * 0.5f; mask |= 4; }
    if (1e-4f < (bot[2] + lift) - v->w[W[2]].ground) { pitch += PI_F; roll -= PI_F * 0.5f; mask |= 2; }
    if (1e-4f < (bot[3] + lift) - v->w[W[3]].ground) { pitch += PI_F; roll += PI_F * 0.5f; mask |= 1; }
    if (up[1] <= 0.1f) { pitch = -pitch; roll = -roll; }
    float s2 = dt + dt;
    pitch = ease(v->omega[0], pitch, s2);
    roll  = ease(v->omega[2], roll, s2);
    for (int i = 0; i < 3; i++) v->pos[i] += (double)(lift * v->normal[i]);

    float ax2[3][3]; double pos2[3] = { v->pos[0], v->pos[1], v->pos[2] };
    memcpy(ax2, v->axes, sizeof ax2);
    if (pitch * pitch >= 1e-8f || roll * roll >= 1e-8f) {
        float px = v->pivots[mask][0], pz = v->pivots[mask][1];
        double P[3];
        for (int i = 0; i < 3; i++) P[i] = v->pos[i] + (double)(px * right[i] + pz * fwd[i]);
        float om[3] = { pitch, 0.0f, roll };
        orient(ax2, om, dt);
        for (int i = 0; i < 3; i++) pos2[i] = P[i] - (double)(px * ax2[0][i] + pz * ax2[2][i]);
    }

    /* Contact plane through front-right, rear middle and front-left. */
    float c[4][3];
    VehSim tmp = *v;
    memcpy(tmp.axes, ax2, sizeof ax2);
    memcpy(tmp.pos, pos2, sizeof pos2);
    for (int i = 0; i < 4; i++) {
        float o[3]; wheel_world(&tmp, W[i], o);
        c[i][0] = o[0]; c[i][1] = v->w[W[i]].ground - v->w[W[i]].offset; c[i][2] = o[2];
    }
    float mid[3] = { (c[2][0] + c[3][0]) * 0.5f, (c[2][1] + c[3][1]) * 0.5f, (c[2][2] + c[3][2]) * 0.5f };
    const float *p2 = c[1], *p3 = mid, *p4 = c[0];
    float nx = (p4[2]-p2[2])*(p3[1]-p2[1]) - (p4[1]-p2[1])*(p3[2]-p2[2]);
    float ny = (p4[0]-p2[0])*(p3[2]-p2[2]) - (p4[2]-p2[2])*(p3[0]-p2[0]);
    float nz = (p4[1]-p2[1])*(p3[0]-p2[0]) - (p4[0]-p2[0])*(p3[1]-p2[1]);
    float nl = sqrtf(nx*nx + ny*ny + nz*nz);
    float nw[3] = { 0, 1, 0 };
    if (nl > 0.0f) { nw[0] = nx / nl; nw[1] = ny / nl; nw[2] = nz / nl; }

    if (keep || dot3(up, nw) <= dot3(up, ax2[1])) {      /* keep the tilt */
        v->omega[0] = pitch; v->omega[2] = roll;
        memcpy(v->axes, ax2, sizeof ax2);
        memcpy(v->pos, pos2, sizeof pos2);
        return;
    }
    /* Snap to the wheel plane, keeping the heading. */
    float f[3] = { fwd[0], fwd[1], fwd[2] };
    if (1e-4f < nw[1]) f[1] = -((nw[2] * f[2] + nw[0] * f[0]) / nw[1]);
    float r[3]; cross3(nw, f, r);
    float rl = len3(r);
    if (rl > 0.0f) for (int i = 0; i < 3; i++) r[i] /= rl;
    memcpy(v->axes[0], r, sizeof r);
    memcpy(v->axes[1], nw, sizeof nw);
    cross3(r, nw, v->axes[2]);
    v->omega[0] = v->omega[2] = 0.0f;
}

/* ---- one physics step -------------------------------------------------------- */

static void step(VehSim *v, const VsWorld *w, float dt, float frame_dt)
{
    /* Engine start: runs once the ignition time has elapsed. */
    if (v->flags & 8) {
        v->start_t -= dt;
        if (v->start_t <= 0.0f) { v->start_t = 0.0f; v->flags = (v->flags & ~8u) | 1u; }
    }

    /* Upside down at rest: the player's car is flipped upright. */
    float ud = dot3(v->axes[1], v->normal);
    if (fabsf(v->omega[0]) < 0.62831855f && fabsf(v->omega[2]) < 0.62831855f && fabsf(ud) > 0.8f &&
        !(v->flags & 0x24) && ud < 0.0f) {
        if (v->flags & 0x10000) {
            for (int k = 0; k < 3; k++) { v->axes[1][k] = -v->axes[1][k]; v->axes[0][k] = -v->axes[0][k]; }
        } else { v->omega[0] = v->omega[1] = v->omega[2] = 0.0f; }
    }
    v->flags &= ~0x200u;

    if (v->flags & 0x8020) v->throttle = -1.0f;
    v->w[0].steer = v->w[1].steer = v->steer * -0.7853982f;

    unsigned old = v->flags;
    unsigned nf = ground_step(v, w, dt);
    float sp = fabsf(v->speed);
    v->speed = sp;
    v->inv_speed = sp > 1e-4f ? 1.0f / sp : 1e30f;

    if (!(nf & 4)) {
        v->flags = nf;
        for (int k = 0; k < VS_WHEELS; k++) v->w[k].spin = v->speed;
        float q[3] = { (float)v->pos[0], (float)v->pos[1], (float)v->pos[2] };
        if (v->axes[1][1] < 0.0f) for (int k = 0; k < 3; k++) q[k] += v->axes[1][k] * v->roof_y;
        float h; int s;
        ground_at(w, q[0], q[1], q[2], &h, v->normal, &s);
        v->surface = s;
        v->g_n = v->normal[1] * v->normal[1] * v->normal[1] * 9.8f;
        if (!(v->flags & 4)) v->last_ground = h;
        post_step(v, w, old & 0x202, dt);
    } else {
        v->flags = nf;
        for (int k = 0; k < VS_WHEELS; k++) if (v->p.wheel_present[k]) suspension(v, w, k, 1, 0.0f);
    }
    engine_update(v, frame_dt);
}

void vehsim_frame(VehSim *v, const VsWorld *w, float frame_dt)
{
    if (frame_dt <= 0.0f) return;
    int n = (int)(frame_dt * 20.0f) + 1;       /* sub-steps of at most 1/20 s */
    if (n > 20) n = 20;
    float dt = frame_dt / (float)n;
    for (int i = 0; i < n; i++) step(v, w, dt, frame_dt);
}

void vehsim_matrix(const VehSim *v, float R[9])
{
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) R[r * 3 + c] = v->axes[c][r];
}
