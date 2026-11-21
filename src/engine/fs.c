/*
 * fs.c — Filesystem utilities
 */

#include "fs.h"

#include <ctype.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

static char s_root[512] = ".";

void fs_set_root(const char *root)
{
    strncpy(s_root, root, sizeof(s_root) - 1);
    s_root[sizeof(s_root) - 1] = '\0';
    /* Strip trailing slash for consistent joining */
    size_t len = strlen(s_root);
    while (len > 1 && (s_root[len - 1] == '/' || s_root[len - 1] == '\\'))
        s_root[--len] = '\0';
}

static void normalise_separators(char *buf)
{
    for (char *p = buf; *p; p++)
        if (*p == '\\') *p = '/';
}

static void set_basename_case(char *buf, int upper)
{
    char *base = buf;
    for (char *p = buf; *p; p++)
        if (*p == '/') base = p + 1;
    for (char *p = base; *p; p++)
        *p = (char)(upper ? toupper((unsigned char)*p)
                          : tolower((unsigned char)*p));
}

static void build_path(char *buf, size_t bufsz, const char *path)
{
    if (path[0] == '/' || (path[0] && path[1] == ':')) {
        /* Absolute path — use as-is */
        strncpy(buf, path, bufsz - 1);
    } else {
        snprintf(buf, bufsz, "%s/%s", s_root, path);
    }
    buf[bufsz - 1] = '\0';
    normalise_separators(buf);
}

const char *fs_get_root(void) { return s_root; }

FILE *fs_fopen(const char *path, const char *mode)
{
    char buf[1024];
    build_path(buf, sizeof(buf), path);

    FILE *f = fopen(buf, mode);
    if (f) return f;

    set_basename_case(buf, 1); /* try UPPERCASE.EXT */
    f = fopen(buf, mode);
    if (f) return f;

    set_basename_case(buf, 0); /* try lowercase.ext */
    return fopen(buf, mode);
}

int fs_list_dir(const char *path, void (*fn)(const char *name, int is_dir, void *ud), void *ud)
{
#ifdef _WIN32
    /* UNTESTED: Windows build not yet exercised. */
    char pat[1024];
    snprintf(pat, sizeof pat, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        fn(fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, ud);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 1;
#else
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        struct stat st;
        int is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        fn(e->d_name, is_dir, ud);
    }
    closedir(d);
    return 1;
#endif
}
