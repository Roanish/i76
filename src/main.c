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
#include "SDL.h"

#include "platform/platform.h"
#include "engine/gamestate.h"
#include "engine/cmdline.h"
#include "engine/fs.h"
#include "engine/vfs.h"
#include "engine/strlookup.h"
#include "engine/pcx.h"
#include "engine/font.h"
#include "engine/video.h"
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

static void state_gameplay_update(void);
static void state_frontend_update(void);
static void state_mainmenu_update(void);
static void state_lobby_update(void);
static void state_endsession_update(void);
static void state_missionend_update(void);

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
    /* Font test — remove before ship                                   */
    /* -------------------------------------------------------------- */

    {
        static uint8_t fb[WINDOW_WIDTH * WINDOW_HEIGHT];
        static Rgb8    pal[256];
        memset(fb,  0, sizeof(fb));
        memset(pal, 0, sizeof(pal));
        pal[255] = (Rgb8){255, 255, 255};

        /* Render-path smoke test: white rectangle, no font needed */
        for (int row = 10; row < 20; row++)
            for (int col = 10; col < 200; col++)
                fb[row * WINDOW_WIDTH + col] = 255;

        Font *fnt = font_load("base6x7.fnt");
        fprintf(stdout, "[main] font_load: %s\n", fnt ? "OK" : "FAILED");
        fflush(stdout);
        if (fnt) {
            const char *lines[] = {
                "INTERSTATE '76  --  OPEN REWRITE",
                "font system: OK",
            };
            int y = (WINDOW_HEIGHT - (int)(fnt->height * 2 + 4)) / 2;
            for (int i = 0; i < 2; i++) {
                int tx = (WINDOW_WIDTH - font_text_width(fnt, lines[i])) / 2;
                font_draw(fnt, lines[i], fb, WINDOW_WIDTH, WINDOW_HEIGHT,
                          tx, y, 255);
                y += (int)fnt->height + 4;
            }
            font_free(fnt);
        }

        render_begin_frame();
        render_set_palette(pal);
        render_blit_indexed(fb, WINDOW_WIDTH, WINDOW_HEIGHT);
        render_end_frame();
        SDL_Delay(2000);
    }

    /* -------------------------------------------------------------- */
    /* Intro + credits videos                                           */
    /* Original: video_play(hwnd, "introf01.smk") then                 */
    /*           video_play(hwnd, "credf01.smk") before main loop.     */
    /* Any keypress skips. Not found = silently continue.              */
    /* -------------------------------------------------------------- */

    video_play("smk/introf01.smk");
    video_play("smk/credf01.smk");

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
    /* -------------------------------------------------------------- */

    fprintf(stdout, "[main] Entering game loop. ESC or close to quit.\n");

    while (g_gamestate != GS_QUIT) {

        PlatformEvent evt;
        if (!platform_pump_events(&evt)) {
            g_gamestate = GS_QUIT;
            break;
        }

        render_begin_frame();

        switch (g_gamestate) {
            case GS_GAMEPLAY:    state_gameplay_update();    break;
            case GS_FRONTEND:    state_frontend_update();    break;
            case GS_MAINMENU:    state_mainmenu_update();    break;
            case GS_SHELL:       /* ShellMain running — handled inside session runner */ break;
            case GS_LOBBY:
            case GS_LOBBY_ALT:   state_lobby_update();       break;
            case GS_ENDSESSION:  state_endsession_update();  break;
            case GS_MISSIONEND:  state_missionend_update();  break;
            case GS_MP_FOLLOWON:
                g_gamestate = GS_FRONTEND;
                break;
            case GS_QUIT:
                break;
        }

        render_end_frame();
    }

    /* -------------------------------------------------------------- */
    /* Shutdown                                                         */
    /* -------------------------------------------------------------- */

    platform_vfree(pool_secondary, 0x80000);
    platform_vfree(pool_primary,   0x40000);

    StrLookupDestroy(sl);
    vfs_shutdown();
    render_shutdown();
    platform_shutdown();

    fprintf(stdout, "[main] Exiting with code %d\n", g_exit_code);
    return g_exit_code;
}

/* -----------------------------------------------------------------------
 * State handlers — stubs
 * Each will grow into its own .c/.h as we implement them.
 * ----------------------------------------------------------------------- */

static void state_gameplay_update(void)
{
    static bool printed = false;
    if (!printed) {
        fprintf(stdout, "[state] GS_GAMEPLAY — stub\n");
        printed = true;
    }
}

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

static void state_mainmenu_update(void)
{
    static bool printed = false;
    if (!printed) {
        fprintf(stdout, "[state] GS_MAINMENU — stub\n");
        printed = true;
    }
}

static void state_lobby_update(void)
{
    static bool printed = false;
    if (!printed) {
        fprintf(stdout, "[state] GS_LOBBY — stub\n");
        printed = true;
    }
}

static void state_endsession_update(void)
{
    static bool printed = false;
    if (!printed) {
        fprintf(stdout, "[state] GS_ENDSESSION — stub\n");
        printed = true;
    }
}

static void state_missionend_update(void)
{
    static bool printed = false;
    if (!printed) {
        fprintf(stdout, "[state] GS_MISSIONEND — stub\n");
        printed = true;
    }
}
