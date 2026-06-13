/*
 * input.c — Per-frame input snapshot (see input.h)
 *
 * Held state persists across frames in file-static storage; input_poll()
 * mutates it from the events drained each frame and derives the per-frame
 * edge (pressed[]) by diffing against the previous frame's held state.
 */

#include "input.h"

#include <string.h>

#include "platform/platform.h"

/* Held state, persisted between frames. Updated by key down/up events. */
static bool s_held[INPUT_BTN_COUNT];
/* Held state as it was at the end of the previous frame — for edge detection. */
static bool s_prev[INPUT_BTN_COUNT];

/*
 * Map a platform key to a game button, or -1 if the key isn't bound.
 * WASD and the arrows alias onto the same drive buttons.
 */
static int key_to_button(PlatformKey key)
{
    switch (key) {
        case PK_UP:    case PK_W: return INPUT_BTN_UP;
        case PK_DOWN:  case PK_S: return INPUT_BTN_DOWN;
        case PK_LEFT:  case PK_A: return INPUT_BTN_LEFT;
        case PK_RIGHT: case PK_D: return INPUT_BTN_RIGHT;
        case PK_SPACE:            return INPUT_BTN_FIRE;
        case PK_P:                return INPUT_BTN_PAUSE;
        default:                  return -1;
    }
}

void input_init(void)
{
    memset(s_held, 0, sizeof(s_held));
    memset(s_prev, 0, sizeof(s_prev));
}

void input_poll(InputState *out)
{
    /* Last frame's final held state becomes this frame's "previous". */
    memcpy(s_prev, s_held, sizeof(s_held));

    bool quit = false;

    for (;;) {
        PlatformEvent e;
        if (!platform_pump_events(&e)) {
            /* Hard quit (window closed). */
            quit = true;
            break;
        }
        if (e.type == PLATFORM_EVENT_NONE)
            break;                              /* queue drained */

        /* ESC quits out of the state that owns input. */
        if (e.type == PLATFORM_EVENT_KEY_DOWN && e.key == PK_ESCAPE)
            quit = true;

        int btn = key_to_button(e.key);
        if (btn < 0)
            continue;

        if (e.type == PLATFORM_EVENT_KEY_DOWN)
            s_held[btn] = true;
        else if (e.type == PLATFORM_EVENT_KEY_UP)
            s_held[btn] = false;
    }

    for (int i = 0; i < INPUT_BTN_COUNT; i++) {
        out->held[i]    = s_held[i];
        out->pressed[i] = s_held[i] && !s_prev[i];
    }
    out->quit = quit;
}
