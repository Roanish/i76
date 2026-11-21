/*
 * mods.c — presentation-only mod override index (see mods.h for the scope rule).
 *
 * At startup we walk each enabled mod folder recursively and index every file
 * by its lower-cased base name. Higher-priority mods are scanned first; a name
 * already indexed by a higher-priority mod is shadowed (logged). Lookups are a
 * binary search over the sorted index.
 */

#include "engine/mods.h"
#include "engine/fs.h"
#include "engine/pix.h"   /* vfs_lod — the base (archive) resolver */
#include "engine/vfs.h"   /* vfs_read_file — the base direct-file resolver */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 8

typedef struct {
    char key[64];      /* lower-cased base name, e.g. "wa11rwl3.geo" */
    char path[1024];   /* full path on disk                           */
    int  prio;         /* 0 = highest-priority mod                    */
    const char *mod;   /* owning mod folder name (s_mods)             */
} ModFile;

static ModFile *s_files;
static int      s_nfiles, s_capfiles;

static char   (*s_mods)[256];   /* enabled mod folder names, priority order */
static int      s_nmods;

static void lower_copy(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; i + 1 < n && src[i]; i++) dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = '\0';
}

/* ---- index build ------------------------------------------------------- */

typedef struct { const char *dir; int prio; const char *mod; int depth; } ScanCtx;

static void scan_dir(const char *dir, int prio, const char *mod, int depth);

static void scan_entry(const char *name, int is_dir, void *ud)
{
    ScanCtx *c = ud;
    char full[1024];
    snprintf(full, sizeof full, "%s/%s", c->dir, name);
    if (is_dir) {
        if (c->depth < MAX_DEPTH) scan_dir(full, c->prio, c->mod, c->depth + 1);
        return;
    }
    if (strlen(name) >= sizeof s_files[0].key) {
        fprintf(stderr, "[mods] %s: name too long, ignored: %s\n", c->mod, name);
        return;
    }
    if (s_nfiles == s_capfiles) {
        s_capfiles = s_capfiles ? s_capfiles * 2 : 256;
        s_files = realloc(s_files, sizeof *s_files * (size_t)s_capfiles);
    }
    ModFile *f = &s_files[s_nfiles++];
    lower_copy(f->key, sizeof f->key, name);
    snprintf(f->path, sizeof f->path, "%s", full);
    f->prio = c->prio;
    f->mod  = c->mod;
}

static void scan_dir(const char *dir, int prio, const char *mod, int depth)
{
    ScanCtx c = { dir, prio, mod, depth };
    fs_list_dir(dir, scan_entry, &c);
}

static int cmp_file(const void *a, const void *b)
{
    const ModFile *x = a, *y = b;
    int k = strcmp(x->key, y->key);
    return k ? k : x->prio - y->prio;
}

static void add_mod(const char *name)
{
    for (int i = 0; i < s_nmods; i++)
        if (strcmp(s_mods[i], name) == 0) return;          /* listed twice */
    s_mods = realloc(s_mods, sizeof *s_mods * (size_t)(s_nmods + 1));
    snprintf(s_mods[s_nmods++], sizeof s_mods[0], "%s", name);
}

static void collect_subdir(const char *name, int is_dir, void *ud)
{
    (void)ud;
    if (is_dir) add_mod(name);
}

static int cmp_name(const void *a, const void *b) { return strcmp(a, b); }

static void read_load_order(const char *root)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/load_order.txt", root);
    FILE *f = fopen(p, "r");
    if (!f) {
        /* No explicit order: every subfolder, alphabetical. */
        fs_list_dir(root, collect_subdir, NULL);
        if (s_nmods > 1) qsort(s_mods, (size_t)s_nmods, sizeof s_mods[0], cmp_name);
        return;
    }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        char *e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
        if (!*s || *s == '#') continue;
        add_mod(s);
    }
    fclose(f);
}

void mods_init(const char *mods_root)
{
    mods_shutdown();
    if (!mods_root || !*mods_root) return;

    read_load_order(mods_root);
    if (s_nmods == 0) {
        fprintf(stdout, "[mods] none active (root %s)\n", mods_root);
        return;
    }
    for (int i = 0; i < s_nmods; i++) {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s/%s", mods_root, s_mods[i]);
        int before = s_nfiles;
        scan_dir(dir, i, s_mods[i], 0);
        fprintf(stdout, "[mods] #%d %s: %d files\n", i, s_mods[i], s_nfiles - before);
    }

    /* Sort by (name, priority) and keep the highest-priority copy of each. */
    if (s_nfiles > 1) qsort(s_files, (size_t)s_nfiles, sizeof *s_files, cmp_file);
    int w = 0;
    for (int r = 0; r < s_nfiles; r++) {
        if (w > 0 && strcmp(s_files[w - 1].key, s_files[r].key) == 0) {
            fprintf(stdout, "[mods] %s: %s shadowed by %s\n",
                    s_files[r].key, s_files[r].mod, s_files[w - 1].mod);
            continue;
        }
        s_files[w++] = s_files[r];
    }
    s_nfiles = w;
    fprintf(stdout, "[mods] %d mod(s), %d override file(s) from %s\n",
            s_nmods, s_nfiles, mods_root);
}

void mods_shutdown(void)
{
    free(s_files); s_files = NULL; s_nfiles = s_capfiles = 0;
    free(s_mods);  s_mods  = NULL; s_nmods  = 0;
}

int mods_count(void) { return s_nfiles; }

/* ---- lookup ------------------------------------------------------------ */

static const ModFile *find(const char *name)
{
    if (!s_nfiles || !name || !*name) return NULL;
    /* Match on the base name: "addon/LOADGAME.PCX" -> "loadgame.pcx". */
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\') name = p + 1;
    char key[64];
    lower_copy(key, sizeof key, name);
    int lo = 0, hi = s_nfiles - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(key, s_files[mid].key);
        if (c == 0) return &s_files[mid];
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return NULL;
}

const char *mods_find(const char *name)
{
    const ModFile *f = find(name);
    return f ? f->path : NULL;
}

void *mods_read(const char *name, size_t *out_size)
{
    if (out_size) *out_size = 0;
    const ModFile *mf = find(name);
    if (!mf) return NULL;
    FILE *f = fopen(mf->path, "rb");
    if (!f) {
        fprintf(stderr, "[mods] cannot open %s\n", mf->path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(n > 0 ? (size_t)n : 1);
    if (!buf || (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n)) {
        free(buf);
        fclose(f);
        fprintf(stderr, "[mods] read failed: %s\n", mf->path);
        return NULL;
    }
    fclose(f);
    if (out_size) *out_size = (size_t)n;
    fprintf(stdout, "[mods] %s <- %s\n", name, mf->mod);
    return buf;
}

void *asset_present_read(const char *name, size_t *out_size)
{
    void *b = mods_read(name, out_size);
    return b ? b : vfs_lod(name, out_size);
}

void *asset_present_file(const char *name, size_t *out_size)
{
    void *b = mods_read(name, out_size);
    return b ? b : vfs_read_file(name, out_size);
}
