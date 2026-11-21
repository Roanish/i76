#ifndef MODS_H
#define MODS_H

#include <stddef.h>

/*
 * mods.h — presentation-only mod overrides.
 *
 * SCOPE (project decision): mods may change how the game LOOKS — textures,
 * models, graphics — never physics or gameplay. That boundary is structural:
 *
 *   - Renderer/UI code reads assets through asset_present_read(): a mod file
 *     with that name wins, else the original archives (vfs_lod).
 *   - Simulation code (vehicle/mission/terrain definitions, anything that
 *     affects placement, mounts, collision or handling) keeps using
 *     vfs_read_file()/vfs_lod(), which NEVER see mod files.
 *
 * Layout: <mods_root>/<mod>/... ; files match by BASE NAME (case-insensitive)
 * at any depth inside a mod folder, because archive records are flat names —
 * e.g. mods/hd_wheels/geo/WA11RWL3.geo overrides record "WA11RWL3.geo".
 *
 * Load order: <mods_root>/load_order.txt, one mod folder name per line, FIRST
 * line = highest priority; blank lines and '#' comments ignored. Without that
 * file every subfolder is enabled in alphabetical order.
 */

/* Scan mods_root and build the override index. NULL/missing dir = no mods.
 * Safe to call once at startup (after fs_set_root). */
void mods_init(const char *mods_root);
void mods_shutdown(void);

/* Number of indexed override files (0 = no mods active). */
int mods_count(void);

/* Path of the mod file overriding `name` (base name, any case), or NULL.
 * For loaders that want a path (future PNG/glTF readers). */
const char *mods_find(const char *name);

/* Read the mod override for `name` into a malloc'd buffer (free()/vfs_free()),
 * or NULL if no mod supplies it. Does NOT fall back to the archives. */
void *mods_read(const char *name, size_t *out_size);

/* PRESENTATION read: mod override if present, else the original archives
 * (vfs_lod: PIX/PAK index, then ZFS/loose). Buffer is malloc'd (vfs_free()). */
void *asset_present_read(const char *name, size_t *out_size);

/* Same, but the base fallback is vfs_read_file (direct ZFS/loose file, no PIX
 * record lookup) — for callers that previously read with vfs_read_file. */
void *asset_present_file(const char *name, size_t *out_size);

#endif /* MODS_H */
