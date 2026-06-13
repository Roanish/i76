#ifndef WORLD_H
#define WORLD_H

/*
 * world.h — The simulated world: the seam between the frame loop and the game.
 *
 * The gameplay loop (loop.c) drives the world through exactly four calls:
 *
 *   world_init()      once, when gameplay starts
 *   world_tick()      zero or more times per frame, each advancing the sim by
 *                     a FIXED timestep (deterministic — no wall-clock here)
 *   world_render()    once per frame, with an interpolation factor
 *   world_shutdown()  once, when gameplay ends
 *
 * WHY THIS SPLIT:
 *   Update runs on a fixed timestep so the simulation is deterministic and
 *   stable regardless of frame rate; rendering runs once per displayed frame
 *   and interpolates between the last two sim states (alpha) for smooth motion.
 *
 * NEXT STEP:
 *   This file is the home of the game-object system (the original's 2029-bucket
 *   g_obj_buckets entity store). Today world.c holds only a placeholder marker
 *   so the loop has something live to update and draw; that gets replaced by
 *   the real entity store and its per-tick update.
 */

#include <stdint.h>

#include "input.h"

void world_init(void);
void world_shutdown(void);

/*
 * Advance the simulation by exactly dt seconds (the loop's fixed step).
 * Called zero or more times per rendered frame. Must be deterministic:
 * read input, integrate state, no wall-clock or frame-rate dependence.
 */
void world_tick(const InputState *in, double dt);

/*
 * Draw the world into an 8-bit indexed framebuffer.
 *   fb        — w*h bytes, one palette index per pixel
 *   alpha     — interpolation factor in [0,1) between the previous and current
 *               sim state, for smooth rendering between fixed ticks
 */
void world_render(uint8_t *fb, int w, int h, double alpha);

#endif /* WORLD_H */
