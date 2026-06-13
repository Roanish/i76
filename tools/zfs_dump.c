/*
 * zfs_dump.c — Extractor for Interstate '76 Nitro Pack .zfs archives.
 *
 * Thin CLI front-end over the engine's ZFS reader (src/engine/zfs.c), so it
 * shares the exact same parser AND decompression path the game uses. Earlier
 * this tool had its own duplicate parser whose `get`/`extract` wrote the
 * *stored* (LZO-compressed) bytes — silently wrong for any compressed entry.
 * Routing through zfs_read() fixes that: output is always decompressed.
 *
 * Usage:
 *   zfs_dump list    <archive.zfs>            — list all files
 *   zfs_dump extract <archive.zfs> <dest/>    — extract + decompress all files
 *   zfs_dump get     <archive.zfs> <name> <out> — extract one file by name
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#include "engine/zfs.h"

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

static void mkdir_p(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* Decompress `name` and write it to `out_path`. Returns 0 on success. */
static int write_decompressed(ZFSHandle *z, const char *name, const char *out_path)
{
    size_t size = 0;
    void  *data = zfs_read(z, name, &size);
    if (!data) {
        fprintf(stderr, "zfs_dump: read/decompress failed for '%s'\n", name);
        return -1;
    }

    FILE *out = fopen(out_path, "wb");
    if (!out) { perror(out_path); free(data); return -1; }

    fwrite(data, 1, size, out);
    fclose(out);
    free(data);
    return 0;
}

/* -----------------------------------------------------------------------
 * Commands
 * ----------------------------------------------------------------------- */

static int cmd_list(const char *zfs_path)
{
    ZFSHandle *z = zfs_open(zfs_path);
    if (!z) return 1;

    int n = zfs_entry_count(z);
    printf("%d files in %s\n\n", n, zfs_path);
    printf("%6s  %10s  %10s  %-8s  %s\n",
           "index", "stored", "actual", "flags", "name");
    for (int i = 0; i < n; i++) {
        const ZFSEntry *e = zfs_entry_at(z, i);
        uint32_t actual = (e->flags & ZFS_FLAG_COMPRESSED)
                        ? ZFS_UNCOMPRESSED_SIZE(e->flags)
                        : e->size;
        printf("%6u  %10u  %10u  %08x  %s\n",
               e->index, e->size, actual, e->flags, e->name);
    }
    zfs_close(z);
    return 0;
}

static int cmd_extract(const char *zfs_path, const char *dest_dir)
{
    ZFSHandle *z = zfs_open(zfs_path);
    if (!z) return 1;

    mkdir_p(dest_dir);

    int n = zfs_entry_count(z);
    uint32_t ok = 0, fail = 0;
    for (int i = 0; i < n; i++) {
        const ZFSEntry *e = zfs_entry_at(z, i);
        if (e->size == 0) continue;

        char out[1024];
        snprintf(out, sizeof(out), "%s/%s", dest_dir, e->name);

        if (write_decompressed(z, e->name, out) == 0) ok++;
        else                                          fail++;

        if ((i + 1) % 100 == 0 || i + 1 == n)
            fprintf(stderr, "\r  %d / %d", i + 1, n);
    }
    fprintf(stderr, "\n");

    zfs_close(z);
    fprintf(stderr, "Extracted %u files (%u failed) -> %s\n", ok, fail, dest_dir);
    return fail > 0 ? 1 : 0;
}

static int cmd_get(const char *zfs_path, const char *name, const char *out_path)
{
    ZFSHandle *z = zfs_open(zfs_path);
    if (!z) return 1;

    int size = zfs_get_size(z, name);
    if (size < 0) {
        fprintf(stderr, "zfs_dump: '%s' not found in archive\n", name);
        zfs_close(z);
        return 1;
    }

    int rc = write_decompressed(z, name, out_path);
    if (rc == 0)
        fprintf(stderr, "Extracted '%s' (%d bytes, decompressed) -> %s\n",
                name, size, out_path);

    zfs_close(z);
    return rc;
}

/* -----------------------------------------------------------------------
 * Main
 * ----------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr,
            "Usage:\n"
            "  zfs_dump list    <archive.zfs>\n"
            "  zfs_dump extract <archive.zfs> <dest_dir/>\n"
            "  zfs_dump get     <archive.zfs> <filename> <output>\n");
        return 1;
    }

    const char *cmd = argv[1];
    const char *zfs = argv[2];

    if (strcmp(cmd, "list") == 0 && argc == 3)
        return cmd_list(zfs);
    if (strcmp(cmd, "extract") == 0 && argc == 4)
        return cmd_extract(zfs, argv[3]);
    if (strcmp(cmd, "get") == 0 && argc == 5)
        return cmd_get(zfs, argv[3], argv[4]);

    fprintf(stderr, "Unknown command or wrong argument count.\n");
    return 1;
}
