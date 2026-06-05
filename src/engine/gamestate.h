#ifndef GAMESTATE_H
#define GAMESTATE_H

/*
 * gamestate.h — Game state machine definitions
 *
 * ORIGINAL BINARY:
 *   The state variable is DAT_004f30cc, a global int.
 *   It's set to 5 at startup (just before the main do-while loop)
 *   and checked/mutated throughout Fun_Real_Entry and game code.
 *
 *   Observed values and their meanings, reverse engineered from
 *   Fun_Real_Entry control flow:
 *
 *   DAT_004f30cc = 5   Set at 0x00431dcc before the main loop.
 *                      The inner while(DAT_004f30cc == 5) is the
 *                      gameplay loop itself.
 *
 *   DAT_004f30cc = 1   "Return to frontend" — triggers a video mode
 *                      reset to 0 and plays the outro SMK video
 *                      (DAT_00653f90). Reloads vehicle scene file
 *                      (vehscn.vsf). Multiplayer variant copies
 *                      a savegame path.
 *
 *   DAT_004f30cc = 0   Lobby / network state. Runs FUN_0049e900()
 *                      which returns 0 if setup fails → forces state 2.
 *                      Inner loop runs while state == 0.
 *
 *   DAT_004f30cc = 2   Main menu (inferred — state 0 falls through to
 *                      it on FUN_0049e900 failure, and it's the "give up"
 *                      state for lobby).
 *
 *   DAT_004f30cc = 3   End-of-session score limit reached. Set when
 *                      kill/points/caps/laps condition triggers and
 *                      10-second grace timer elapses.
 *
 *   DAT_004f30cc = 7   Mission complete / game over. Triggers path
 *                      that checks DAT_00653b20 (value 2, 3, or 4)
 *                      and copies a path into DAT_00653ff0. Then
 *                      resets to state 5.
 *
 *   DAT_004f30cc = 8   Multiplayer follow-on state (after state 7
 *                      when scenario starts with "mp" prefix,
 *                      checked via _strnicmp at 0x004337xx).
 *
 *   DAT_004f30cc = 0xb  Alias for lobby (treated same as 0 in the
 *                       else-if branch). Set when DAT_00534080 != 0
 *                       and state is 1.
 *
 *   local_3c == -1 (0xff)  Quit path — NOT stored in DAT_004f30cc.
 *                          local_3c is the return value of
 *                          FUN_00430eb0() (the session runner).
 *                          -1 means "exit the application entirely".
 *
 *   DAT_00526a44  The final exit/return code passed to exit().
 *                 Set from WM_QUIT wParam or defaulting to 0.
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
    GS_LOBBY_ALT    = 0xb,  /* alternate lobby (set when DAT_00534080!=0) */
    GS_QUIT         = 0xff, /* exit application */
} GameState;

/*
 * g_gamestate — global state variable.
 * Mirrors DAT_004f30cc in the original binary.
 * Written from multiple places; always check it at the top of the
 * outer game loop.
 */
extern GameState g_gamestate;

/*
 * g_exit_code — final process exit code.
 * Mirrors DAT_00526a44 in the original binary.
 * Set from WM_QUIT wParam in the original; we set it from
 * SDL_QUIT or normal exit paths.
 */
extern int g_exit_code;

#endif /* GAMESTATE_H */
