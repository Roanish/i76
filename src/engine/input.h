#ifndef INPUT_H
#define INPUT_H

/*
 * input.h — Per-frame input snapshot
 *
 * The game loop wants a coherent picture of input for one frame, not a stream
 * of raw events. input_poll() drains the platform event queue, folds key
 * up/down events into a held-key state, and hands back a snapshot:
 *
 *   held[]     — button is down right now
 *   pressed[]  — button transitioned up→down THIS frame (edge; good for
 *                toggles like pause and for menu navigation)
 *   quit       — the player asked to quit (window closed, or ESC in gameplay)
 *
 * Buttons are abstract game actions, not physical keys. The mapping from
 * PlatformKey → InputButton lives in input.c, so rebinding later touches one
 * table. WASD and the arrow keys are aliased onto the same drive buttons.
 *
 * ORIGINAL BINARY:
 *   The original polled Win32 keyboard state (and DirectInput for the joystick)
 *   inside each inner loop. We don't have those globals reversed yet; this is a
 *   clean re-implementation of the same per-frame-snapshot idea. The joystick /
 *   analog steering axes will grow into this struct when we get there.
 */

#include <stdbool.h>

typedef enum {
    INPUT_BTN_UP,       /* accelerate / forward  (Up / W)    */
    INPUT_BTN_DOWN,     /* brake / reverse       (Down / S)  */
    INPUT_BTN_LEFT,     /* steer left            (Left / A)  */
    INPUT_BTN_RIGHT,    /* steer right           (Right / D) */
    INPUT_BTN_FIRE,     /* fire                  (Space)     */
    INPUT_BTN_PAUSE,    /* pause toggle          (P)         */
    INPUT_BTN_COUNT
} InputButton;

typedef struct {
    bool held[INPUT_BTN_COUNT];
    bool pressed[INPUT_BTN_COUNT];
    bool quit;
} InputState;

/*
 * Reset the persistent held-key state. Call once when entering a state that
 * owns the input (e.g. start of the gameplay loop) so stale key-downs from a
 * previous state don't leak in.
 */
void input_init(void);

/*
 * Drain the platform event queue and fill *out with this frame's snapshot.
 * Call exactly once per frame, before updating the world.
 */
void input_poll(InputState *out);

#endif /* INPUT_H */
