/*
 * gamestate.c — global state variable definitions
 *
 * Just the storage for the externs declared in gamestate.h.
 * Logic lives in main.c and the individual state handlers.
 */

#include "gamestate.h"

/*
 * Start in GS_GAMEPLAY to mirror the original:
 *   DAT_004f30cc = 5;  (set at 0x00431dcc in Fun_Real_Entry,
 *                       just before the outer do-while loop)
 *
 * In practice the real startup sequence goes through init,
 * videos, and loading before hitting the loop — we'll add
 * those phases back as we implement them. For now, drop
 * straight into gameplay so there's something to test against.
 */
GameState g_gamestate = GS_FRONTEND;

/*
 * Exit code — 0 is clean exit.
 * Original DAT_00526a44 defaulted to 0 and was only changed
 * by WM_QUIT wParam or explicit error paths.
 */
int g_exit_code = 0;
