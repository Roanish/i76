/*
 * main.c — Application entry point and outer game loop
 *
 * Mirrors Fun_Real_Entry (WinMain at 0x00431760) from the original binary.
 *
 * Startup sequence implemented so far:
 *   cmdline_parse()        cmdline equivalent
 *   fs_set_root()          asset path from --assets or "."
 *   vfs_init()             parse nitro.zix, lazy-open ZFS sources
 *   srand(time(NULL))      confirmed in Fun_Real_Entry
 *   platform_init()        RegisterClassA + CreateWindowA + ShowWindow
 *   render_init()          LoadLibraryA("NITSHELL.DLL") + Init(surface,mode)
 *   pool alloc             FUN_004a3c00 × 2
 *   pcx load screen        addon/loadgame.pcx
 *
 * Still stubbed:
 *   StrLookup / lang.txt   (StrLookupCreate)
 *   Single-instance guard  (FindWindowA)
 *   Smacker intro/credits  (introf01.smk, credf01.smk) — non-goal for now
 *   Audio init             (audio_load_dat / engsnd.dat)
 *   The actual session runner (FUN_00430eb0)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "platform/platform.h"
#include "engine/gamestate.h"
#include "engine/cmdline.h"
#include "engine/fs.h"
#include "engine/vfs.h"
#include "engine/strlookup.h"
#include "engine/pcx.h"
#include "engine/video.h"
#include "engine/loop.h"
#include "engine/meshcache.h"
#include "engine/meshview.h"
#include "render/render.h"

/* -----------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */

#define WINDOW_TITLE   "Vigalante '76"
#define WINDOW_WIDTH   640
#define WINDOW_HEIGHT  480

/* Asset paths — original used backslashes; we use forward slashes.
 * Place the game's addon/ directory alongside the binary. */
#define PCX_LOADGAME   "addon/loadgame.pcx"

/* -----------------------------------------------------------------------
 * Forward declarations — state handlers
 * ----------------------------------------------------------------------- */

static void state_frontend_update(void);

/* -----------------------------------------------------------------------
 * Entry point
 * ----------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    fprintf(stdout, "Interstate '76 Nitro Pack — open source rewrite\n");
    fprintf(stdout, "------------------------------------------------\n");

    /* -------------------------------------------------------------- */
    /* Command line                                                     */
    /* -------------------------------------------------------------- */

    CmdlineArgs args;
    cmdline_parse(argc, argv, &args);

    /* --meshview: dev tool — browse decoded OEG meshes instead of running the
     * game. Handled here (not in cmdline.c) since it's a developer entry point. */
    bool mesh_view = false;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--meshview") == 0) mesh_view = true;

    /* -------------------------------------------------------------- */
    /* Asset root                                                       */
    /* Mirrors the original's assumption that assets live alongside the */
    /* executable. Use --assets <path> to override.                     */
    /* -------------------------------------------------------------- */

    fs_set_root(args.asset_root);
    fprintf(stdout, "[main] Asset root: %s\n", args.asset_root);

    if (!vfs_init())
        fprintf(stderr, "[main] VFS init failed — ZFS assets unavailable.\n");

    /* -------------------------------------------------------------- */
    /* Mesh cache                                                       */
    /* Original: obj_system_init() during session setup. Set up the     */
    /* GEO mesh cache (dual index + LRU + heap budget) now; meshes are   */
    /* loaded lazily on first geo_cache_acquire().                       */
    /* -------------------------------------------------------------- */

    meshcache_init();
#ifndef NDEBUG
    meshcache_selftest();
#endif

    /* -------------------------------------------------------------- */
    /* String table                                                     */
    /* Original: StrLookupCreate("lang.txt") — absent in GOG release,  */
    /* so sl will be NULL throughout; callers handle NULL gracefully.   */
    /* -------------------------------------------------------------- */

    StrLookup *sl = StrLookupCreate("lang.txt");
    if (!sl)
        fprintf(stdout, "[main] lang.txt not found — using hardcoded strings.\n");

    /* -------------------------------------------------------------- */
    /* RNG seed                                                         */
    /* Confirmed in Fun_Real_Entry: srand(time(NULL)) before init.     */
    /* -------------------------------------------------------------- */

    srand((unsigned int)time(NULL));

    /* -------------------------------------------------------------- */
    /* Platform init (window)                                           */
    /* -------------------------------------------------------------- */

    if (!platform_init(WINDOW_TITLE, WINDOW_WIDTH, WINDOW_HEIGHT)) {
        fprintf(stderr, "[main] Platform init failed.\n");
        return 1;
    }

    /* -------------------------------------------------------------- */
    /* Render init (Vulkan)                                             */
    /* Mirrors: LoadLibraryA("NITSHELL.DLL") + Init(surface, mode)    */
    /* -------------------------------------------------------------- */

    if (!render_init()) {
        fprintf(stderr, "[main] Render init failed.\n");
        platform_shutdown();
        return 1;
    }

    /* -------------------------------------------------------------- */
    /* Virtual memory pools                                             */
    /*   DAT_00653fa0 = FUN_004a3c00(0x1a5e0, 0x40000) primary pool   */
    /*   DAT_00653fa4 = FUN_004a3c00(0x80000, 0x80000) secondary pool  */
    /* -------------------------------------------------------------- */

    void *pool_primary   = platform_valloc(0x40000);
    void *pool_secondary = platform_valloc(0x80000);
    if (!pool_primary || !pool_secondary) {
        fprintf(stderr, "[main] Unable to allocate virtual memory pools.\n");
        render_shutdown();
        platform_shutdown();
        return 1;
    }

    /* -------------------------------------------------------------- */
    /* Intro + credits videos                                           */
    /* Original: video_play(hwnd, "introf01.smk") then                 */
    /*           video_play(hwnd, "credf01.smk") before main loop.     */
    /* Any keypress skips. Not found = silently continue.              */
    /* -------------------------------------------------------------- */

    if (!mesh_view) {
        video_play("smk/introf01.smk");
        video_play("smk/credf01.smk");
    }

    /* -------------------------------------------------------------- */
    /* Loading screen                                                   */
    /* Original: FUN_00471000(path, dest_surface) after valloc.        */
    /* -------------------------------------------------------------- */

    {
        PcxImage *loading = pcx_load(PCX_LOADGAME);
        if (loading) {
            render_begin_frame();
            render_set_palette((const Rgb8 *)loading->palette);
            render_blit_indexed(loading->pixels, loading->width, loading->height);
            render_end_frame();
            pcx_free(loading);
        } else {
            fprintf(stdout, "[main] %s not found — skipping loading screen.\n",
                    PCX_LOADGAME);
        }
    }

    /* -------------------------------------------------------------- */
    /* Outer game loop                                                  */
    /* Original: do { local_3c = FUN_00430eb0(...); ... } while(true)  */
    /*                                                                  */
    /* Each top-level state owns its own inner frame loop (mirroring    */
    /* the original, where every game state ran its own while() with    */
    /* its own message pump). The dispatcher just hands control to the  */
    /* active state's runner and loops until something sets GS_QUIT.    */
    /* -------------------------------------------------------------- */

    if (mesh_view) {
        /* Developer entry point: browse decoded OEG meshes, then exit. */
        fprintf(stdout, "[main] --meshview: entering mesh viewer.\n");
        meshview_run();
    } else {
        fprintf(stdout, "[main] Entering game loop. ESC or close to quit.\n");

        while (g_gamestate != GS_QUIT) {
            switch (g_gamestate) {
                case GS_FRONTEND:
                    state_frontend_update();   /* transitions, returns immediately */
                    break;

                case GS_GAMEPLAY:
                    gameplay_run();            /* owns the screen until state changes */
                    break;

                default:
                    /* States we haven't built yet (menus, lobby, session end). */
                    fprintf(stdout, "[main] state %d not implemented — quitting.\n",
                            (int)g_gamestate);
                    g_gamestate = GS_QUIT;
                    break;
            }
        }
    }

    /* -------------------------------------------------------------- */
    /* Shutdown                                                         */
    /* -------------------------------------------------------------- */

    platform_vfree(pool_secondary, 0x80000);
    platform_vfree(pool_primary,   0x40000);

    StrLookupDestroy(sl);
    meshcache_shutdown();
    vfs_shutdown();
    render_shutdown();
    platform_shutdown();

    fprintf(stdout, "[main] Exiting with code %d\n", g_exit_code);
    return g_exit_code;
}

/* -----------------------------------------------------------------------
 * State handlers
 * ----------------------------------------------------------------------- */

static void state_frontend_update(void)
{
    /*
     * Stub: bypass NITSHELL entirely and drop straight into gameplay.
     * In the original, GS_FRONTEND causes Fun_Real_Entry's outer loop
     * to call game_session_run() which runs ShellMain for the menu.
     * We skip all that and pretend the player already picked a scenario.
     *
     * TODO: replace with real session runner once engine is further along.
     */
    fprintf(stdout, "[frontend] Skipping shell — jumping straight to gameplay.\n");
    g_gamestate = GS_GAMEPLAY;
}
