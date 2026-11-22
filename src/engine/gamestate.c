/*
 * gamestate.c — global state variable definitions
 *
 * Just the storage for the externs declared in gamestate.h.
 * Logic lives in main.c and the individual state handlers.
 */

#include "gamestate.h"

/* Startup begins at the frontend. */
GameState g_gamestate = GS_FRONTEND;

/* 0 = clean exit. */
int g_exit_code = 0;
