#ifndef GAMESTATE_H
#define GAMESTATE_H

/*
 * gamestate.h — the game's top-level state machine.
 * Reference: docs/REVERSING.md "Game State Machine".
 */

typedef enum {
    GS_LOBBY        = 0,    /* network lobby setup */
    GS_FRONTEND     = 1,    /* return to frontend / menus */
    GS_MAINMENU     = 2,    /* main menu */
    GS_ENDSESSION   = 3,    /* multiplayer session ended (score limit) */
    GS_GAMEPLAY     = 5,    /* in-game — the hot loop */
    GS_SHELL        = 6,    /* shell (NITSHELL ShellMain) is running */
    GS_MISSIONEND   = 7,    /* mission complete / game over */
    GS_MP_FOLLOWON  = 8,    /* multiplayer follow-on scenario */
    GS_LOBBY_ALT    = 0xb,  /* alternate lobby */
    GS_QUIT         = 0xff, /* exit application */
} GameState;

/* Current state; checked at the top of the outer game loop. */
extern GameState g_gamestate;

/* Process exit code, set on quit. */
extern int g_exit_code;

#endif /* GAMESTATE_H */
