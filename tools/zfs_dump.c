/*
 * zfs_dump.c — Extractor for Interstate '76 Nitro Pack .zfs archives.
 *
 * ZFSF format (reverse-engineered):
 *
 *   Main header (28 bytes):
 *     [0]  char[4]  magic = "ZFSF"
 *     [4]  uint32   version = 1
 *     [8]  uint32   0x10 (unknown)
 *     [12] uint32   entries_per_block = 100
 *     [16] uint32   total_file_count  (includes deleted entries)
 *     [20] uint32   creation_flags    (passed to zfs_create; purpose unknown)
 *     [24] uint32   first_block_offset (= 28 for fresh archives)
 *
 *   Blocks form a singly-linked list starting at first_block_offset.
 *   Each block is exactly 3604 bytes (0xe14):
 *     [+0]  uint32    next_block_offset  (absolute; 0 = last block)
 *     [+4]  entry[100]  directory entries, 36 bytes each:
 *             char[16]  name (null-padded, DOS 8.3, all-caps)
 *             uint32    offset     — absolute byte offset of file data in .zfs
 *             uint32    index      — sequential 0-based index
 *             uint32    size       — stored (compressed) size in bytes
 *             uint32    timestamp
 *             uint32    flags      — bit 0=deleted, bits 1/2=compression type, bits 8-31=uncompressed size
 *
 *   Deleted entries (flags & 1) are skipped. total_count after open reflects
 *   live entries only.
 *
 * Usage:
 *   zfs_dump list <archive.zfs>            — list all files
 *   zfs_dump extract <archive.zfs> <dest/> — extract all files
 *   zfs_dump get <archive.zfs> <name> <out>— extract one file by name
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

/* -----------------------------------------------------------------------
 * Structures
 * ----------------------------------------------------------------------- */

typedef struct {
    char     name[16];
    uint32_t offset;
    uint32_t index;
    uint32_t size;
    uint32_t timestamp;
    uint32_t flags;
} ZFSEntry;

typedef struct {
    uint32_t  entries_per_block;
    uint32_t  total_count;
    ZFSEntry *entries;  /* heap-allocated, total_count entries */
} ZFS;

/* -----------------------------------------------------------------------
 * Parser
 * ----------------------------------------------------------------------- */

static ZFS *zfs_open(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }

    /* Main header — 28 bytes */
    uint8_t hdr[28];
    if (fread(hdr, 1, 28, f) != 28) { fprintf(stderr, "Short header\n"); fclose(f); return NULL; }

    if (memcmp(hdr, "ZFSF", 4) != 0) {
        fprintf(stderr, "Not a ZFSF archive\n"); fclose(f); return NULL;
    }

    uint32_t epb, count, first_block;
    memcpy(&epb,         hdr + 12, 4);
    memcpy(&count,       hdr + 16, 4);
    memcpy(&first_block, hdr + 24, 4);

    ZFS *z = malloc(sizeof(ZFS));
    z->entries_per_block = epb;
    z->total_count       = count;
    z->entries           = malloc(count * sizeof(ZFSEntry));

    uint32_t live    = 0;    /* non-deleted entries written so far */
    uint32_t scanned = 0;    /* raw entries walked (includes deleted) */
    long     block_pos = (long)first_block;

    while (scanned < count) {
        fseek(f, block_pos, SEEK_SET);

        /* First 4 bytes of each block = absolute offset of the next block
         * (0 on the last block). This is the linked-list pointer. */
        uint32_t next_block;
        fread(&next_block, 4, 1, f);

        uint32_t n = (count - scanned < epb) ? (count - scanned) : epb;

        for (uint32_t i = 0; i < n; i++) {
            ZFSEntry e;
            fread(e.name,       1,  16, f);
            fread(&e.offset,    4,   1, f);
            fread(&e.index,     4,   1, f);
            fread(&e.size,      4,   1, f);
            fread(&e.timestamp, 4,   1, f);
            fread(&e.flags,     4,   1, f);

            if (e.name[0] == '\0') continue;  /* empty slot */
            if (e.flags & 1)       continue;  /* deleted */

            z->entries[live++] = e;
        }

        scanned += n;
        block_pos = (long)next_block;
    }

    z->total_count = live;  /* update to live count after filtering deleted */

    fclose(f);
    return z;
}

static void zfs_free(ZFS *z)
{
    if (z) { free(z->entries); free(z); }
}

static ZFSEntry *zfs_find(ZFS *z, const char *name)
{
    for (uint32_t i = 0; i < z->total_count; i++) {
        if (strcasecmp(z->entries[i].name, name) == 0)
            return &z->entries[i];
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * Extraction
 * ----------------------------------------------------------------------- */

static int extract_entry(FILE *zfs_f, ZFSEntry *e, const char *out_path)
{
    FILE *out = fopen(out_path, "wb");
    if (!out) { perror(out_path); return -1; }

    fseek(zfs_f, (long)e->offset, SEEK_SET);

    uint8_t buf[65536];
    uint32_t remaining = e->size;
    while (remaining > 0) {
        uint32_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
        size_t  got   = fread(buf, 1, chunk, zfs_f);
        if (got == 0) break;
        fwrite(buf, 1, got, out);
        remaining -= (uint32_t)got;
    }
    fclose(out);
    return 0;
}

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

/* -----------------------------------------------------------------------
 * Commands
 * ----------------------------------------------------------------------- */

static int cmd_list(const char *zfs_path)
{
    ZFS *z = zfs_open(zfs_path);
    if (!z) return 1;
    printf("%u files in %s\n\n", z->total_count, zfs_path);
    for (uint32_t i = 0; i < z->total_count; i++) {
        ZFSEntry *e = &z->entries[i];
        printf("%6u  %10u  %08x  %s\n", e->index, e->size, e->flags, e->name);
    }
    zfs_free(z);
    return 0;
}

static int cmd_extract(const char *zfs_path, const char *dest_dir)
{
    ZFS *z = zfs_open(zfs_path);
    if (!z) return 1;

    mkdir_p(dest_dir);

    FILE *zfs_f = fopen(zfs_path, "rb");
    if (!zfs_f) { perror(zfs_path); zfs_free(z); return 1; }

    uint32_t ok = 0, fail = 0;
    for (uint32_t i = 0; i < z->total_count; i++) {
        ZFSEntry *e = &z->entries[i];
        if (e->size == 0) continue;

        char out[1024];
        snprintf(out, sizeof(out), "%s/%s", dest_dir, e->name);

        if (extract_entry(zfs_f, e, out) == 0) {
            ok++;
        } else {
            fail++;
        }

        if ((i + 1) % 100 == 0 || i + 1 == z->total_count)
            fprintf(stderr, "\r  %u / %u", i + 1, z->total_count);
    }
    fprintf(stderr, "\n");

    fclose(zfs_f);
    zfs_free(z);
    fprintf(stderr, "Extracted %u files (%u failed) → %s\n", ok, fail, dest_dir);
    return fail > 0 ? 1 : 0;
}

static int cmd_get(const char *zfs_path, const char *name, const char *out_path)
{
    ZFS *z = zfs_open(zfs_path);
    if (!z) return 1;

    ZFSEntry *e = zfs_find(z, name);
    if (!e) {
        fprintf(stderr, "zfs_dump: '%s' not found in archive\n", name);
        zfs_free(z);
        return 1;
    }

    FILE *zfs_f = fopen(zfs_path, "rb");
    if (!zfs_f) { perror(zfs_path); zfs_free(z); return 1; }

    int rc = extract_entry(zfs_f, e, out_path);
    if (rc == 0)
        fprintf(stderr, "Extracted '%s' (%u bytes) → %s\n", name, e->size, out_path);

    fclose(zfs_f);
    zfs_free(z);
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
