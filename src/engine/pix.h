#ifndef PIX_H
#define PIX_H

#include <stddef.h>

/*
 * pix.h — global PIX/PAK record index (our reimplementation of the original's
 * pix_init (FUN_00467cd0) + vfs_lod_pak resolver).
 *
 * The original game indexes every record packed into the .pak archives through
 * a set of plain-text .pix files. Each ".pix" sits beside a ".pak" of the same
 * base name and lists, one per line:
 *
 *     <count>\r\n
 *     NAME.ext <byte_offset_into_pak> <byte_length>\r\n   x count
 *
 * pix_init globs all *.pix, strips each record name to its 8-char base, and
 * builds one global array sorted by name. vfs_lod(name) bsearches that array;
 * on a hit it loads the owning .pak (decompressed) and returns the record's
 * bytes; on a miss it falls back to a loose VFS file. This is how a bare mesh
 * name like "AN1ABLD1" resolves to its bytes inside anbunk1g.pak.
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
