#ifndef LOOP_H
#define LOOP_H

/*
 * loop.h — The gameplay frame loop.
 *
 * gameplay_run() is the engine heartbeat: it owns the screen while
 * g_gamestate == GS_GAMEPLAY and returns when the state changes (e.g. the
 * player quits). This mirrors the original binary's inner `while(state==5)`
 * loop in Fun_Real_Entry — each top-level game state runs its own inner loop
 * rather than sharing one outer pump.
 *
 * The loop runs a fixed-timestep simulation with interpolated rendering:
 * the world updates in deterministic FIXED_STEP increments, decoupled from
 * the (variable) display frame rate. See world.h for the update/render split.
 */

void gameplay_run(void);

#endif /* LOOP_H */
