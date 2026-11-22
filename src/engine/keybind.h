#ifndef KEYBIND_H
#define KEYBIND_H

#include <stdbool.h>
#include <stdint.h>

#include "engine/input.h"   /* RawInputState (per-HID key state) */

/*
 * keybind.h — the original game's input binding system, reproduced.
 *
 * Reads the two config files the original reads (nitro.exe, see
 * docs/REVERSING.md "Input bindings"):
 *   input.map   — held actions: `action { + keyboard Key  - keyboard Shift ... }`
 *                 (+ = key must be held, - = must NOT be held; a block is one
 *                 chord, several blocks per action OR together)
 *   gamekey.map — commands: `COMMAND  [Control+|ALT+|Shift+]Key`
 *
 * Actions come from the original action table (keybind_tables.c). Axis
 * actions (type 0) bound through X_plus / X_minus buttons are integrated per
 * frame exactly like the original's analog sink stepper (FUN_00441110):
 * 16.16 value in [-1,1], velocity += 3*ftol(dt*182) << rate (cap 0x5555),
 * "_delta" axes alias velocity and value (they snap back to 0 on release),
 * output = integer in [min, max].
 */

#define KB_NUM_CHANNELS 125
#define KB_NUM_ACTIONS  86

typedef struct { const char *name; uint8_t hid; } KbChannel;
typedef struct {
    const char *name;
    int type;                    /* 0 axis, 1 button, 2 toggle            */
    int dflt, min, max, rate;    /* axis parameters                       */
    int shift;                   /* output right-shift (flags byte)       */
} KbActionDef;
typedef struct { const char *name; int id; } KbCommandDef;

extern const KbChannel    KB_CHANNELS[KB_NUM_CHANNELS];
extern const KbActionDef  KB_ACTIONS[KB_NUM_ACTIONS];
extern const KbCommandDef KB_COMMANDS[];

/* Load input.map + gamekey.map from the asset root (missing files fall back
 * to a small built-in default set). Safe to call again to reload. */
void keybind_load(void);

/* Advance one frame: evaluate chords, step axes, queue pressed commands. */
void keybind_update(const RawInputState *in, float dt);

/* Action index by (case-insensitive) name, or -1. */
int  keybind_action(const char *name);

/* Axis output (integer in [min, max] for shift 16; the action's default if
 * nothing drives it). */
int  keybind_axis(int action);

/* Button held this frame (any bound chord satisfied). */
bool keybind_button(int action);

/* Next command triggered this frame (gamekey.map), or 0 when none remain. */
int  keybind_next_command(void);

/* Command id by name (e.g. "PRESET_VIEW_2" -> 15), or 0. */
int  keybind_command_id(const char *name);

#endif /* KEYBIND_H */
