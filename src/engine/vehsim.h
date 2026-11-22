#ifndef VEHSIM_H
#define VEHSIM_H

/*
 * vehsim.h — vehicle simulation, faithful to the original game.
 * Reference: docs/REVERSING.md "Vehicle physics".
 *
 * World coordinates are left-handed: +X right, +Y up, +Z forward. The body
 * frame is three row vectors (right, up, fwd) in world space.
 *
 * Vehicle data is read from the base archives only; physics is not moddable.
 */

#include <stdbool.h>

#include "engine/vehicle.h"

/* Wheel slots: 0/1 front, 2/3 middle (6-wheelers), 4/5 rear;
 * even = right, odd = left. */
enum { VS_WHEELS = 6 };

/* Ground under (x, z) for a body at height y: returns the height, writes the
 * surface normal and the surface type (0..7). */
typedef float (*VsGroundFn)(void *ud, float x, float y, float z, float n[3], int *surface);

typedef struct {
    VsGroundFn ground;
    void      *ud;
    float      grip[8], roll[8], bump[8];   /* per surface type (mission data) */
    int        effect[8];
} VsWorld;

/* Per-vehicle constants (vehsim_params_load). */
typedef struct {
    char  name[32];
    float mass;               /* body + engine + brakes + suspension + wheels */
    float drag;
    float engine_P;           /* torque curve peak                            */
    float brake;
    float susp_grip;
    float drive_scale;
    int   wheel_present[VS_WHEELS];
    float wheel_pos[VS_WHEELS][3];   /* wheel mounts, vehicle frame           */
    float wheel_mu[VS_WHEELS];
    float wheel_roll[VS_WHEELS];
    float box_min[3], box_max[3];    /* body box                              */
    float ign_time;           /* engine start delay                           */
    bool  player;             /* the player's grip model (AI is simpler)      */
} VsParams;

typedef struct { double a1, a2, g, y1, y2; } VsFilter;

typedef struct {
    float ground;        /* ground height under the wheel */
    float bump_h, bump_v;
    float offset;        /* suspension offset             */
    float rest_y;
    float radius;
    float lim_hi, lim_lo;
    int   bottomed;
    VsFilter filt;
    float steer;         /* visual steer angle            */
    float spin;          /* visual spin speed             */
} VsWheel;

typedef struct {
    int   gear;          /* 0 reverse, 1 idle, 2..4 = 1st..3rd */
    float torque;
    float health;
    float rpm;
    float speedo;
} VsEngine;

typedef struct {
    VsParams p;
    /* geometry derived at spawn */
    float track, wheelbase, steer_k, pivot_z, yaw_ease;
    float pivots[16][2];
    float box[4][3];
    float roof_y;
    /* state */
    double pos[3];
    float  axes[3][3];           /* rows: right, up, fwd */
    float  vel[3], omega[3], accel[3];
    float  speed, inv_speed;
    float  throttle, steer;
    float  dir;                  /* +1 forward, -1 reverse                  */
    int    ebrake;
    int    selector;             /* 0 P, 1 R, 2 N, 3 D, 4 "2", 5 "1"        */
    unsigned flags;
    int    surface;
    float  normal[3];
    float  g_n;                  /* gravity normal to the ground            */
    float  air_dy;
    float  last_ground;
    float  hold[4];              /* digital key hold times                  */
    float  start_t;              /* engine start countdown                  */
    VsWheel  w[VS_WHEELS];
    VsEngine e;
} VehSim;

/* Load a vehicle's constants from its .vcf and the files it names. Wheel
 * mounts come from the loaded VehicleModel. Returns false if a required file
 * is missing. */
bool vehsim_params_load(VsParams *p, const char *vcf_name, const VehicleModel *vm,
                        const float box_min[3], const float box_max[3]);

/* Spawn on the ground at (x, z), facing fwd (unit, horizontal). */
void vehsim_spawn(VehSim *v, const VsParams *p, const VsWorld *w,
                  double x, double z, const float fwd[3]);

/* Driver controls. Analog axes in [-1, 1]; digital keys ramp. */
typedef struct {
    float throttle_axis, steer_axis;     /* used when no digital key is held */
    bool  throttle_up, throttle_down, steer_left, steer_right;
    bool  ebrake, start_engine;
    bool  shift_up, shift_down, reverse; /* pressed this frame */
} VsControls;
void vehsim_controls(VehSim *v, const VsControls *c, float frame_dt);

/* Advance one frame (split into sub-steps of at most 1/20 s). */
void vehsim_frame(VehSim *v, const VsWorld *w, float frame_dt);

/* Orientation as a row-major 3x3 whose columns are the local axes. */
void vehsim_matrix(const VehSim *v, float R[9]);

#endif /* VEHSIM_H */
