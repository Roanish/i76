#ifndef CMDLINE_H
#define CMDLINE_H

/*
 * cmdline.h — Command line argument parser
 *
 * Mirrors cmdline(cmdline, &DAT_00526758) from Fun_Real_Entry.
 *
 * Confirmed effects from REVERSING.md:
 *   - Result written into DAT_00526758 (scenario name buffer, 256 bytes)
 *   - DAT_00526a64 |= 2  — some session config flag
 *   - DAT_00526a68 |= 4  — some session config flag
 *
 * Scenario prefix semantics (confirmed via _strnicmp in Fun_Real_Entry):
 *   "mp*" or "ms*" → is_multiplayer = true
 *   "sp*"          → is_multiplayer = false
 *
 * The exact flag strings (-mp, -demo, etc.) are not yet reversed.
 * Placeholder flags are provided so callers can OR them into session config
 * once cmdline is fully reversed.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char     scenario[256];   /* DAT_00526758 — scenario/map name */
    char     asset_root[512]; /* --assets <path> — game asset directory */
    bool     is_multiplayer;
    uint32_t session_flags;   /* bits ORed from DAT_00526a64/a68 */
} CmdlineArgs;

/*
 * cmdline_parse()
 *   Parse argc/argv into *out. Safe to call with argc==1 (no args).
 *   Unrecognised flags are silently ignored until cmdline is reversed.
 */
void cmdline_parse(int argc, char *argv[], CmdlineArgs *out);

#endif /* CMDLINE_H */
