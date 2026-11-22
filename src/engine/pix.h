#ifndef PIX_H
#define PIX_H

#include <stddef.h>

/*
 * pix.h — global PIX/PAK record index (our reimplementation of the original's
 * pix_init + vfs_lod_pak resolver).
 *
 * The original game indexes every record packed into the .pak archives through
 * a set of plain-text .pix files. Each ".pix" sits beside a ".pak" of the same
 * base name and lists, one per line:
 *
 *     <count>\r\n
 *     NAME.ext <byte_offset_into_pak> <byte_length>\r\n   x count
 *
 * pix_init globs all *.pix and builds one global array keyed by uppercased
 * "BASE.EXT" (8-char base + extension), sorted for bsearch. vfs_lod(name)
 * looks up that key; a bare name (no extension) defaults to ".GEO" so mesh
 * names resolve as geometry, while textures pass an explicit extension
 * (X.vqm / X.map / X.tmt) and no longer collide on a shared base. On a hit it
 * loads the owning .pak (decompressed) and returns the record bytes; on a miss
 * it falls back to a loose VFS file. So "AN1ABLD1" -> AN1ABLD1.geo in
 * anbunk1g.pak, while "an_1bdt1.vqm" -> that record in anbunk1m.pak.
 *
 * Verified against real data: the .pix offsets are byte offsets into the
 * DECOMPRESSED pak and land exactly on the OEG records.
 */

/* Build the global record index from every *.pix in the VFS. Idempotent. */
void pix_init(void);

void pix_shutdown(void);

/* Number of indexed records (diagnostics). */
int pix_count(void);

/*
 * Resolve `name` (a record/geometry name, with or without extension) through
 * the PIX/PAK index. On a hit, returns a freshly malloc'd copy of the record's
 * bytes from its pak. On a miss, falls back to vfs_read_file(name) (loose file
 * or whole-archive entry). Returns NULL if neither resolves. Caller frees with
 * free(). *out_size receives the byte count.
 */
void *vfs_lod(const char *name, size_t *out_size);

#endif /* PIX_H */
