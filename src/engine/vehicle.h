#ifndef VEHICLE_H
#define VEHICLE_H

#include "engine/scene.h"   /* SceneItem */

/*
 * vehicle.h — a vehicle chassis model loaded from a .vdf's VGEO part grid.
 *
 * The VGEO chunk holds 28 groups (7 damage states x 4 LODs) of 0x64-byte part
 * records (the standard object_alloc record: name/3x3/pos/parent/type). We load
 * group (0,0) — pristine, highest LOD — compose each part's parent chain, and
 * acquire its "<name>.geo" mesh (a standard OEG pak record) via the mesh cache.
 * See docs/REVERSING.md "VDF vehicle definition" (vgeo_load FUN_004b90b0,
 * vdf_parts_build FUN_004b3140).
 *
 * Parts are returned as SceneItems whose R/t are VEHICLE-ROOT-relative; the
 * caller composes them with the vehicle's world transform each frame. Records
 * with no mesh ("NULL"/"none" names — mount points) come back with mesh=NULL.
 */

/*
 * Skin binding: faces on a vehicle part name PLACEHOLDER textures ("V1 FT RT.MAP",
 * "V1 BO DY.MAP", ...). The skin .vtf maps each placeholder, per part model
 * slot, to a real texture name (a .tmt name table or a .map). Mirrors the
 * original's per-model override table (FUN_0043c460, keyed by model+name) as
 * filled by the in-game VTFC handler (FUN_004ba600). part_slot < 0 = any part.
 */
typedef struct {
    int  part_slot;          /* VEH_SLOT_* or -1 (all parts: "V1 BO DY")  */
    char placeholder[16];    /* e.g. "V1 FT RT.map" (compared case-insens.) */
    char real[16];           /* e.g. "1ds1FTRT.TMT"                       */
} VehicleSkinBind;

/* Part model slots (vgeo_part_register FUN_004b8ff0: part type -> slot). */
enum { VEH_SLOT_FT, VEH_SLOT_MD, VEH_SLOT_BK, VEH_SLOT_TP, VEH_SLOT_FB, VEH_SLOT_BB,
       VEH_SLOT_COUNT };

typedef struct {
    SceneItem *parts;
    int        count;        /* all records (meshed + mount points) */
    int        mesh_count;   /* parts with a loaded mesh            */
    int       *part_slot;    /* [count] VEH_SLOT_* or -1            */
    VehicleSkinBind *binds;  /* skin bindings (NULL if no skin)     */
    int        bind_count;
    /* WLOC: 6 wheel mounts (3 axles x right/left), vehicle-root frame. */
    int        wloc_present[6];
    float      wloc_R[6][9], wloc_t[6][3];
} VehicleModel;

/* Load a .vdf by VFS name (e.g. "vdstag.vdf"), optionally skinned by a .vtf
 * (e.g. "stag1.vtf"; NULL = no skin). Returns NULL on failure. */
VehicleModel *vehicle_load(const char *vdf_name, const char *vtf_name);

/* Load from a .vcf vehicle config (e.g. "vdstag1.vcf"): its VCFC chunk names
 * the chassis .vdf and the skin .vtf. Returns NULL on failure. */
VehicleModel *vehicle_load_vcf(const char *vcf_name);

/* Attach the tyres of one axle from a .wdf wheel def (e.g. "wauto_3a.wdf"):
 * axle_first = 0/2/4 (front/middle/rear WLOC pair). Its WGEO holds two part
 * sets (right wheel, left wheel); each is placed at its WLOC mount. Returns
 * 0 on failure. "null" names are a no-op success (no wheels on that axle). */
int vehicle_attach_wheels(VehicleModel *v, const char *wdf_name, int axle_first);

/* The texture name to use for a face on part `part`: the skin binding for its
 * placeholder name if one exists, else face_name unchanged. */
const char *vehicle_face_texture(const VehicleModel *v, int part, const char *face_name);

void vehicle_free(VehicleModel *v);

#endif /* VEHICLE_H */
