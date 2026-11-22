#ifndef TRACKCAM_H
#define TRACKCAM_H

/*
 * trackcam.h — the original's external "track" camera (view 0x0f, the
 * default external view), reproduced.
 * See docs/REVERSING.md "Camera views + the external (track) camera".
 *
 * Per frame, with s = 2*dt:
 *   pitch target += axis_pitch * (pi/180), clamped [1 deg, 90 deg]
 *   yaw   target += axis_yaw   * (pi/180)
 *   dist  target  = axis_dist, clamped [1, 10]            (x target radius)
 *   current pitch/yaw/dist ease toward the targets by s (snap if the step is
 *   out of [0, 0.95] or the gap < 0.005)
 *   lagged orientation = slerp(lagged, target orientation, s)
 *   eye = lookat + R_lag * Ry(yaw) * Rx(pitch) * (0, 0, -min(dist*radius, 150))
 *   ground: if eye.y - 0.1 < ground(eye.x, eye.z) + 0.5, eye.y = ground + 0.6
 */

typedef float (*TrackGroundFn)(void *ud, float x, float z);

typedef struct {
    float pitch_t, yaw_t, dist_t;    /* targets                        */
    float pitch, yaw, dist;          /* smoothed                       */
    float q[4];                      /* lagged target orientation      */
    float qprev[4];                  /* last target quat (hemisphere)  */
    int   init;
} TrackCam;

/* Reset to the original's camera-init defaults (pitch pi/6, dist 5.5). */
void trackcam_reset(TrackCam *c);

/*
 * target_R: the target's orientation, row-major 3x3 whose COLUMNS are its
 *           local axes in world space (p_world = R * p_local); local +Z =
 *           forward.
 * lookat:   world-space look-at point (the target's bounding-box centre).
 * radius:   the target's bounding radius.
 * axis_*:   the track_pitch_delta / track_yaw_delta / track_distance inputs.
 * Outputs eye position, forward and up (world, unit).
 */
void trackcam_update(TrackCam *c, const float target_R[9], const float lookat[3],
                     float radius, int axis_pitch, int axis_yaw, int axis_dist,
                     float dt, TrackGroundFn ground, void *ud,
                     float eye[3], float fwd[3], float up[3]);

#endif /* TRACKCAM_H */
