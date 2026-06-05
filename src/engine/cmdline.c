/*
 * cmdline.c — Command line argument parser
 *
 * Placeholder implementation until cmdline is reversed.
 * Handles the confirmed behaviours; unknown flags are collected so
 * we can add them once the original parsing is known.
 */

#include "cmdline.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  define strncasecmp _strnicmp
#endif

void cmdline_parse(int argc, char *argv[], CmdlineArgs *out)
{
    memset(out, 0, sizeof(*out));
    strncpy(out->asset_root, ".", sizeof(out->asset_root) - 1);

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--assets") == 0 && i + 1 < argc) {
            strncpy(out->asset_root, argv[++i], sizeof(out->asset_root) - 1);
        } else if (arg[0] == '-') {
            /*
             * Original flags — exact strings not yet reversed from cmdline.
             * TODO: reverse cmdline and fill these in.
             */
            fprintf(stdout, "[cmdline] Unknown flag (not yet reversed): %s\n", arg);
        } else if (out->scenario[0] == '\0') {
            strncpy(out->scenario, arg, sizeof(out->scenario) - 1);
        }
    }

    /* Derive is_multiplayer from scenario prefix — confirmed _strnicmp logic */
    if (strncasecmp(out->scenario, "mp", 2) == 0 ||
        strncasecmp(out->scenario, "ms", 2) == 0) {
        out->is_multiplayer = true;
    }

    if (out->scenario[0])
        fprintf(stdout, "[cmdline] Scenario: \"%s\" (multiplayer=%d)\n",
                out->scenario, out->is_multiplayer);
}
