#ifndef VFS_H
#define VFS_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Initialise the VFS by parsing <root>/nitro.zix.
 * Call once after fs_set_root(). Returns false on fatal failure.
 */
bool vfs_init(void);
void vfs_shutdown(void);

/*
 * Read a file by name. Searches ZFS archives and loose files.
 * Returns a heap-allocated buffer; caller must call vfs_free().
 * Returns NULL if the file is not found.
 */
void *vfs_read_file(const char *name, size_t *size_out);

/*
 * Returns the uncompressed byte count for the named file, or 0 if not found.
 * Does not allocate or read file data.
 */
int vfs_exists(const char *name);

/* Free a buffer returned by vfs_read_file(). */
void vfs_free(void *buf);

/* Call fn(name, src_type, ud) for every file in the VFS index.
 * src_type: 0 = loose, 1 = ZFS archive. */
void vfs_foreach(void (*fn)(const char *name, int src_type, void *ud), void *ud);

#endif /* VFS_H */