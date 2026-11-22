#ifndef CMDLINE_H
#define CMDLINE_H

/*
 * cmdline.h — command line arguments.
 *
 * Scenario prefix: "mp*" or "ms*" = multiplayer, "sp*" = single player.
 * The original's own flag strings are not known yet (docs/REVERSING.md).
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char     scenario[256];   /* scenario/map name */
    char     asset_root[512]; /* --assets <path> — game asset directory */
    char     mods_root[528];  /* --mods <path> (default <asset_root>/mods); "" = --no-mods */
    bool     no_mods;         /* --no-mods: run on the original assets only */
    bool     legacy;          /* --legacy: faithful path for every deviation (docs/DEVIATIONS.md) */
    bool     is_multiplayer;
    uint32_t session_flags;   /* session config bits */
} CmdlineArgs;

/*
 * cmdline_parse()
 *   Parse argc/argv into *out. Safe to call with argc==1 (no args).
 *   Unrecognised flags are silently ignored until cmdline is reversed.
 */
void cmdline_parse(int argc, char *argv[], CmdlineArgs *out);

#endif /* CMDLINE_H */
