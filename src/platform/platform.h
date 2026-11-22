#ifndef PLATFORM_H
#define PLATFORM_H

/*
 * platform.h — Platform Hardware Abstraction Layer
 *
 * Every OS call goes through here: no file outside src/platform/ calls SDL
 * or the OS directly. The original's Win32 details are in docs/REVERSING.md
 * "Platform layer".
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward-declare SDL_Window so callers don't need to include SDL headers. */
struct SDL_Window;

/* ------------------------------------------------------------------ */
/* Types                                                                */
/* ------------------------------------------------------------------ */

/*
 * Platform event — what the game loop actually cares about.
 * We don't expose raw SDL/Win32 events; just what the game needs.
 */
typedef enum {
    PLATFORM_EVENT_NONE,
    PLATFORM_EVENT_QUIT,        /* WM_QUIT equivalent */
    PLATFORM_EVENT_KEY_DOWN,
    PLATFORM_EVENT_KEY_UP,
    PLATFORM_EVENT_MOUSE_DOWN,  /* any mouse button press (video skip etc.) */
} PlatformEventType;

/*
 * Platform-neutral key codes.
 *
 * The original game read raw Win32 VK_ codes; we don't want SDL_Keycode (or
 * any OS key enum) leaking past the HAL boundary, so platform_sdl.c translates
 * its native keys into these. Add entries here as the game needs more keys —
 * everything above the HAL refers to keys only by PlatformKey.
 */
typedef enum {
    PK_UNKNOWN = 0,
    PK_ESCAPE,
    PK_RETURN,
    PK_SPACE,
    PK_UP, PK_DOWN, PK_LEFT, PK_RIGHT,
    PK_W, PK_A, PK_S, PK_D,
    PK_Q, PK_E,
    PK_P,
    PK_O,
    PK_L,
    PK_F,
    PK_TAB,
    PK_LSHIFT,
    PK_COUNT
} PlatformKey;

typedef struct {
    PlatformEventType type;
    PlatformKey       key;      /* PK_UNKNOWN for keys we don't translate */
    uint16_t          hid;      /* physical key as a USB-HID usage id (0 = none);
                                 * layout-independent, used by key bindings   */
} PlatformEvent;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

/*
 * platform_init()
 *   Bring up the window and SDL subsystems.
 *
 *   title  — window title (original: "Interstate '76 Nitro Pack")
 *   width  — client area width  in pixels
 *   height — client area height in pixels
 *
 *   Returns true on success, false on failure (caller should exit).
 */
/*
 * platform_get_window()
 *   Return the SDL_Window* created by platform_init().
 *   Used by render_vk.c to create the Vulkan surface via SDL_Vulkan_*.
 *   Returns NULL before platform_init() succeeds.
 */
struct SDL_Window *platform_get_window(void);

bool platform_init(const char *title, int width, int height);

/*
 * platform_shutdown()
 *   Tears down the window and SDL.
 */
void platform_shutdown(void);

/* ------------------------------------------------------------------ */
/* Event loop                                                           */
/* ------------------------------------------------------------------ */

/*
 * platform_pump_events()
 *   Non-blocking event pump. Call once per frame at the top of the
 *   game loop. Fills *evt with the first relevant event found, or
 *   PLATFORM_EVENT_NONE if the queue is empty.
 *
 *   Returns false if a quit event was received (caller should set
 *   g_gamestate = GS_QUIT).
 */
bool platform_pump_events(PlatformEvent *evt);

/*
 * platform_flush_events()
 *   Drain and discard everything queued (stale keypresses buffered during a
 *   video or a long load), so the next consumer starts from a clean queue.
 *   A quit event encountered while flushing is NOT discarded — it is
 *   reported by returning false, like platform_pump_events().
 */
bool platform_flush_events(void);

/* ------------------------------------------------------------------ */
/* Timing                                                               */
/* ------------------------------------------------------------------ */

/*
 * platform_get_ticks()
 *   Milliseconds since platform_init().
 */
uint64_t platform_get_ticks(void);

/*
 * platform_sleep()
 *   Yield CPU for approximately ms milliseconds.
 *   Used to cap frame rate when the game is idle.
 */
void platform_sleep(uint32_t ms);

/* ------------------------------------------------------------------ */
/* Memory                                                               */
/* ------------------------------------------------------------------ */

/*
 * platform_valloc() / platform_vfree()
 *   Large virtual memory blocks.
 *
 *   On Linux we use mmap(MAP_ANONYMOUS|MAP_PRIVATE).
 *   Returns NULL on failure (caller mirrors original fatal error path).
 */
void *platform_valloc(size_t size);
void  platform_vfree(void *ptr, size_t size);

#endif /* PLATFORM_H */
