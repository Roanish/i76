/*
 * platform_sdl.c — SDL2 implementation of platform.h
 *
 * SDL2 owns: window creation, event pump, timing, virtual memory.
 * Vulkan owns: all rendering (render_vk.c calls SDL_Vulkan_* to get the surface).
 *
 * SDL_Renderer is gone — Vulkan presents directly to the swapchain.
 */

#include "platform.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __linux__
#  include <sys/mman.h>
#endif

/* -----------------------------------------------------------------------
 * Internal state
 * ----------------------------------------------------------------------- */

static SDL_Window *s_window  = NULL;
static bool        s_running = false;

/* -----------------------------------------------------------------------
 * Window accessor (used by render_vk.c via platform_get_window())
 * ----------------------------------------------------------------------- */

struct SDL_Window *platform_get_window(void)
{
    return s_window;
}

/* -----------------------------------------------------------------------
 * Lifecycle
 * ----------------------------------------------------------------------- */

bool platform_init(const char *title, int width, int height)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[platform] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    /*
     * SDL_WINDOW_VULKAN: tells SDL2 to set up the window for Vulkan surface
     * creation. render_vk.c calls SDL_Vulkan_CreateSurface() later.
     */
    s_window = SDL_CreateWindow(
        title,
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        width,
        height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_VULKAN
    );

    if (!s_window) {
        fprintf(stderr, "[platform] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }

    s_running = true;
    fprintf(stdout, "[platform] Window created: %s (%dx%d)\n", title, width, height);
    return true;
}

void platform_shutdown(void)
{
    if (s_window) {
        SDL_DestroyWindow(s_window);
        s_window = NULL;
    }
    SDL_Quit();
    fprintf(stdout, "[platform] Shutdown complete.\n");
}

/* -----------------------------------------------------------------------
 * Event loop
 * ----------------------------------------------------------------------- */

/*
 * Translate an SDL keycode into a platform-neutral PlatformKey.
 * Keys we don't care about map to PK_UNKNOWN and are ignored upstream.
 */
static PlatformKey translate_key(SDL_Keycode sym)
{
    switch (sym) {
        case SDLK_ESCAPE: return PK_ESCAPE;
        case SDLK_RETURN: return PK_RETURN;
        case SDLK_SPACE:  return PK_SPACE;
        case SDLK_UP:     return PK_UP;
        case SDLK_DOWN:   return PK_DOWN;
        case SDLK_LEFT:   return PK_LEFT;
        case SDLK_RIGHT:  return PK_RIGHT;
        case SDLK_w:      return PK_W;
        case SDLK_a:      return PK_A;
        case SDLK_s:      return PK_S;
        case SDLK_d:      return PK_D;
        case SDLK_p:      return PK_P;
        default:          return PK_UNKNOWN;
    }
}

bool platform_pump_events(PlatformEvent *evt)
{
    evt->type = PLATFORM_EVENT_NONE;
    evt->key  = PK_UNKNOWN;

    SDL_Event sdl_evt;
    if (!SDL_PollEvent(&sdl_evt))
        return true;

    switch (sdl_evt.type) {

        /*
         * Hard quit: the window was closed by the OS / WM. ESC is NOT handled
         * here any more — it's reported as an ordinary key so the game layer
         * decides what it means (quit from gameplay, back out of a menu, ...).
         */
        case SDL_QUIT:
            evt->type = PLATFORM_EVENT_QUIT;
            s_running = false;
            return false;

        case SDL_KEYDOWN:
            evt->type = PLATFORM_EVENT_KEY_DOWN;
            evt->key  = translate_key(sdl_evt.key.keysym.sym);
            return true;

        case SDL_KEYUP:
            evt->type = PLATFORM_EVENT_KEY_UP;
            evt->key  = translate_key(sdl_evt.key.keysym.sym);
            return true;

        default:
            return true;
    }
}

/* -----------------------------------------------------------------------
 * Timing
 * ----------------------------------------------------------------------- */

uint64_t platform_get_ticks(void)
{
    return SDL_GetTicks64();
}

void platform_sleep(uint32_t ms)
{
    SDL_Delay(ms);
}

/* -----------------------------------------------------------------------
 * Memory
 * ----------------------------------------------------------------------- */

void *platform_valloc(size_t size)
{
#ifdef __linux__
    void *ptr = mmap(NULL, size,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS,
                     -1, 0);
    if (ptr == MAP_FAILED) {
        fprintf(stderr, "[platform] mmap failed for size %zu\n", size);
        return NULL;
    }
    return ptr;
#else
    void *ptr = calloc(1, size);
    if (!ptr)
        fprintf(stderr, "[platform] calloc failed for size %zu\n", size);
    return ptr;
#endif
}

void platform_vfree(void *ptr, size_t size)
{
#ifdef __linux__
    if (ptr) munmap(ptr, size);
#else
    (void)size;
    free(ptr);
#endif
}
