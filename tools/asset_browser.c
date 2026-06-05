/*
 * asset_browser.c — GUI asset browser for Interstate '76 Nitro Pack
 *
 * Two-panel SDL2 window: scrollable file list (left) + asset preview (right).
 *
 * Controls:
 *   ↑ / ↓ / PgUp / PgDn / Home / End  — navigate list
 *   Type characters                    — live filename filter
 *   Backspace                          — delete last filter char
 *   Escape                             — clear filter (or quit if empty)
 *   Enter / Space                      — play WAV (toggle); PCX already shown
 *   Mouse click                        — select row (double-click = activate)
 *   Mouse wheel                        — scroll list
 *   Q                                  — quit
 *
 * Build dep: sdl2_ttf (sudo pacman -S sdl2_ttf)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp, strcasestr */
#include <unistd.h>    /* access() */
#include "SDL.h"
#include "SDL_ttf.h"

#include "../src/engine/fs.h"
#include "../src/engine/vfs.h"
#include "../src/engine/pcx.h"

/* -------------------------------------------------------------------------
 * Layout constants
 * ------------------------------------------------------------------------- */

#define WIN_W       1100
#define WIN_H        700
#define LIST_W       310
#define DIVIDER_W      2
#define HEADER_H      32
#define FILTERBAR_H   26
#define FOOTER_H      22
#define ROW_H         20
#define PAD            6
#define FONT_SIZE     13
#define FONT_SIZE_SM  11

/* -------------------------------------------------------------------------
 * Colours
 * ------------------------------------------------------------------------- */

static const SDL_Color COL_BG      = { 0x18,0x18,0x1c,0xff };
static const SDL_Color COL_PANEL   = { 0x20,0x20,0x28,0xff };
static const SDL_Color COL_HEADER  = { 0x14,0x14,0x1e,0xff };
static const SDL_Color COL_SEL     = { 0x1a,0x3c,0x60,0xff };
static const SDL_Color COL_HOVER   = { 0x28,0x28,0x38,0xff };
static const SDL_Color COL_FILTER  = { 0x16,0x16,0x22,0xff };
static const SDL_Color COL_DIV     = { 0x38,0x38,0x50,0xff };
static const SDL_Color COL_TEXT    = { 0xc8,0xc8,0xc8,0xff };
static const SDL_Color COL_BRIGHT  = { 0xff,0xff,0xff,0xff };
static const SDL_Color COL_DIM     = { 0x58,0x58,0x68,0xff };
static const SDL_Color COL_ACCENT  = { 0xe0,0x9a,0x1c,0xff };
static const SDL_Color COL_ZFS     = { 0x44,0x99,0x44,0xff };
static const SDL_Color COL_PLAYING = { 0x40,0xc0,0x60,0xff };

/* -------------------------------------------------------------------------
 * File table
 * ------------------------------------------------------------------------- */

typedef struct { char name[32]; int src_type; } FileEntry;

static FileEntry *g_files      = NULL;
static int        g_file_count = 0;

static void collect_cb(const char *name, int src_type, void *ud)
{
    (void)ud;
    if (g_file_count % 512 == 0)
        g_files = realloc(g_files, (size_t)(g_file_count + 512) * sizeof(FileEntry));
    strncpy(g_files[g_file_count].name, name, 31);
    g_files[g_file_count].name[31] = '\0';
    g_files[g_file_count].src_type  = src_type;
    g_file_count++;
}

/* -------------------------------------------------------------------------
 * Font
 * ------------------------------------------------------------------------- */

static TTF_Font *g_font   = NULL;
static TTF_Font *g_font_s = NULL;   /* small, for tags */
static int       g_fh     = ROW_H;  /* font line height */

static int font_init(void)
{
    if (TTF_Init() < 0) return 0;

    char path[512] = {0};
    FILE *fp = popen("fc-match --format='%{file}' :spacing=mono", "r");
    if (fp) {
        if (fgets(path, sizeof(path), fp))
            path[strcspn(path, "\r\n")] = 0;
        pclose(fp);
    }
    /* Fallbacks for when fc-match isn't available */
    if (!path[0] || access(path, R_OK) != 0) {
        static const char *fb[] = {
            "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
            "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf",
            NULL
        };
        for (int i = 0; fb[i]; i++) {
            if (access(fb[i], R_OK) == 0) { strncpy(path, fb[i], 511); break; }
        }
    }
    if (!path[0]) { fprintf(stderr, "[browser] No font found\n"); return 0; }

    g_font   = TTF_OpenFont(path, FONT_SIZE);
    g_font_s = TTF_OpenFont(path, FONT_SIZE_SM);
    if (!g_font) { fprintf(stderr, "[browser] TTF_OpenFont: %s\n", TTF_GetError()); return 0; }
    g_fh = TTF_FontLineSkip(g_font);
    return 1;
}

/* -------------------------------------------------------------------------
 * Drawing helpers
 * ------------------------------------------------------------------------- */

static void fill(SDL_Renderer *r, int x, int y, int w, int h, SDL_Color c)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_Rect rc = {x, y, w, h};
    SDL_RenderFillRect(r, &rc);
}

/* Render text clipped to [x, y, max_w]. Returns rendered width. */
static int draw_text_clipped(SDL_Renderer *r, TTF_Font *font,
                              const char *text, int x, int y, int max_w,
                              SDL_Color col)
{
    if (!text || !*text || max_w <= 0) return 0;
    SDL_Surface *s = TTF_RenderUTF8_Blended(font, text, col);
    if (!s) return 0;
    SDL_Texture *t = SDL_CreateTextureFromSurface(r, s);
    int tw = s->w, th = s->h;
    SDL_FreeSurface(s);
    if (!t) return 0;
    SDL_Rect src = {0, 0, SDL_min(tw, max_w), th};
    SDL_Rect dst = {x, y, src.w, src.h};
    SDL_RenderCopy(r, t, &src, &dst);
    SDL_DestroyTexture(t);
    return SDL_min(tw, max_w);
}

static void draw_text(SDL_Renderer *r, TTF_Font *font, const char *text,
                      int x, int y, SDL_Color col)
{
    draw_text_clipped(r, font, text, x, y, 9999, col);
}

/* Right-align text ending at x. */
static void draw_text_right(SDL_Renderer *r, TTF_Font *font, const char *text,
                             int x, int y, SDL_Color col)
{
    int tw, th; TTF_SizeUTF8(font, text, &tw, &th);
    draw_text_clipped(r, font, text, x - tw, y, tw, col);
}

/* -------------------------------------------------------------------------
 * Browser state
 * ------------------------------------------------------------------------- */

typedef struct {
    int   *filtered;
    int    fcount;
    int    sel;
    int    scroll;
    int    hover;

    char   filter[128];
    int    filter_len;

    SDL_Texture *pcx_tex;
    int          pcx_w, pcx_h;
    char         pcx_name[32];

    SDL_AudioDeviceID audio_dev;
    int               audio_playing;
    char              audio_name[32];
    char              audio_info[128];

    /* hex dump of current non-image/audio file */
    uint8_t  hex_buf[128];
    size_t   hex_size;
    size_t   hex_total;
    char     hex_name[32];

    char status[256];
} State;

static State G;

/* -------------------------------------------------------------------------
 * Filter
 * ------------------------------------------------------------------------- */

static void rebuild_filter(void)
{
    free(G.filtered);
    G.filtered = malloc((size_t)(g_file_count + 1) * sizeof(int));
    G.fcount   = 0;
    for (int i = 0; i < g_file_count; i++) {
        if (!G.filter_len || strcasestr(g_files[i].name, G.filter))
            G.filtered[G.fcount++] = i;
    }
    if (G.sel >= G.fcount) G.sel = G.fcount - 1;
    if (G.sel < 0)         G.sel = 0;
    G.scroll = 0;
}

/* -------------------------------------------------------------------------
 * Audio
 * ------------------------------------------------------------------------- */

static void audio_stop(void)
{
    if (G.audio_dev) {
        SDL_CloseAudioDevice(G.audio_dev);
        G.audio_dev     = 0;
        G.audio_playing = 0;
    }
}

static void audio_play(const char *name)
{
    audio_stop();
    size_t sz;
    void *data = vfs_read_file(name, &sz);
    if (!data) { snprintf(G.status, sizeof(G.status), "Read failed: %s", name); return; }

    SDL_AudioSpec spec;
    Uint8 *buf = NULL; Uint32 len = 0;
    SDL_RWops *rw = SDL_RWFromMem(data, (int)sz);
    if (!SDL_LoadWAV_RW(rw, 1, &spec, &buf, &len)) {
        snprintf(G.status, sizeof(G.status), "Bad WAV: %s", SDL_GetError());
        vfs_free(data); return;
    }
    vfs_free(data);

    G.audio_dev = SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0);
    if (!G.audio_dev) {
        snprintf(G.status, sizeof(G.status), "Audio: %s", SDL_GetError());
        SDL_FreeWAV(buf); return;
    }
    SDL_QueueAudio(G.audio_dev, buf, len);
    SDL_FreeWAV(buf);
    SDL_PauseAudioDevice(G.audio_dev, 0);
    G.audio_playing = 1;

    strncpy(G.audio_name, name, 31);
    int bps = SDL_AUDIO_BITSIZE(spec.format) / 8;
    double dur = (double)len / (spec.freq * spec.channels * bps);
    snprintf(G.audio_info, sizeof(G.audio_info),
             "%.2f s  |  %d Hz  |  %d ch", dur, spec.freq, spec.channels);
    snprintf(G.status, sizeof(G.status), "Playing: %s", name);
}

/* -------------------------------------------------------------------------
 * PCX preview
 * ------------------------------------------------------------------------- */

static void pcx_clear(void)
{
    if (G.pcx_tex) { SDL_DestroyTexture(G.pcx_tex); G.pcx_tex = NULL; }
    G.pcx_name[0] = '\0';
}

static void pcx_load_preview(SDL_Renderer *rend, const char *name)
{
    pcx_clear();
    PcxImage *img = pcx_load(name);
    if (!img) return;

    SDL_Surface *surf = SDL_CreateRGBSurface(0, (int)img->width, (int)img->height, 8, 0,0,0,0);
    if (!surf) { pcx_free(img); return; }
    SDL_Color pal[256];
    for (int i = 0; i < 256; i++) {
        pal[i] = (SDL_Color){ img->palette[i*3], img->palette[i*3+1],
                               img->palette[i*3+2], 255 };
    }
    SDL_SetPaletteColors(surf->format->palette, pal, 0, 256);
    if (SDL_MUSTLOCK(surf)) SDL_LockSurface(surf);
    memcpy(surf->pixels, img->pixels, img->width * img->height);
    if (SDL_MUSTLOCK(surf)) SDL_UnlockSurface(surf);

    G.pcx_tex = SDL_CreateTextureFromSurface(rend, surf);
    G.pcx_w   = (int)img->width;
    G.pcx_h   = (int)img->height;
    strncpy(G.pcx_name, name, 31);
    SDL_FreeSurface(surf);
    pcx_free(img);
}

/* -------------------------------------------------------------------------
 * Hex preview
 * ------------------------------------------------------------------------- */

static void hex_load(const char *name)
{
    G.hex_size = 0;
    strncpy(G.hex_name, name, 31);
    size_t total;
    void *data = vfs_read_file(name, &total);
    if (!data) return;
    G.hex_total = total;
    G.hex_size  = total < sizeof(G.hex_buf) ? total : sizeof(G.hex_buf);
    memcpy(G.hex_buf, data, G.hex_size);
    vfs_free(data);
}

/* -------------------------------------------------------------------------
 * Selection change
 * ------------------------------------------------------------------------- */

static void on_select(SDL_Renderer *rend, int new_sel)
{
    if (new_sel < 0 || new_sel >= G.fcount) return;
    G.sel = new_sel;
    audio_stop();
    pcx_clear();
    G.hex_size  = 0;

    const char *name = g_files[G.filtered[new_sel]].name;
    const char *ext  = strrchr(name, '.'); ext = ext ? ext + 1 : "";
    snprintf(G.status, sizeof(G.status), "%s", name);

    if      (strcasecmp(ext, "pcx") == 0) pcx_load_preview(rend, name);
    else if (strcasecmp(ext, "wav") == 0) { /* info shown; play on Enter */ }
    else                                   hex_load(name);
}

static void on_activate(void)
{
    if (!G.fcount) return;
    const char *name = g_files[G.filtered[G.sel]].name;
    const char *ext  = strrchr(name, '.'); ext = ext ? ext + 1 : "";
    if (strcasecmp(ext, "wav") == 0) {
        if (G.audio_playing) audio_stop();
        else audio_play(name);
    }
}

/* -------------------------------------------------------------------------
 * Visible rows
 * ------------------------------------------------------------------------- */

static int list_top(void)   { return HEADER_H + FILTERBAR_H; }
static int list_bottom(int win_h) { return win_h - FOOTER_H; }
static int vrows(int win_h) { return (list_bottom(win_h) - list_top()) / ROW_H; }

static void clamp_scroll(int win_h)
{
    int vr = vrows(win_h);
    if (G.scroll > G.fcount - vr) G.scroll = G.fcount - vr;
    if (G.scroll < 0)              G.scroll = 0;
}

static void ensure_visible(int win_h)
{
    int vr = vrows(win_h);
    if (G.sel < G.scroll)         G.scroll = G.sel;
    if (G.sel >= G.scroll + vr)   G.scroll = G.sel - vr + 1;
    clamp_scroll(win_h);
}

/* -------------------------------------------------------------------------
 * Render
 * ------------------------------------------------------------------------- */

static void render(SDL_Renderer *r, int ww, int wh)
{
    fill(r, 0, 0, ww, wh, COL_BG);

    int vr = vrows(wh);
    int lt = list_top();
    int lb = list_bottom(wh);
    int lh = lb - lt;

    /* ── Header ─────────────────────────────────────────────────── */
    fill(r, 0, 0, ww, HEADER_H, COL_HEADER);
    {
        int ty = (HEADER_H - g_fh) / 2;
        draw_text(r, g_font, "Interstate '76  —  Asset Browser", PAD, ty, COL_ACCENT);
        char buf[64];
        snprintf(buf, sizeof(buf), "%d / %d", G.fcount, g_file_count);
        draw_text_right(r, g_font, buf, ww - PAD, ty, COL_DIM);
    }

    /* ── Filter bar ──────────────────────────────────────────────── */
    fill(r, 0, HEADER_H, LIST_W, FILTERBAR_H, COL_FILTER);
    {
        char fbuf[160];
        snprintf(fbuf, sizeof(fbuf), "filter: %s%s", G.filter, G.filter_len ? "_" : "");
        int ty = HEADER_H + (FILTERBAR_H - g_fh) / 2;
        draw_text_clipped(r, g_font, fbuf, PAD, ty, LIST_W - PAD*2,
                          G.filter_len ? COL_TEXT : COL_DIM);
    }
    fill(r, 0, HEADER_H + FILTERBAR_H - 1, LIST_W, 1, COL_DIV);

    /* ── File list ───────────────────────────────────────────────── */
    fill(r, 0, lt, LIST_W, lh, COL_PANEL);

    for (int row = 0; row < vr; row++) {
        int idx = G.scroll + row;
        if (idx >= G.fcount) break;
        int fi = G.filtered[idx];
        int ry = lt + row * ROW_H;

        SDL_Color bg = (idx == G.sel)   ? COL_SEL
                     : (idx == G.hover) ? COL_HOVER
                     :                    COL_PANEL;
        fill(r, 0, ry, LIST_W, ROW_H, bg);

        SDL_Color nc = (idx == G.sel) ? COL_BRIGHT : COL_TEXT;
        int ty = ry + (ROW_H - g_fh) / 2;
        draw_text_clipped(r, g_font, g_files[fi].name, PAD, ty,
                          LIST_W - 36 - PAD, nc);

        /* ZFS tag */
        if (g_files[fi].src_type == 1) {
            TTF_Font *sf = g_font_s ? g_font_s : g_font;
            int tw, th; TTF_SizeUTF8(sf, "zfs", &tw, &th);
            draw_text(r, sf, "zfs",
                      LIST_W - tw - PAD, ry + (ROW_H - th)/2, COL_ZFS);
        }
    }

    /* Scrollbar */
    if (G.fcount > vr) {
        int th = SDL_max(16, lh * vr / G.fcount);
        int ty = lt + (lh - th) * G.scroll / SDL_max(1, G.fcount - vr);
        fill(r, LIST_W - 4, lt, 4, lh, COL_FILTER);
        fill(r, LIST_W - 4, ty, 4, th, COL_DIM);
    }

    /* ── Divider ─────────────────────────────────────────────────── */
    fill(r, LIST_W, HEADER_H, DIVIDER_W, wh - HEADER_H - FOOTER_H, COL_DIV);

    /* ── Preview panel ───────────────────────────────────────────── */
    int px  = LIST_W + DIVIDER_W;
    int pw  = ww - px;
    int py  = HEADER_H;
    int ph  = wh - HEADER_H - FOOTER_H;

    /* PCX image */
    if (G.pcx_tex) {
        /* Fit image in panel preserving aspect ratio */
        int margin = PAD * 2;
        float scale = (float)(pw - margin) / G.pcx_w;
        if ((int)(G.pcx_h * scale) > ph - margin)
            scale = (float)(ph - margin) / G.pcx_h;
        int dw = (int)(G.pcx_w * scale);
        int dh = (int)(G.pcx_h * scale);
        SDL_Rect dst = { px + (pw - dw)/2, py + (ph - dh)/2, dw, dh };
        SDL_RenderCopy(r, G.pcx_tex, NULL, &dst);

        /* Dim overlay with dimensions at bottom-right of image */
        char dim[32];
        snprintf(dim, sizeof(dim), "%d × %d", G.pcx_w, G.pcx_h);
        draw_text_right(r, g_font_s ? g_font_s : g_font, dim,
                        dst.x + dw - PAD, dst.y + dh + PAD, COL_DIM);
    }

    /* WAV info */
    else if (G.fcount > 0) {
        const char *name = g_files[G.filtered[G.sel]].name;
        const char *ext  = strrchr(name, '.'); ext = ext ? ext + 1 : "";

        int cx = px + PAD;
        int cy = py + ph / 2 - g_fh * 2;

        if (strcasecmp(ext, "wav") == 0) {
            draw_text(r, g_font, name, cx, cy, COL_BRIGHT);
            cy += g_fh + 4;

            if (G.audio_playing) {
                draw_text(r, g_font, G.audio_info,  cx, cy,           COL_DIM);
                cy += g_fh + 8;
                draw_text(r, g_font, "■ PLAYING",   cx, cy,           COL_PLAYING);
                cy += g_fh + 4;
                draw_text(r, g_font, "Enter/Space to stop", cx, cy,   COL_DIM);
            } else {
                cy += g_fh + 8;
                draw_text(r, g_font, "Enter or Space to play", cx, cy, COL_DIM);
            }
        }

        /* Hex dump for unknown types */
        else if (G.hex_size > 0) {
            draw_text(r, g_font, G.hex_name, cx, py + PAD, COL_BRIGHT);

            char szlabel[64];
            snprintf(szlabel, sizeof(szlabel), "%zu bytes", G.hex_total);
            draw_text(r, g_font_s ? g_font_s : g_font, szlabel,
                      cx, py + PAD + g_fh + 2, COL_DIM);

            TTF_Font *hf = g_font_s ? g_font_s : g_font;
            int hfh = TTF_FontLineSkip(hf);
            int hy  = py + PAD + g_fh * 2 + 12;

            for (size_t row = 0; row < G.hex_size && hy < py + ph - PAD; row += 16) {
                char line[80];
                int  len = 0;

                /* Offset */
                len += snprintf(line + len, sizeof(line) - (size_t)len,
                                "%04zx  ", row);

                /* Hex bytes */
                for (int col = 0; col < 16; col++) {
                    if (row + col < G.hex_size)
                        len += snprintf(line + len, sizeof(line) - (size_t)len,
                                        "%02x ", G.hex_buf[row + col]);
                    else
                        len += snprintf(line + len, sizeof(line) - (size_t)len, "   ");
                    if (col == 7) line[len++] = ' ';
                }

                /* ASCII */
                line[len++] = ' '; line[len++] = '|';
                for (int col = 0; col < 16 && row + col < G.hex_size; col++) {
                    uint8_t b = G.hex_buf[row + col];
                    line[len++] = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
                }
                line[len++] = '|'; line[len] = '\0';

                draw_text(r, hf, line, cx, hy, COL_TEXT);
                hy += hfh;
            }
        }
    }

    /* ── Footer ──────────────────────────────────────────────────── */
    int fy = wh - FOOTER_H;
    fill(r, 0, fy, ww, FOOTER_H, COL_HEADER);
    fill(r, 0, fy, ww, 1, COL_DIV);

    int fty = fy + (FOOTER_H - g_fh) / 2;
    if (G.status[0])
        draw_text_clipped(r, g_font, G.status, PAD, fty, ww/2, COL_DIM);
    draw_text_right(r, g_font,
                    "↑↓ navigate  Enter/Space play  type to filter  Esc clear  Q quit",
                    ww - PAD, fty, COL_DIM);

    SDL_RenderPresent(r);
}

/* -------------------------------------------------------------------------
 * Event loop
 * ------------------------------------------------------------------------- */

static int handle_events(SDL_Renderer *r, int ww, int wh)
{
    (void)ww;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        int vr = vrows(wh);

        if (e.type == SDL_QUIT) return 0;

        if (e.type == SDL_KEYDOWN) {
            SDL_Keycode k = e.key.keysym.sym;
            if (k == SDLK_ESCAPE) {
                if (G.filter_len) {
                    G.filter[0] = '\0'; G.filter_len = 0;
                    rebuild_filter(); on_select(r, 0);
                } else return 0;
            }
            else if (k == SDLK_q)        return 0;
            else if (k == SDLK_UP)       { on_select(r, G.sel - 1); ensure_visible(wh); }
            else if (k == SDLK_DOWN)     { on_select(r, G.sel + 1); ensure_visible(wh); }
            else if (k == SDLK_PAGEUP)   { on_select(r, G.sel - vr); ensure_visible(wh); }
            else if (k == SDLK_PAGEDOWN) { on_select(r, G.sel + vr); ensure_visible(wh); }
            else if (k == SDLK_HOME)     { on_select(r, 0);              G.scroll = 0; }
            else if (k == SDLK_END)      { on_select(r, G.fcount - 1); ensure_visible(wh); }
            else if (k == SDLK_RETURN || k == SDLK_SPACE) on_activate();
            else if (k == SDLK_BACKSPACE) {
                if (G.filter_len > 0) {
                    G.filter[--G.filter_len] = '\0';
                    rebuild_filter(); on_select(r, 0);
                }
            }
        }

        if (e.type == SDL_TEXTINPUT) {
            for (char *p = e.text.text; *p && G.filter_len < 127; p++) {
                G.filter[G.filter_len++] = *p;
            }
            G.filter[G.filter_len] = '\0';
            rebuild_filter(); on_select(r, 0);
        }

        if (e.type == SDL_MOUSEWHEEL) {
            G.scroll -= e.wheel.y * 3;
            clamp_scroll(wh);
        }

        if (e.type == SDL_MOUSEMOTION) {
            int mx = e.motion.x, my = e.motion.y;
            if (mx < LIST_W && my >= list_top() && my < list_bottom(wh)) {
                int row = G.scroll + (my - list_top()) / ROW_H;
                G.hover = (row < G.fcount) ? row : -1;
            } else {
                G.hover = -1;
            }
        }

        if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
            int mx = e.button.x, my = e.button.y;
            if (mx < LIST_W && my >= list_top() && my < list_bottom(wh)) {
                int row = G.scroll + (my - list_top()) / ROW_H;
                if (row >= 0 && row < G.fcount) {
                    if (row == G.sel) on_activate();
                    else { on_select(r, row); }
                }
            }
        }
    }

    /* Audio finished? */
    if (G.audio_playing && G.audio_dev &&
        SDL_GetQueuedAudioSize(G.audio_dev) == 0) {
        audio_stop();
        snprintf(G.status, sizeof(G.status), "Done — %s", G.audio_name);
    }

    return 1;
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    const char *asset_root = ".";
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "--assets") == 0) asset_root = argv[i+1];

    fs_set_root(asset_root);
    if (!vfs_init()) { fprintf(stderr, "[browser] VFS init failed\n"); return 1; }

    vfs_foreach(collect_cb, NULL);
    fprintf(stdout, "[browser] %d files indexed\n", g_file_count);

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);

    if (!font_init()) {
        fprintf(stderr, "[browser] Font init failed — install sdl2_ttf\n");
        return 1;
    }

    SDL_Window   *win  = SDL_CreateWindow(
        "Interstate '76 — Asset Browser",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    SDL_Renderer *rend = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);

    SDL_StartTextInput();

    memset(&G, 0, sizeof(G));
    G.hover = -1;
    rebuild_filter();
    if (G.fcount > 0) on_select(rend, 0);

    while (1) {
        int ww, wh;
        SDL_GetWindowSize(win, &ww, &wh);
        if (!handle_events(rend, ww, wh)) break;
        render(rend, ww, wh);
        SDL_Delay(14);
    }

    SDL_StopTextInput();
    audio_stop();
    pcx_clear();
    free(G.filtered);
    free(g_files);
    if (g_font_s) TTF_CloseFont(g_font_s);
    TTF_CloseFont(g_font);
    TTF_Quit();
    SDL_DestroyRenderer(rend);
    SDL_DestroyWindow(win);
    SDL_Quit();
    vfs_shutdown();
    return 0;
}
