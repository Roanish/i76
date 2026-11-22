/*
 * main.c — Application entry point and outer game loop
 *
 * Follows the original's WinMain startup order (docs/REVERSING.md "Entry Points").
 *
 * Startup sequence implemented so far:
 *   cmdline_parse()        cmdline equivalent
 *   fs_set_root()          asset path from --assets or "."
 *   vfs_init()             parse nitro.zix, lazy-open ZFS sources
 *   srand(time(NULL))
 *   platform_init()        window
 *   render_init()          renderer
 *   pool alloc             two memory pools
 *   pcx load screen        addon/loadgame.pcx
 *
 * Still stubbed:
 *   String table / lang.txt
 *   Single-instance guard
 *   Audio init (engsnd.dat)
 *   The session runner
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "platform/platform.h"
#include "engine/fidelity.h"
#include "engine/gamestate.h"
#include "engine/cmdline.h"
#include "engine/fs.h"
#include "engine/vfs.h"
#include "engine/pix.h"
#include "engine/mods.h"
#include "engine/strlookup.h"
#include "engine/pcx.h"
#include "engine/video.h"
#include "engine/loop.h"
#include "engine/meshcache.h"
#include "engine/meshview.h"
#include "engine/sceneview.h"
#include "engine/worldview.h"
#include "render/render.h"

/* -----------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */

#define WINDOW_TITLE   "Vigalante '76"
/* Larger than the original 640x480 to make the in-engine viewers easier to
 * diagnose; 4:3 preserved so 2D screens (640x480 indexed) upscale cleanly. */
#define WINDOW_WIDTH   1280
#define WINDOW_HEIGHT  960

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
    const char *scene_view = NULL;   /* wireframe mission path, or NULL */
    const char *world_view = NULL;   /* solid 3D mission path, or NULL  */
    bool game_loop = false;          /* --gameloop: state-machine scaffold */
    RenderMode  world_mode = RENDER_HARDWARE;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--meshview") == 0) mesh_view = true;
        else if (strcmp(argv[i], "--gameloop") == 0) game_loop = true;
        else if (strcmp(argv[i], "--sceneview") == 0)
            scene_view = (i + 1 < argc && argv[i+1][0] != '-') ? argv[++i]
                                                               : "miss8/N01.CBT";
        else if (strcmp(argv[i], "--worldview") == 0)
            world_view = (i + 1 < argc && argv[i+1][0] != '-') ? argv[++i]
                                                               : "miss8/N01.CBT";
        else if (strcmp(argv[i], "--render") == 0 && i + 1 < argc) {
            const char *m = argv[++i];
            world_mode = (strcmp(m, "hw") == 0 || strcmp(m, "hardware") == 0)
                       ? RENDER_HARDWARE : RENDER_SOFTWARE;
        }
    }

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
    /* PIX/PAK index                                                    */
    /* Original: pix_init() during session setup. Globs every *.pix and */
    /* builds the global record->pak index so a bare mesh name resolves */
    /* to its bytes inside a packed .pak (see pix.c).                    */
    /* -------------------------------------------------------------- */

    pix_init();

    /* -------------------------------------------------------------- */
    /* Mods (ours, not the original's): presentation-only overrides.    */
    /* Renderer/UI reads go through asset_present_*(); the simulation   */
    /* keeps reading the base archives only. See engine/mods.h.          */
    /* -------------------------------------------------------------- */

    if (!args.no_mods) mods_init(args.mods_root);
    fidelity_set_legacy(args.legacy);
    if (args.legacy) fprintf(stdout, "[main] --legacy: faithful paths for all deviations\n");
    else fprintf(stdout, "[mods] disabled (--no-mods)\n");

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
    /* Seeded before init, as the original.                            */
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
    /* Memory pools, sized as the original's                          */
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
    /* Any keypress skips. Not found = silently continue.              */
    /* -------------------------------------------------------------- */

    /* With no viewer chosen, the intros play and the world viewer opens on
     * the default mission: there is no playable game yet. */
    if (!mesh_view && !scene_view && !world_view) {
        video_play("smk/introf01.smk");
        video_play("smk/credf01.smk");
        if (!game_loop) world_view = "miss8/N01.CBT";
    }

    /* -------------------------------------------------------------- */
    /* Loading screen                                                   */
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
    /*                                                                  */
    /* Each top-level state owns its own inner frame loop (mirroring    */
    /* the original, where every game state ran its own while() with    */
    /* its own message pump). The dispatcher just hands control to the  */
    /* active state's runner and loops until something sets GS_QUIT.    */
    /* -------------------------------------------------------------- */

    if (world_view) {
        /* Solid 3D world: free-fly camera and the vehicle simulation. */
        fprintf(stdout, "[main] world viewer: %s (%s)\n", world_view,
                world_mode == RENDER_SOFTWARE ? "software" : "hardware");
        worldview_run(world_view, world_mode);
    } else if (scene_view) {
        /* Developer entry point: render a mission's placed objects, then exit. */
        fprintf(stdout, "[main] --sceneview: %s\n", scene_view);
        sceneview_run(scene_view);
    } else if (mesh_view) {
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
     * In the original, GS_FRONTEND causes the outer loop
     * to call game_session_run() which runs ShellMain for the menu.
     * We skip all that and pretend the player already picked a scenario.
     *
     * TODO: replace with real session runner once engine is further along.
     */
    fprintf(stdout, "[frontend] Skipping shell — jumping straight to gameplay.\n");
    g_gamestate = GS_GAMEPLAY;
}
