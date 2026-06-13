/*
 * geomesh.c — OEG mesh decode (see geomesh.h)
 *
 * Faithful transcription of geo_build_mesh's (FUN_0043aa60) input parse, run on
 * the decompressed OEG image. We extract positions (vertex array A) and the
 * per-face vertex-index lists — enough to render. The second vertex array and
 * the per-face texture/UV data are parsed-over but not yet stored (TODO when
 * the renderer needs materials).
 */

#include "geomesh.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OEG_MAGIC 0x2e47454fu

/* Sanity caps — guard against a bad/misaligned image producing wild counts. */
#define MAX_VERTS      100000
#define MAX_FACES      100000
#define MAX_FACE_VERTS 64

static uint32_t rd_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static float    rd_f32(const uint8_t *p) { float    v; memcpy(&v, p, 4); return v; }

/* Header field offsets (relative to the OEG magic at byte 0). */
#define OFF_NAME    0x08
#define OFF_NVERTS  0x18
#define OFF_NFACES  0x1c
#define OFF_VERTS   0x24
/* Per-face record: 0x37-byte header, then nFaceVerts × 0x10-byte entries. */
#define FACE_HDR    0x37
#define FACE_VTX    0x10

GeoMesh *geomesh_decode(const void *image, size_t size)
{
    const uint8_t *b = (const uint8_t *)image;

    if (size < OFF_VERTS)        return NULL;
    if (rd_u32(b) != OEG_MAGIC)  return NULL;

    int nV = (int)rd_u32(b + OFF_NVERTS);
    int nF = (int)rd_u32(b + OFF_NFACES);
    if (nV < 0 || nV > MAX_VERTS) return NULL;
    if (nF < 0 || nF > MAX_FACES) return NULL;

    /* Faces begin right after the two vertex arrays: (nVerts*6 + 9) dwords. */
    size_t faces_off = (size_t)(nV * 6 + 9) * 4;
    if (faces_off > size) return NULL;

    /* First pass: validate the face table fits and count total indices. */
    size_t p = faces_off;
    long   total_idx = 0;
    for (int f = 0; f < nF; f++) {
        if (p + FACE_HDR > size) return NULL;
        int nfv = (int)rd_u32(b + p + 4);
        if (nfv < 0 || nfv > MAX_FACE_VERTS) return NULL;
        size_t rec = (size_t)FACE_HDR + (size_t)nfv * FACE_VTX;
        if (p + rec > size) return NULL;
        for (int k = 0; k < nfv; k++) {
            int idx = (int)rd_u32(b + p + FACE_HDR + (size_t)k * FACE_VTX);
            if (idx < 0 || idx >= nV) return NULL;
        }
        total_idx += nfv;
        p += rec;
    }

    GeoMesh *m = calloc(1, sizeof(*m));
    if (!m) return NULL;

    memcpy(m->name, b + OFF_NAME, 16);
    m->name[16]     = '\0';
    m->num_verts    = nV;
    m->num_faces    = nF;
    m->num_indices  = (int)total_idx;

    m->verts      = malloc(sizeof(float) * 3 * (nV ? nV : 1));
    m->face_first = malloc(sizeof(int) * (nF ? nF : 1));
    m->face_count = malloc(sizeof(int) * (nF ? nF : 1));
    m->indices    = malloc(sizeof(int) * (total_idx ? (size_t)total_idx : 1));
    if (!m->verts || !m->face_first || !m->face_count || !m->indices) {
        geomesh_free(m);
        return NULL;
    }

    /* Positions (vertex array A) — 3 floats each, at byte 0x24. */
    for (int i = 0; i < nV; i++) {
        const uint8_t *v = b + OFF_VERTS + (size_t)i * 12;
        m->verts[i * 3 + 0] = rd_f32(v + 0);
        m->verts[i * 3 + 1] = rd_f32(v + 4);
        m->verts[i * 3 + 2] = rd_f32(v + 8);
    }

    /* Faces — vertex-index lists. */
    p = faces_off;
    int oi = 0;
    for (int f = 0; f < nF; f++) {
        int nfv = (int)rd_u32(b + p + 4);
        m->face_first[f] = oi;
        m->face_count[f] = nfv;
        for (int k = 0; k < nfv; k++)
            m->indices[oi++] = (int)rd_u32(b + p + FACE_HDR + (size_t)k * FACE_VTX);
        p += (size_t)FACE_HDR + (size_t)nfv * FACE_VTX;
    }

    /* Bounding box. */
    if (nV > 0) {
        for (int c = 0; c < 3; c++) m->bb_min[c] = m->bb_max[c] = m->verts[c];
        for (int i = 1; i < nV; i++)
            for (int c = 0; c < 3; c++) {
                float val = m->verts[i * 3 + c];
                if (val < m->bb_min[c]) m->bb_min[c] = val;
                if (val > m->bb_max[c]) m->bb_max[c] = val;
            }
    }
    return m;
}

void geomesh_free(GeoMesh *m)
{
    if (!m) return;
    free(m->verts);
    free(m->face_first);
    free(m->face_count);
    free(m->indices);
    free(m);
}
