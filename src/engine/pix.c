/*
 * pix.c — global PIX/PAK record index (see pix.h).
 *
 * Mirrors the original pix_init (FUN_00467cd0) / vfs_lod_pak (PIX/PAK resolver):
 *   - glob *.pix via the VFS,
 *   - for each, the owning pak is the same base name + ".pak",
 *   - parse the text body ("<count>" then "NAME.ext off len" lines),
 *   - key each record by its 8-char uppercased base name,
 *   - sort the global array so lookups are a bsearch.
 */

#include "engine/pix.h"
#include "engine/vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

/* ----------------------------------------------------------------------- */

typedef struct {
    char     key[9];      /* 8-char uppercased base name, null-terminated   */
    int      pak;         /* index into s_paks                              */
    uint32_t off;         /* byte offset into the decompressed pak          */
    uint32_t len;         /* record byte length                             */
} PixEntry;

#define MAX_PAKS 4096

static PixEntry  *s_ent;
static int        s_count;
static int        s_cap;

static char     (*s_paks)[24];   /* pak filenames, e.g. "anbunk1g.pak"      */
static int        s_pak_count;

static bool       s_inited;

/* Small most-recently-used cache of decompressed paks (avoids re-inflating a
 * pak for every record it contains). */
#define PAK_CACHE 6
static struct { int pak; void *data; size_t size; } s_cache[PAK_CACHE];
static int s_cache_next;

/* ----------------------------------------------------------------------- */
/* Key helper: strip extension, uppercase, max 8 chars.                      */
/* ----------------------------------------------------------------------- */

static void make_key(const char *name, char out[9])
{
    int i = 0;
    for (; i < 8 && name[i] && name[i] != '.'; i++)
        out[i] = (char)toupper((unsigned char)name[i]);
    out[i] = '\0';
}

static int ent_cmp(const void *a, const void *b)
{
    return strcmp(((const PixEntry *)a)->key, ((const PixEntry *)b)->key);
}

/* ----------------------------------------------------------------------- */
/* Index construction                                                        */
/* ----------------------------------------------------------------------- */

static void add_entry(const char *recname, int pak, uint32_t off, uint32_t len)
{
    if (s_count == s_cap) {
        s_cap = s_cap ? s_cap * 2 : 4096;
        s_ent = realloc(s_ent, sizeof(*s_ent) * s_cap);
    }
    PixEntry *e = &s_ent[s_count++];
    make_key(recname, e->key);
    e->pak = pak;
    e->off = off;
    e->len = len;
}

/* Parse one .pix (already loaded as text) into the global array. */
static void parse_pix(const char *pix_name, const char *text, size_t size)
{
    /* Register the owning pak: same base, ".pak" extension. */
    if (s_pak_count >= MAX_PAKS) return;
    int pak = s_pak_count++;
    char *pakf = s_paks[pak];
    size_t n = 0;
    for (; pix_name[n] && pix_name[n] != '.' && n < 19; n++)
        pakf[n] = pix_name[n];
    snprintf(pakf + n, 24 - n, ".pak");

    /* Body: first token = count, then "NAME off len" lines. */
    const char *p = text;
    const char *end = text + size;
    /* skip the count line */
    while (p < end && *p != '\n') p++;
    if (p < end) p++;

    while (p < end) {
        char nm[32]; long off, len;
        /* sscanf stops at whitespace; \r is whitespace-ish, tolerated. */
        int got = sscanf(p, "%31s %ld %ld", nm, &off, &len);
        if (got == 3)
            add_entry(nm, pak, (uint32_t)off, (uint32_t)len);
        /* advance to next line */
        while (p < end && *p != '\n') p++;
        if (p < end) p++;
    }
}

/* vfs_foreach callback: collect .pix names. We parse in a second pass so we
 * aren't reading files from inside the iterator. */
struct collect { char (*names)[24]; int count; int cap; };

static void collect_pix(const char *name, int src_type, void *ud)
{
    (void)src_type;
    size_t l = strlen(name);
    if (l < 4 || l >= 24) return;
    if (strcasecmp(name + l - 4, ".pix") != 0) return;
    struct collect *c = ud;
    if (c->count == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 256;
        c->names = realloc(c->names, sizeof(*c->names) * c->cap);
    }
    snprintf(c->names[c->count++], 24, "%s", name);
}

void pix_init(void)
{
    if (s_inited) return;

    s_paks = malloc(sizeof(*s_paks) * MAX_PAKS);

    struct collect c = {0};
    vfs_foreach(collect_pix, &c);

    for (int i = 0; i < c.count; i++) {
        size_t sz = 0;
        void *txt = vfs_read_file(c.names[i], &sz);
        if (!txt) continue;
        parse_pix(c.names[i], (const char *)txt, sz);
        vfs_free(txt);
    }
    free(c.names);

    qsort(s_ent, s_count, sizeof(*s_ent), ent_cmp);

    s_inited = true;
    fprintf(stderr, "[pix] indexed %d records across %d paks\n",
            s_count, s_pak_count);
}

void pix_shutdown(void)
{
    for (int i = 0; i < PAK_CACHE; i++) {
        free(s_cache[i].data);
        s_cache[i].data = NULL;
        s_cache[i].pak = -1;
    }
    free(s_ent);   s_ent = NULL;   s_count = s_cap = 0;
    free(s_paks);  s_paks = NULL;  s_pak_count = 0;
    s_inited = false;
}

int pix_count(void) { return s_count; }

/* ----------------------------------------------------------------------- */
/* Resolution                                                                */
/* ----------------------------------------------------------------------- */

static const PixEntry *find(const char *name)
{
    char key[9];
    make_key(name, key);
    PixEntry probe;
    memcpy(probe.key, key, sizeof(key));
    return bsearch(&probe, s_ent, s_count, sizeof(*s_ent), ent_cmp);
}

/* Load (or fetch cached) the decompressed pak `pak`. */
static const void *load_pak(int pak, size_t *size_out)
{
    for (int i = 0; i < PAK_CACHE; i++)
        if (s_cache[i].data && s_cache[i].pak == pak) {
            *size_out = s_cache[i].size;
            return s_cache[i].data;
        }
    size_t sz = 0;
    void *data = vfs_read_file(s_paks[pak], &sz);
    if (!data) return NULL;

    int slot = s_cache_next;
    s_cache_next = (s_cache_next + 1) % PAK_CACHE;
    free(s_cache[slot].data);
    s_cache[slot].pak = pak;
    s_cache[slot].data = data;
    s_cache[slot].size = sz;
    *size_out = sz;
    return data;
}

void *vfs_lod(const char *name, size_t *out_size)
{
    if (out_size) *out_size = 0;
    if (!name || !*name) return NULL;

    if (s_inited) {
        const PixEntry *e = find(name);
        if (e) {
            size_t paksz = 0;
            const void *pak = load_pak(e->pak, &paksz);
            if (pak && (size_t)e->off + e->len <= paksz) {
                void *rec = malloc(e->len ? e->len : 1);
                memcpy(rec, (const char *)pak + e->off, e->len);
                if (out_size) *out_size = e->len;
                return rec;
            }
            /* index hit but pak load/bounds failed — fall through to loose */
        }
    }

    return vfs_read_file(name, out_size);
}
