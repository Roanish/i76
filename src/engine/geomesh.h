#ifndef GEOMESH_H
#define GEOMESH_H

/*
 * geomesh.h — Decoded OEG mesh
 *
 * The renderable form of an "OEG." geometry record, produced by geomesh_decode
 * from the (already decompressed) raw image that geo_build_mesh (FUN_0043aa60)
 * consumes in the original. See docs/REVERSING.md "GEO/OEG mesh file format —
 * CONFIRMED & DECODED" for the byte layout. The decode here mirrors the
 * original's pointer arithmetic faithfully:
 *
 *   0x00 u32  magic "OEG." (0x2e47454f)
 *   0x04 u32  count
 *   0x08 char name[16]
 *   0x18 u32  nVerts          (= param_1[6])
 *   0x1c u32  nFaces          (= param_1[7])
 *   0x20 u32  ?
 *   0x24 vec3f posA[nVerts]   (positions — what we render)
 *        vec3f posB[nVerts]   (second vertex set; normals?)
 *        faces[nFaces]: 0x37-byte header (+0x04 = nFaceVerts) then
 *                       nFaceVerts × 0x10-byte entries (+0x00 = vertex index)
 *
 * NOTE: a .pak holds several OEG records concatenated. geomesh_decode reads the
 * FIRST record at the start of `image` (for a single .geo that's the whole
 * file; for a g-tier .pak that's the first sub-mesh — typically the hull).
 */

#include <stddef.h>

typedef struct {
    char   name[17];        /* embedded 16-byte name, null-terminated        */
    int    num_verts;
    int    num_faces;
    int    num_indices;     /* total face-vertex references                   */
    float *verts;           /* [num_verts*3]  x,y,z (positions / array A)      */
    int   *face_first;      /* [num_faces]    offset of face f into indices[]  */
    int   *face_count;      /* [num_faces]    vertex count of face f           */
    int   *indices;         /* [num_indices]  vertex indices into verts        */
    float  bb_min[3];
    float  bb_max[3];
} GeoMesh;

/*
 * Decode the first OEG record in `image` (decompressed bytes, `size` long).
 * Returns a heap GeoMesh (free with geomesh_free) or NULL on a bad/short image.
 * A valid header with zero verts/faces yields a non-NULL empty mesh.
 */
GeoMesh *geomesh_decode(const void *image, size_t size);

void geomesh_free(GeoMesh *m);

#endif /* GEOMESH_H */
