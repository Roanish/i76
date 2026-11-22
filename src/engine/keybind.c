/*
 * keybind.c — the original input binding system (see keybind.h).
 * Reference: docs/REVERSING.md "Input bindings".
 */

#include "engine/keybind.h"
#include "engine/fs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_TERMS    8      /* "chord too big" above 8 */
#define MAX_CHORDS 256
#define MAX_GAMEKEYS 64

/* Virtual "either side" channels, set from both left and right keys. */
#define CH_SHIFT   118
#define CH_CONTROL 119

typedef struct { int ch; int exclude; } Term;
typedef struct { int action; int nterms; Term t[MAX_TERMS]; } Chord;

typedef struct {
    int bound;            /* driven by keys this session                   */
    int value;            /* 16.16 in [-1, 1]                              */
    int vel;              /* 16.16 velocity (unused when alias)            */
    int alias;            /* "_delta" sink: velocity IS the value          */
    int dflt_value;
} Sink;

static Chord s_chords[MAX_CHORDS];
static int   s_nchords;
static bool  s_held[KB_NUM_ACTIONS];
static int   s_plus_of[KB_NUM_ACTIONS];    /* button -> axis it drives (+), or -1 */
static int   s_minus_of[KB_NUM_ACTIONS];   /* button -> axis it drives (-), or -1 */
static Sink  s_sink[KB_NUM_ACTIONS];

typedef struct { int cmd; uint8_t hid; uint8_t ctrl, alt, shift; } GameKey;
static GameKey s_gk[MAX_GAMEKEYS];
static int     s_ngk;
static int     s_cmdq[16], s_ncmd, s_cmdpos;

/* ----------------------------------------------------------------------- */

int keybind_action(const char *name)
{
    for (int i = 0; i < KB_NUM_ACTIONS; i++)
        if (strcasecmp(KB_ACTIONS[i].name, name) == 0) return i;
    return -1;
}

int keybind_command_id(const char *name)
{
    for (const KbCommandDef *c = KB_COMMANDS; c->name; c++)
        if (strcasecmp(c->name, name) == 0) return c->id;
    return 0;
}

static int channel_by_name(const char *name)
{
    for (int i = 0; i < KB_NUM_CHANNELS; i++) {
        const char *n = KB_CHANNELS[i].name;
        if (n[0] == '*') n++;                 /* "*Esc" */
        if (strcasecmp(n, name) == 0) return i;
    }
    return -1;
}

/* Sink init: value = ((dflt<<16) - (min<<16))*2/range - 1.0 */
static void sink_init(int a)
{
    const KbActionDef *d = &KB_ACTIONS[a];
    Sink *k = &s_sink[a];
    int range = d->max - d->min;
    k->dflt_value = range ? (int)((((long long)d->dflt << 16) - ((long long)d->min << 16)) * 2 / range) - 0x10000
                          : 0;
    k->value = k->dflt_value;
    k->vel   = 0;
    k->alias = strstr(d->name, "_delta") != NULL;
}

/* X_plus / X_minus -> the axis X, else X_delta. */
static void link_plus_minus(int a)
{
    char base[48];
    const char *n = KB_ACTIONS[a].name;
    const char *suf = NULL;
    int sign = 0;
    if ((suf = strstr(n, "_plus")))  sign = +1;
    else if ((suf = strstr(n, "_minus"))) sign = -1;
    if (!sign) return;
    snprintf(base, sizeof base, "%.*s", (int)(suf - n), n);
    int ax = keybind_action(base);
    if (ax < 0 || KB_ACTIONS[ax].type != 0) {
        char d[56];
        snprintf(d, sizeof d, "%s_delta", base);
        ax = keybind_action(d);
    }
    if (ax < 0 || KB_ACTIONS[ax].type != 0) return;
    if (sign > 0) s_plus_of[a] = ax; else s_minus_of[a] = ax;
    s_sink[ax].bound = 1;
}

/* ----------------------------------------------------------------------- */
/* input.map                                                                 */
/* ----------------------------------------------------------------------- */

/* Line source: a config file, or an in-memory default (portable — no
 * fmemopen). */
typedef struct { FILE *f; const char *s; } LineSrc;

static char *next_line(char *buf, int n, LineSrc *src)
{
    if (src->f) return fgets(buf, n, src->f);
    if (!src->s || !*src->s) return NULL;
    int i = 0;
    while (*src->s && i < n - 1) {
        char c = *src->s++;
        buf[i++] = c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return buf;
}

/* Strip comments + newline; return 1 if the line has non-space content. */
static int clean_line(char *l)
{
    for (char *p = l; *p; p++)
        if (*p == '#' || *p == '\n' || *p == '\r') { *p = '\0'; break; }
    for (char *p = l; *p; p++)
        if (!isspace((unsigned char)*p)) return 1;
    return 0;
}

static void parse_input_map(LineSrc *f, const char *src)
{
    char line[256];
    int lineno = 0;
    while (next_line(line, sizeof line, f)) {
        lineno++;
        if (!clean_line(line)) continue;
        char name[64];
        if (sscanf(line, "%63s", name) != 1) continue;
        char *brace = strchr(name, '{');
        if (brace) *brace = '\0';
        int a = keybind_action(name);
        if (a < 0)
            fprintf(stderr, "[keys] %s:%d unknown action '%s' (block skipped)\n", src, lineno, name);

        Chord c = { a, 0, {{0}} };
        int usable = 1;
        while (next_line(line, sizeof line, f)) {
            lineno++;
            if (!clean_line(line)) continue;
            char sign[2], dev[32], key[48];
            char *s = line;
            while (isspace((unsigned char)*s)) s++;
            if (*s == '}') break;
            if (sscanf(s, "%1[+-] %31s %47s", sign, dev, key) != 3) {
                fprintf(stderr, "[keys] %s:%d read error\n", src, lineno);
                continue;
            }
            if (strcasecmp(dev, "keyboard") != 0) { usable = 0; continue; } /* joystick/mouse: not yet */
            int ch = channel_by_name(key);
            if (ch < 0) {
                fprintf(stderr, "[keys] %s:%d no key named '%s'\n", src, lineno, key);
                usable = 0;
                continue;
            }
            if (c.nterms >= MAX_TERMS) {
                fprintf(stderr, "[keys] %s:%d chord too big\n", src, lineno);
                usable = 0;
                continue;
            }
            c.t[c.nterms].ch = ch;
            c.t[c.nterms].exclude = sign[0] == '-';
            c.nterms++;
        }
        if (a < 0 || !usable || c.nterms == 0) continue;
        if (KB_ACTIONS[a].type == 0) {
            /* A key bound straight to an axis is an analog channel in the
             * original — keyboards don't provide those. */
            fprintf(stderr, "[keys] %s: '%s' is an axis; bind %s_plus/_minus instead\n",
                    src, name, name);
            continue;
        }
        if (s_nchords < MAX_CHORDS) s_chords[s_nchords++] = c;
        link_plus_minus(a);
    }
}

/* ----------------------------------------------------------------------- */
/* gamekey.map                                                               */
/* ----------------------------------------------------------------------- */

/* Key token -> HID usage. Accepts single characters, common key names and
 * keyboard channel names. */
static int gamekey_hid(const char *k)
{
    if (k[0] && !k[1]) {
        char c = (char)tolower((unsigned char)k[0]);
        if (c >= 'a' && c <= 'z') return 4 + (c - 'a');
        if (c >= '1' && c <= '9') return 30 + (c - '1');
        switch (c) {
        case '0': return 39; case '`': return 53; case '[': return 47; case ']': return 48;
        case '-': return 45; case '=': return 46; case ';': return 51; case '\'': return 52;
        case ',': return 54; case '.': return 55; case '/': return 56; case '\\': return 49;
        }
        return 0;
    }
    if ((k[0] == 'F' || k[0] == 'f') && isdigit((unsigned char)k[1])) {
        int n = atoi(k + 1);
        if (n >= 1 && n <= 12) return n <= 10 ? 57 + n : 68 + (n - 11);
    }
    static const struct { const char *n; int hid; } names[] = {
        { "PAUSE", 72 }, { "Delete", 76 }, { "Insert", 73 }, { "Home", 74 }, { "End", 77 },
        { "PageUp", 75 }, { "PageDown", 78 }, { "Escape", 41 }, { "Space", 44 },
        { "Enter", 40 }, { "Tab", 43 }, { "Backspace", 42 },
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcasecmp(names[i].n, k) == 0) return names[i].hid;
    int ch = channel_by_name(k);
    return ch >= 0 ? KB_CHANNELS[ch].hid : 0;
}

static void parse_gamekey_map(LineSrc *f, const char *src)
{
    char line[256];
    int lineno = 0;
    while (next_line(line, sizeof line, f)) {
        lineno++;
        if (!clean_line(line)) continue;
        char cmd[64], keys[64];
        if (sscanf(line, "%63s %63s", cmd, keys) != 2) continue;
        int id = keybind_command_id(cmd);
        if (!id) { fprintf(stderr, "[keys] %s:%d no gamekey %s\n", src, lineno, cmd); continue; }
        GameKey g = { id, 0, 0, 0, 0 };
        char *tok = strtok(keys, "+");
        char *last = NULL;
        while (tok) {
            char *next = strtok(NULL, "+");
            if (!next) { last = tok; break; }
            if (!strcasecmp(tok, "Control") || !strcasecmp(tok, "CTRL")) g.ctrl = 1;
            else if (!strcasecmp(tok, "ALT"))   g.alt = 1;
            else if (!strcasecmp(tok, "Shift")) g.shift = 1;
            tok = next;
        }
        int hid = last ? gamekey_hid(last) : 0;
        if (!hid) { fprintf(stderr, "[keys] %s:%d unknown key '%s'\n", src, lineno, last ? last : ""); continue; }
        g.hid = (uint8_t)hid;
        if (s_ngk < MAX_GAMEKEYS) s_gk[s_ngk++] = g;
    }
}

/* Our own fallback bindings (used only when a config file is missing):
 * keypad arrows drive, the grey cursor keys move the track camera, PgUp/PgDn
 * its distance, F1/F2 the two main views. */
static const char *DEFAULT_INPUT =
    "throttle_up { \n + keyboard UpArrow \n }\n"
    "throttle_down { \n + keyboard DownArrow \n }\n"
    "steer_left { \n + keyboard LeftArrow \n }\n"
    "steer_right { \n + keyboard RightArrow \n }\n"
    "track_pitch_plus { \n + keyboard GreyDownArrow \n }\n"
    "track_pitch_minus { \n + keyboard GreyUpArrow \n }\n"
    "track_yaw_minus { \n + keyboard GreyLeftArrow \n }\n"
    "track_yaw_plus { \n + keyboard GreyRightArrow \n }\n"
    "track_distance_plus { \n + keyboard GreyPageDown \n }\n"
    "track_distance_minus { \n + keyboard GreyPageUp \n }\n";
static const char *DEFAULT_GAMEKEY =
    "PRESET_VIEW_1 F1\nPRESET_VIEW_2 F2\n";

static void load_one(const char *name, const char *fallback,
                     void (*parse)(LineSrc *, const char *))
{
    LineSrc src = { fs_fopen(name, "r"), NULL };
    if (src.f) {
        parse(&src, name);
        fclose(src.f);
        return;
    }
    fprintf(stdout, "[keys] %s not found — using built-in defaults\n", name);
    src.s = fallback;
    parse(&src, "(defaults)");
}

void keybind_load(void)
{
    s_nchords = 0;
    s_ngk = 0;
    s_ncmd = s_cmdpos = 0;
    memset(s_held, 0, sizeof s_held);
    for (int i = 0; i < KB_NUM_ACTIONS; i++) {
        s_plus_of[i] = s_minus_of[i] = -1;
        memset(&s_sink[i], 0, sizeof s_sink[i]);
        if (KB_ACTIONS[i].type == 0) sink_init(i);
    }
    load_one("input.map",   DEFAULT_INPUT,   parse_input_map);
    load_one("gamekey.map", DEFAULT_GAMEKEY, parse_gamekey_map);
    fprintf(stdout, "[keys] %d key chords, %d command keys\n", s_nchords, s_ngk);
}

/* ----------------------------------------------------------------------- */
/* Per frame                                                                 */
/* ----------------------------------------------------------------------- */

static bool hid(const RawInputState *in, int u) { return u > 0 && u < INPUT_HID_KEYS && in->hid_held[u]; }

static bool channel_held(const RawInputState *in, int ch)
{
    if (ch == CH_SHIFT)   return hid(in, 225) || hid(in, 229);
    if (ch == CH_CONTROL) return hid(in, 224) || hid(in, 228);
    return hid(in, KB_CHANNELS[ch].hid);
}

/* Analog sink stepper, one frame. */
static void sink_step(Sink *k, int rate, bool plus, bool minus, float dt)
{
    int a  = (int)(dt * 182.0f);                 /* ftol truncates (dt * 0x43360000) */
    int *v = k->alias ? &k->value : &k->vel;
    int act = 0;
    if (plus) {
        if (*v < 0) *v = 0;
        *v += (a * 3) << rate;
        if (*v > 0x5555) *v = 0x5555;
        k->value += *v;                           /* doubles when aliased */
        if (k->value > 0x10000) k->value = 0x10000;
        act = 1;
    }
    if (minus) {
        if (*v > 0) *v = 0;
        *v -= (a * 3) << rate;
        if (*v < -0x5555) *v = -0x5555;
        k->value += *v;
        if (k->value < -0x10000) k->value = -0x10000;
        act = 1;
    }
    if (!act) *v = 0;
}

void keybind_update(const RawInputState *in, float dt)
{
    /* Buttons: any chord satisfied (all + held, all - released). */
    memset(s_held, 0, sizeof s_held);
    for (int i = 0; i < s_nchords; i++) {
        const Chord *c = &s_chords[i];
        bool ok = true;
        for (int t = 0; t < c->nterms && ok; t++) {
            bool h = channel_held(in, c->t[t].ch);
            ok = c->t[t].exclude ? !h : h;
        }
        if (ok) s_held[c->action] = true;
    }

    /* Axes driven by their _plus/_minus buttons. */
    bool plus[KB_NUM_ACTIONS] = { 0 }, minus[KB_NUM_ACTIONS] = { 0 };
    for (int b = 0; b < KB_NUM_ACTIONS; b++) {
        if (!s_held[b]) continue;
        if (s_plus_of[b]  >= 0) plus[s_plus_of[b]]   = true;
        if (s_minus_of[b] >= 0) minus[s_minus_of[b]] = true;
    }
    for (int a = 0; a < KB_NUM_ACTIONS; a++)
        if (KB_ACTIONS[a].type == 0 && s_sink[a].bound)
            sink_step(&s_sink[a], KB_ACTIONS[a].rate, plus[a], minus[a], dt);

    /* Commands: key pressed this frame with exactly the bound modifiers. */
    s_ncmd = s_cmdpos = 0;
    bool ctrl  = hid(in, 224) || hid(in, 228);
    bool alt   = hid(in, 226) || hid(in, 230);
    bool shift = hid(in, 225) || hid(in, 229);
    for (int i = 0; i < s_ngk; i++) {
        const GameKey *g = &s_gk[i];
        if (!in->hid_pressed[g->hid]) continue;
        if (g->ctrl != ctrl || g->alt != alt || g->shift != shift) continue;
        if (s_ncmd < (int)(sizeof s_cmdq / sizeof s_cmdq[0])) s_cmdq[s_ncmd++] = g->cmd;
    }
}

int keybind_axis(int a)
{
    if (a < 0 || a >= KB_NUM_ACTIONS) return 0;
    const KbActionDef *d = &KB_ACTIONS[a];
    int v = s_sink[a].value;
    if (v >  0x10000) v =  0x10000;
    if (v < -0x10000) v = -0x10000;
    /* input_poll: out = (((v + 1.0) * range) / 2 + (min << 16)) >> shift */
    long long o = ((long long)(v + 0x10000) * (d->max - d->min)) / 2 + ((long long)d->min << 16);
    return (int)(o >> d->shift);
}

bool keybind_button(int a)
{
    return a >= 0 && a < KB_NUM_ACTIONS && s_held[a];
}

int keybind_next_command(void)
{
    return s_cmdpos < s_ncmd ? s_cmdq[s_cmdpos++] : 0;
}
