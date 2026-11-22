#ifndef SCENE_H
#define SCENE_H

#include <stdint.h>

#include "engine/geomesh.h"

/*
 * scene.h — a world scene built from a mission file's placed objects.
 *
 * Parses an I'76 mission file (e.g. miss8/N01.CBT), walks ODEF -> OBJ records,
 * and turns each placed object into one or more renderable items. The OBJ
 * record layout:
 *   +0x00 name[16]   +0x08 3x3 orientation   +0x2c world position   +0x5c type
 * For a structure object the name resolves to a <name>.sdf whose SGEO chunk
 * lists part meshes (each with a structure-LOCAL transform); we compose
 * object-world * part-local and resolve each part mesh through the PIX/PAK
 * index. Marker objects (spawn/regen, no .sdf) become mesh-less items.
 */

typedef struct {
    const GeoMesh *mesh;   /* renderable mesh, or NULL for a marker        */
    float R[9];            /* world 3x3 orientation (row-major)            */
    float t[3];            /* world position                               */
    int   type;            /* OBJ type code                                */
    char  name[16];
} SceneItem;

/*
 * A terrain patch: one 128x128 heightfield tile placed at a coarse-grid cell.
 * The mission TDEF/ZMAP is a sparse 80x80 grid of zone indices (0xff = empty);
 * each non-empty cell gets one zone slab from the .TER file. Scale is derived
 * empirically (see scene.c): coarse cell = 640 world units, so fine cell = 5.
 * Cell (fz,fx) -> world x = origin[0]+fx*cell, z = origin[1]+fz*cell,
 *                 y = heights[fz*dim+fx] * 0.1 (bilinear between cells).
 * Each .TER cell is a u16; the visible height is the low 12 bits =
 * ((byte1 & 0x0F) << 8) | byte0, range 0..4095. byte1>>4 is the material.
 */
typedef struct {
    float     origin[2];   /* world (x,z) of the patch's min corner         */
    int       dim;         /* cells per side (128)                          */
    float     cell;        /* world units per fine cell (5.0)               */
    uint16_t *heights;     /* dim*dim 12-bit heights ((b1&0xF)<<8 | b0)      */
    uint8_t  *surface;     /* dim*dim surface type (0..7)                    */
    int       zone;
} TerrainPatch;

/* One road cross-section "rib" (RDEF/RSEG): two edge points in the XZ plane.
 * The road has no stored Y — it drapes onto the terrain. Consecutive ribs form
 * the road ribbon (quad strip). See docs/REVERSING.md "RDEF / road segments". */
typedef struct {
    float lx, lz;   /* edgeA (rib +0x00 X, +0x08 Z) */
    float rx, rz;   /* edgeB (rib +0x0c X, +0x14 Z) */
} RoadRib;

/* One RSEG chunk = one road: payload +0x00 type (selects the texture set,
 * type % 3), +0x04 rib count, ribs from +0x08. */
typedef struct {
    int type;
    int first, count;   /* range in Scene.road */
} RoadSeg;

typedef struct {
    SceneItem    *items;
    int           count;
    TerrainPatch *patches;
    int           patch_count;
    RoadRib      *road;        /* every RSEG's ribs, concatenated (NULL if none) */
    int           road_count;
    RoadSeg      *road_segs;   /* one per RSEG chunk, in file order          */
    int           road_seg_count;
    float         center[3];  /* scene centroid (camera target)            */
    float         radius;     /* bounding radius about center              */
    /* WDEF->WRLD terrain texture set (empty if the mission has no WDEF). */
    char          palette_name[16];  /* e.g. "t15.act"      (terrain palette)  */
    char          ground_tex[16];    /* e.g. "tt161sd_.map" (ground texture)   */
    char          lum_name[16];      /* e.g. "t15.LUM" (32-row shade table)    */
    char          sky_tex[16];       /* e.g. "nk_1cld2.map" (cloud texture)    */
    int           hour;              /* WRLD +0x9b time of day 0..23 (-1 = none) */
    /* Surface table: grip, rolling resistance, bump per surface type. */
    float         surf_grip[8], surf_roll[8], surf_bump[8], surf_f3[8];
    int           surf_effect[8];
    int           have_surfaces;
} Scene;

/* Load a mission file (path relative to the asset root, e.g. "miss8/N01.CBT").
 * Returns NULL on failure. Acquires meshes via the mesh cache. */
Scene *scene_load_mission(const char *path);

void scene_free(Scene *s);

#endif /* SCENE_H */
