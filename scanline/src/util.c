/*
 * Scanline - log, config file, and a small vsnprintf (the kernel C library
 * has snprintf but no va_list form). The formatter is Sidequest's.
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdarg.h>
#include <string.h>
#include <systemctrl.h>
#include "common.h"

static void put(char *buf, int size, int *pos, char c)
{
    if (*pos < size - 1) buf[*pos] = c;
    (*pos)++;
}

static int sl_vsnprintf(char *buf, int size, const char *fmt, va_list ap)
{
    int pos = 0;
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(buf, size, &pos, *fmt); continue; }
        fmt++;
        int zero = 0, width = 0;
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        char tmp[16]; int n = 0;
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)strlen(s);
            for (int i = len; i < width; i++) put(buf, size, &pos, ' ');
            for (int i = 0; i < len; i++) put(buf, size, &pos, s[i]);
            continue;
        }
        case 'd': case 'u': {
            int v = va_arg(ap, int);
            u32 u = (u32)v; int neg = 0;
            if (*fmt == 'd' && v < 0) { neg = 1; u = (u32)(-v); }
            do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
            if (neg) tmp[n++] = '-';
            break;
        }
        case 'x': case 'X': {
            u32 u = va_arg(ap, u32);
            const char *dig = (*fmt == 'x') ? "0123456789abcdef" : "0123456789ABCDEF";
            do { tmp[n++] = dig[u & 15]; u >>= 4; } while (u);
            break;
        }
        case '%': put(buf, size, &pos, '%'); continue;
        default:  put(buf, size, &pos, *fmt); continue;
        }
        for (int i = n; i < width; i++) put(buf, size, &pos, zero ? '0' : ' ');
        while (n) put(buf, size, &pos, tmp[--n]);
    }
    if (size > 0) buf[pos < size ? pos : size - 1] = 0;
    return pos;
}

int sl_snprintf(char *buf, int size, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = sl_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

/* ------------------------------------------------------------------------ */
/* The Memory Stick and Sidequest.                                          */
/* A state holds the system's whole memory, the file system's included. A   */
/* write that is half done when the state is taken would be finished, after */
/* a load, on a stick that has changed since. So the two sides shake hands: */
/*   - we say when we are writing (scanlineCardWriting, which Sidequest asks  */
/*     before it sleeps: it waits for us, or takes no state);               */
/*   - Sidequest says when it is at the stick (sidequestCardBusy: 1 while   */
/*     it takes or loads a state, 2 after a load that could not refresh the */
/*     stick), and then we do not start a write.                            */
/* We raise our mark first and ask second; Sidequest raises its own first   */
/* and asks second, so one of the two always sees the other.                */
/* Without Sidequest, or with one older than this, everything is as before. */
/* ------------------------------------------------------------------------ */
static volatile int g_card_writing;
static volatile int g_card_held;                /* writes put off, for one line in the log afterwards */

int scanlineCardWriting(void) { return g_card_writing; }

static void card_mark(int d, int held)
{
    int k = sceKernelCpuSuspendIntr();
    g_card_writing += d;
    g_card_held += held;
    sceKernelCpuResumeIntr(k);
}

/* Looked up every time: Sidequest may load after us, or not at all. */
int sl_card_busy(void)
{
    int (*busy)(void) = (void *)sctrlHENFindFunction("Sidequest", "SidequestLib", 0x5C4B5001);
    return busy ? busy() : 0;
}

static int card_begin(int count)
{
    card_mark(1, 0);
    int b = sl_card_busy();
    if (!b) return 0;
    card_mark(-1, count);
    return b == 2 ? -2 : -1;
}

/* Before any write: 0 = go ahead, and call sl_card_end() when done. Below 0 =
 * Sidequest is at the stick: do not write now (-2: not again in this program). */
int  sl_card_begin(void) { return card_begin(1); }
void sl_card_end(void)   { card_mark(-1, 0); }

/* For a write that must not be lost: waits while a state is taken or loaded. */
int sl_card_wait(void)
{
    for (int i = 0; i < 150; i++) {             /* 15 s of time awake; a save or a load is over long before */
        int r = card_begin(i == 0);
        if (r == 0 || r == -2) return r;
        sceKernelDelayThread(100 * 1000);
    }
    return -1;
}

/* Appends one line to ms0:/SCANLINE/scanline.log. Never called from the hook. */
void sl_log(const char *fmt, ...)
{
    char line[296];
    if (card_begin(0) < 0) return;              /* a log line is not worth a write at the wrong moment */
    va_list ap; va_start(ap, fmt);
    int n = sl_vsnprintf(line, 198, fmt, ap);
    va_end(ap);
    if (n > 197) n = 197;
    line[n++] = '\r'; line[n++] = '\n'; line[n] = 0;
    if (g_card_held) {                          /* said once, when the stick is ours again */
        int h = g_card_held;
        card_mark(0, -h);
        n += sl_snprintf(line + n, (int)sizeof(line) - n, "card guard: %d write(s) put off while Sidequest was at the stick\r\n", h);
    }
    SceUID fd = sceIoOpen(SL_LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) { sceIoWrite(fd, line, n); sceIoClose(fd); }
    sl_card_end();
}

/* ---- config: "key = value" lines, '#' comments ---------------------------- */
void config_defaults(SlConfig *c)
{
    c->enabled = 1;
    c->strength = 25;
    c->spacing = 2;
    c->mode = 1;
    c->hotkeys = 1;
    c->fast = 1;
    c->ge_wait = 1;
    c->defer = 1;
}

static int parse_int(const char *s, int *out)
{
    int v = 0, neg = 0, any = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); any = 1; }
    if (any && *s == '.') { s++; while (*s >= '0' && *s <= '9') s++; }     /* "12.5": the whole part is enough */
    while (*s == ' ' || *s == '\t' || *s == '%') s++;
    if (!any || *s) return 0;
    *out = neg ? -v : v;
    return 1;
}

/* A value that can't be read, or is out of range, is ignored and the
 * default stays: one bad line never throws away the rest of the file. */
static void apply(SlConfig *c, const char *key, const char *val)
{
    int v;
    if (!parse_int(val, &v)) return;
    if (!strcmp(key, "enabled"))        c->enabled = v != 0;
    else if (!strcmp(key, "strength"))  { if (v >= 0 && v <= 100) c->strength = v; }
    else if (!strcmp(key, "spacing"))   { if (v >= 2 && v <= 4) c->spacing = v; }
    else if (!strcmp(key, "mode"))      { if (v == 1 || v == 2) c->mode = v; }
    else if (!strcmp(key, "hotkeys"))   c->hotkeys = v != 0;
    else if (!strcmp(key, "fast"))      c->fast = v != 0;
    else if (!strcmp(key, "ge_wait"))   c->ge_wait = v != 0;
    else if (!strcmp(key, "defer"))     c->defer = v != 0;
}

int config_load(SlConfig *c)
{
    static char buf[2048];
    config_defaults(c);
    SceUID fd = sceIoOpen(SL_CONFIG_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) return 0;
    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) return 0;
    buf[n] = 0;

    char *line = buf;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *cut = strchr(line, '\r'); if (cut) *cut = 0;
        cut = strchr(line, '#'); if (cut) *cut = 0;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            char *k = line, *e;
            while (*k == ' ' || *k == '\t') k++;
            e = k + strlen(k);
            while (e > k && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
            char *v = eq + 1;
            e = v + strlen(v);
            while (e > v && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
            apply(c, k, v);
        }
        line = next;
    }
    return 1;
}

/* Written whole, comments included, so the file stays readable after the
 * buttons have changed it. Goes through a temporary file. */
int config_save(const SlConfig *c)
{
    static char buf[2400];
    int n = sl_snprintf(buf, sizeof(buf),
        "# Scanline settings. The first four are what a game starts with the first\r\n"
        "# time. Once you change something with the buttons in a game (hold L+R with\r\n"
        "# the button named for half a second), that game's own settings are kept in\r\n"
        "# games.txt and used from then on. This file is not changed by the buttons.\r\n"
        "\r\n"
        "# 1 = scanlines on, 0 = off.   (L + R + TRIANGLE)\r\n"
        "enabled = %d\r\n"
        "\r\n"
        "# How dark each scanline is, in percent. The steps are 12.5, 25, 37.5, 50,\r\n"
        "# 62.5, 75 and 100 (black lines); written here as 13, 25, 38, 50, 63, 75,\r\n"
        "# 100. Other numbers go to the nearest step.   (L + R + UP / DOWN)\r\n"
        "strength = %d\r\n"
        "\r\n"
        "# One dark line every 2, 3 or 4 rows. 2 is the classic look; 3 and 4 are\r\n"
        "# thinner lines that keep more of the brightness.   (L + R + CIRCLE)\r\n"
        "spacing = %d\r\n"
        "\r\n"
        "# 1 = the game's picture change goes through first and the lines are drawn\r\n"
        "# straight after. 2 = the lines are drawn first: for a game whose top rows\r\n"
        "# flicker or lose their lines in mode 1. Mode 2 can tear the picture or pop\r\n"
        "# the sound in some games, so it is chosen game by game.   (L + R + SQUARE)\r\n"
        "mode = %d\r\n"
        "\r\n"
        "# 0 = the L+R button combinations do nothing.\r\n"
        "hotkeys = %d\r\n"
        "\r\n"
        "# 1 = the fast method (CPU cache). 0 = the slow method, only for comparing\r\n"
        "# the times in scanline.log.\r\n"
        "fast = %d\r\n"
        "\r\n"
        "# 1 = if the graphics chip is still drawing a frame when the game hands it\r\n"
        "# over, wait for it (12 ms at most) before adding the scanlines. Without\r\n"
        "# this the lines can flicker in such games. 0 = never wait.\r\n"
        "ge_wait = %d\r\n"
        "\r\n"
        "# Some games change the picture from inside an interrupt. 1 = the lines for\r\n"
        "# those frames are drawn by the plugin's own thread right after it, so the\r\n"
        "# game's sound is not held up. 0 = drawn inside the interrupt (sound pops).\r\n"
        "defer = %d\r\n",
        c->enabled, c->strength, c->spacing, c->mode, c->hotkeys, c->fast, c->ge_wait, c->defer);
    if (n >= (int)sizeof(buf)) return -1;

    if (sl_card_wait() < 0) return -5;          /* Sidequest is at the stick */
    sceIoMkdir(SL_DIR, 0777);
    SceUID fd = sceIoOpen(SL_DIR "/config.tmp", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) { sl_card_end(); return -2; }
    int ok = sceIoWrite(fd, buf, n) == n;
    if (sceIoClose(fd) < 0) ok = 0;
    if (!ok) { sceIoRemove(SL_DIR "/config.tmp"); sl_card_end(); return -3; }
    sceIoRemove(SL_CONFIG_PATH);
    int rr = sceIoRename(SL_DIR "/config.tmp", SL_CONFIG_PATH) < 0 ? -4 : 0;
    sl_card_end();
    return rr;
}

/* ---- per-game settings: ms0:/SCANLINE/games.txt --------------------------------
 * One line a game:  NAME = on strength spacing mode   (e.g. "YS_OATH_3F2A = 1 25 2 1").
 * The fourth number is 2 for mode 2; anything else, or none, is mode 1. (Test
 * builds wrote a 0 or 1 there for something since removed: both mean mode 1.)
 * A game with no line starts with what config.txt says. */

/* A name for a game, made from the file it runs from the way Sidequest names
 * its folders: the ISO's file name, or for an EBOOT.PBP the folder it sits in;
 * letters, digits and '_' only, plus four hex digits of a checksum of the
 * whole path, so two games with similar names never share a line. */
void game_key(char *out, const char *path)
{
    if (!path || !path[0]) path = "UNKNOWN";
    const char *end = path + strlen(path), *name = end, *stop = end;
    while (name > path && name[-1] != '/' && name[-1] != ':') name--;
    if (!strcmp(name, "EBOOT.PBP") && name > path) {            /* use the folder's name */
        stop = name - 1;
        name = stop;
        while (name > path && name[-1] != '/' && name[-1] != ':') name--;
    } else {
        const char *dot = stop;
        while (dot > name && *dot != '.') dot--;
        if (dot > name) stop = dot;
    }
    int n = 0;
    for (const char *c = name; c < stop && n < SL_KEY_MAX - 6; c++) {
        char ch = *c;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        out[n++] = ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) ? ch : '_';
    }
    if (n == 0) { memcpy(out, "GAME", 4); n = 4; }
    u32 h = 2166136261u;                                        /* FNV-1a over the whole path */
    for (const char *c = path; *c; c++) h = (h ^ (u8)*c) * 16777619u;
    sl_snprintf(out + n, SL_KEY_MAX - n, "_%04X", (h ^ (h >> 16)) & 0xFFFF);
}

#define GAMES_MAX 8192                                          /* about 200 games */
/* Two work buffers, 16 KB together, taken for the length of one read or write of
 * games.txt and given back: kernel memory is short with several plugins loaded. */
static char *g_games_in, *g_games_out;
static SceUID g_games_id = -1;

static int games_take(void)
{
    g_games_id = sceKernelAllocPartitionMemory(1, "SlGames", 1 /* high */, 2 * GAMES_MAX, NULL);
    if (g_games_id < 0) return -1;
    g_games_in = sceKernelGetBlockHeadAddr(g_games_id);
    g_games_out = g_games_in + GAMES_MAX;
    return 0;
}

static void games_give(void)
{
    if (g_games_id >= 0) sceKernelFreePartitionMemory(g_games_id);
    g_games_id = -1;
    g_games_in = g_games_out = 0;
}

static int games_read(void)
{
    SceUID fd = sceIoOpen(SL_GAMES_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) { g_games_in[0] = 0; return 0; }
    int n = sceIoRead(fd, g_games_in, GAMES_MAX - 1);
    sceIoClose(fd);
    if (n < 0) n = 0;
    g_games_in[n] = 0;
    return n;
}

/* Does this line belong to `key`? Returns where its values start, or NULL. */
static const char *line_is(const char *line, const char *key)
{
    int k = (int)strlen(key);
    if (strncmp(line, key, (size_t)k)) return 0;
    line += k;
    while (*line == ' ' || *line == '\t') line++;
    return *line == '=' ? line + 1 : 0;
}

static int games_get_in(const char *key, SlConfig *c)
{
    games_read();
    for (const char *line = g_games_in; *line; ) {
        const char *v = line_is(line, key);
        if (v) {
            int val[4], got = 0;
            while (got < 4) {
                while (*v == ' ' || *v == '\t') v++;
                if (*v < '0' || *v > '9') break;
                int x = 0;
                while (*v >= '0' && *v <= '9') x = x * 10 + (*v++ - '0');
                val[got++] = x;
            }
            /* A line that can't be read in full is ignored, like a bad config line. */
            if (got >= 3 && val[1] <= 100 && val[2] >= 2 && val[2] <= 4) {
                c->enabled = val[0] != 0; c->strength = val[1]; c->spacing = val[2];
                c->mode = (got == 4 && val[3] == 2) ? 2 : 1;
                return 1;
            }
            return 0;
        }
        while (*line && *line != '\n') line++;
        if (*line) line++;
    }
    return 0;
}

int games_get(const char *key, SlConfig *c)
{
    if (games_take() < 0) return 0;             /* no memory: as if the game had no line */
    int r = games_get_in(key, c);
    games_give();
    return r;
}

static int games_set_in(const char *key, const SlConfig *c)
{
    char mine[80];
    int mlen = sl_snprintf(mine, sizeof(mine), "%s = %d %d %d %d\r\n", key, c->enabled, c->strength, c->spacing, c->mode == 2 ? 2 : 1);
    static const char head[] =
        "# Scanline: settings remembered for each game. Written by the plugin when\r\n"
        "# you change something with the buttons in that game.\r\n"
        "# NAME = on(1)/off(0)  strength  spacing  mode\r\n";
    int n = sl_snprintf(g_games_out, GAMES_MAX, "%s", head);
    games_read();

    /* Every other game's line is kept as it is. If the file is full, the
     * oldest lines (the top ones) are the ones left out. */
    int total = 0;
    for (int pass = 0; pass < 2; pass++) {
        int skip = total - (GAMES_MAX - 1 - n - mlen), seen = 0;
        for (const char *line = g_games_in; *line; ) {
            const char *e = line;
            while (*e && *e != '\n') e++;
            int len = (int)(e - line) + (*e == '\n');
            int keep = line[0] != '#' && line[0] != '\r' && line[0] != '\n' && !line_is(line, key);
            if (keep) {
                if (pass == 0) total += len;
                else if (seen >= skip) { memcpy(g_games_out + n, line, (size_t)len); n += len; }
                seen += len;
            }
            line += len;
        }
    }
    if (n && g_games_out[n - 1] != '\n') { g_games_out[n++] = '\r'; g_games_out[n++] = '\n'; }
    memcpy(g_games_out + n, mine, (size_t)mlen); n += mlen;

    if (sl_card_wait() < 0) return -5;          /* Sidequest is at the stick */
    sceIoMkdir(SL_DIR, 0777);
    SceUID fd = sceIoOpen(SL_DIR "/games.tmp", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) { sl_card_end(); return -2; }
    int ok = sceIoWrite(fd, g_games_out, n) == n;
    if (sceIoClose(fd) < 0) ok = 0;
    if (!ok) { sceIoRemove(SL_DIR "/games.tmp"); sl_card_end(); return -3; }
    sceIoRemove(SL_GAMES_PATH);
    int rr = sceIoRename(SL_DIR "/games.tmp", SL_GAMES_PATH) < 0 ? -4 : 0;
    sl_card_end();
    return rr;
}

int games_set(const char *key, const SlConfig *c)
{
    if (games_take() < 0) return -6;            /* no memory for the work buffers */
    int r = games_set_in(key, c);
    games_give();
    return r;
}
