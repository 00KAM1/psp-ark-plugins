/*
 * Checkpoint - the journey log: a diary the game writes for you.
 *
 *   ms0:/CHECKPOINT/JOURNEY/<game>.TXT   one line per entry, readable on a PC:
 *        2026-11-14 19:22 | 0h 25m | Town of Redmont | ULUS10558_003
 *   ms0:/CHECKPOINT/JOURNEY/<game>.TIM   seconds played, counted by Checkpoint
 *
 * The description is kept exactly as the game wrote it (UTF-8), so Japanese
 * text is stored and compared correctly even though the 8x8 font cannot draw it.
 *
 * An entry is added when a save's description differs from the last entry
 * made for that same save folder, so ten saves in one place make one line.
 * Uses only sceIo*, so the same file is compiled and tested on a PC.
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <string.h>
#include "common.h"
#include "jtext.h"

#define JR_DIR BT_DIR "/JOURNEY"
static char g_io[2048];
static char g_line[320];

static void path_of(char *out, int size, const char *game, const char *ext)
{
    sq_snprintf(out, size, JR_DIR "/%s.%s", game, ext);
}

/* ---- small text helpers --------------------------------------------------- */
static u32 number(const char **s)
{
    u32 v = 0;
    while (**s >= '0' && **s <= '9') v = v * 10 + (u32)(*(*s)++ - '0');
    return v;
}

static void trim_copy(char *dst, int size, const char *src, int len)
{
    while (len > 0 && (*src == ' ' || *src == '\t')) { src++; len--; }
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t' || src[len - 1] == '\r')) len--;
    if (len > size - 1) len = size - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

/* Length in bytes of the UTF-8 character at s (2 to 4), or 0 if the bytes
 * there are not one whole, well-formed character. */
static int utf8_len(const u8 *s)
{
    int n = s[0] >= 0xC2 && s[0] <= 0xDF ? 2 : s[0] >= 0xE0 && s[0] <= 0xEF ? 3 : s[0] >= 0xF0 && s[0] <= 0xF4 ? 4 : 0;
    for (int i = 1; i < n; i++) if ((s[i] & 0xC0) != 0x80) return 0;
    if (n == 3 && s[0] == 0xE0 && s[1] < 0xA0) return 0;       /* overlong */
    if (n == 3 && s[0] == 0xED && s[1] > 0x9F) return 0;       /* surrogates */
    if (n == 4 && s[0] == 0xF0 && s[1] < 0x90) return 0;
    if (n == 4 && s[0] == 0xF4 && s[1] > 0x8F) return 0;
    return n;
}

/* What goes into the file as the description: the game's own text, with
 * control characters and the field separator removed, single spaces, no
 * spaces at the ends. Characters outside ASCII are kept whole (never cut in
 * half); bytes that are not valid UTF-8 become '?'. In place is allowed. */
void journey_clean(char *out, int size, const char *text)
{
    const u8 *t = (const u8 *)text;
    int o = 0, space = 1;
    while (*t) {
        int n = *t >= 0x80 ? utf8_len(t) : 1;
        if (o + (n ? n : 1) > size - 1) break;
        if (n > 1) {
            if (n == 3 && t[0] == 0xE3 && t[1] == 0x80 && t[2] == 0x80) {      /* Japanese wide space */
                if (!space) { out[o++] = ' '; space = 1; }
            } else {
                for (int i = 0; i < n; i++) out[o++] = (char)t[i];
                space = 0;
            }
            t += n;
            continue;
        }
        char c = n ? (char)*t : '?';
        t++;
        if (c == '|') c = '/';
        if ((u8)c < 0x20 || (u8)c > 0x7E) c = ' ';
        if (c == ' ') { if (space) continue; space = 1; } else space = 0;
        out[o++] = c;
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = 0;
}

/* The same text as the 8x8 font can show it: one '?' for every character it
 * has no shape for. In place is allowed. */
void journey_ascii(char *out, int size, const char *text)
{
    const u8 *t = (const u8 *)text;
    int o = 0;
    while (*t && o < size - 1) {
        int n = *t >= 0x80 ? utf8_len(t) : 1;
        out[o++] = (*t < 0x80 && *t >= 0x20 && *t != 0x7F) ? (char)*t : '?';
        t += n ? n : 1;
    }
    out[o] = 0;
}

/* "2026-11-14 19:22 | 0h 25m | text | save" -> entry. Returns 1 if it is one. */
static int parse_line(const char *line, JrEntry *e)
{
    const char *f[4]; int flen[4], n = 0;
    const char *p = line;
    if (*p == '#' || !*p) return 0;
    for (;;) {
        const char *bar = strstr(p, " | ");
        f[n] = p; flen[n] = bar ? (int)(bar - p) : (int)strlen(p);
        n++;
        if (!bar || n == 4) break;
        p = bar + 3;
    }
    if (n != 4) return 0;
    const char *d = f[0];                               /* YYYY-MM-DD HH:MM */
    if (flen[0] < 16 || d[4] != '-' || d[7] != '-' || d[10] != ' ' || d[13] != ':') return 0;
    for (int i = 0; i < 16; i++)
        if (i != 4 && i != 7 && i != 10 && i != 13 && (d[i] < '0' || d[i] > '9')) return 0;
    memset(e, 0, sizeof(*e));
    memcpy(e->stamp, d, 4); memcpy(e->stamp + 4, d + 5, 2); memcpy(e->stamp + 6, d + 8, 2);
    e->stamp[8] = '_';
    memcpy(e->stamp + 9, d + 11, 2); memcpy(e->stamp + 11, d + 14, 2);
    e->stamp[13] = '0'; e->stamp[14] = '0';
    const char *t = f[1];                               /* Nh MMm */
    while (*t == ' ') t++;
    u32 h = number(&t);
    if (*t != 'h') return 0;
    t++; while (*t == ' ') t++;
    u32 m = number(&t);
    if (*t != 'm') return 0;
    e->secs = h * 3600 + m * 60;
    char raw[160];
    trim_copy(raw, sizeof(raw), f[2], flen[2]);
    journey_clean(e->text, sizeof(e->text), raw);      /* the same form journey_note compares with */
    trim_copy(e->save, sizeof(e->save), f[3], flen[3]);
    return e->text[0] != 0;
}

/* Calls fn for every entry in the file, oldest first. Returns how many. */
static int for_each(const char *game, void (*fn)(const JrEntry *, void *), void *arg)
{
    char p[96];
    static JrEntry e;
    path_of(p, sizeof(p), game, "TXT");
    SceUID fd = sceIoOpen(p, PSP_O_RDONLY, 0);
    if (fd < 0) return 0;
    int count = 0, ll = 0, skip = 0, n;
    while ((n = sceIoRead(fd, g_io, sizeof(g_io))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = g_io[i];
            if (c == '\n') {
                g_line[ll] = 0;
                if (!skip && parse_line(g_line, &e)) { fn(&e, arg); count++; }
                ll = 0; skip = 0;
            } else if (ll < (int)sizeof(g_line) - 1) g_line[ll++] = c;
            else skip = 1;                              /* over-long line: not one of ours */
        }
    }
    sceIoClose(fd);
    g_line[ll] = 0;
    if (ll && !skip && parse_line(g_line, &e)) { fn(&e, arg); count++; }
    return count;
}

/* ---- which text makes an entry ---------------------------------------------- */
/* Games describe a save in two texts, a title and a longer detail. Some put
 * the place in the title ("Town of Redmont"); others call every save "save
 * data" and put the place in the detail, next to a level and a play time
 * that change on every save. So: numbers are ignored when comparing, the
 * title is followed while it changes, and the detail is followed when the
 * title stays the same. What each save folder last showed is remembered in
 *   ms0:/CHECKPOINT/JOURNEY/<game>.KEY     save <tab> title <tab> detail
 * with the numbers already taken out. */
void journey_key(char *out, int size, const char *text)
{
    const char *p = text;
    int o = 0, space = 1;
    for (;;) {
        const char *at = p;
        unsigned cp = jt_next(&p);
        if (!cp) break;
        if ((cp >= '0' && cp <= '9') || (cp >= 0xFF10 && cp <= 0xFF19)) continue;     /* digits, also full-width */
        if (cp == ':' || cp == '.' || cp == '/' || cp == 0xFF1A || cp == '\t') continue;  /* what times and dates are joined with */
        if (cp == ' ') { if (space) continue; space = 1; } else space = 0;
        int len = (int)(p - at);
        if (o + len > size - 1) break;
        memcpy(out + o, at, len); o += len;
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = 0;
}

static char g_state[4096];

/* Finds the save's line in the state text. Returns its start, or NULL. */
static char *state_line(char *state, const char *save, char **end)
{
    int sl = (int)strlen(save);
    for (char *l = state; *l; ) {
        char *e = strchr(l, '\n');
        char *next = e ? e + 1 : l + strlen(l);
        if (!strncmp(l, save, sl) && l[sl] == '\t') { *end = next; return l; }
        l = next;
    }
    return 0;
}

/* Decides whether this save is news. Returns 1 and the text to log in out,
 * or 0. detail_only = 1 ignores the title altogether. */
int journey_pick(const char *game, const char *save, const char *title, const char *detail, int detail_only, char *out, int size)
{
    char p[96], pn[96];
    static char t[JR_TEXT_MAX], d[JR_TEXT_MAX], tk[JR_TEXT_MAX], dk[JR_TEXT_MAX], line[JR_TEXT_MAX * 2 + BT_NAME_MAX + 8];
    out[0] = 0;
    if (!vault_name_ok(game) || !vault_name_ok(save)) return 0;
    journey_clean(t, sizeof(t), detail_only ? "" : title);
    journey_clean(d, sizeof(d), detail);
    journey_key(tk, sizeof(tk), t);
    journey_key(dk, sizeof(dk), d);
    if (!t[0] && !d[0]) return 0;

    path_of(p, sizeof(p), game, "KEY");
    path_of(pn, sizeof(pn), game, "KNW");
    int n = 0;
    SceUID fd = sceIoOpen(p, PSP_O_RDONLY, 0);
    if (fd < 0) fd = sceIoOpen(pn, PSP_O_RDONLY, 0);         /* a write was cut short after the old file went */
    if (fd >= 0) { n = sceIoRead(fd, g_state, sizeof(g_state) - 1); sceIoClose(fd); }
    if (n < 0) n = 0;
    g_state[n] = 0;

    const char *text = 0;
    char *end, *l = state_line(g_state, save, &end);
    if (!l) {
        text = t[0] ? t : d;                                /* first time: the title, as the game's own load screen leads with it */
    } else {
        char *otk = l + strlen(save) + 1, *tab = otk, *odk;
        while (tab < end && *tab != '\t' && *tab != '\n') tab++;
        odk = tab < end && *tab == '\t' ? tab + 1 : tab;
        int tkl = (int)(tab - otk), dkl = 0;
        while (odk + dkl < end && odk[dkl] != '\n' && odk[dkl] != '\r') dkl++;
        int same_t = (int)strlen(tk) == tkl && !strncmp(tk, otk, tkl);
        int same_d = (int)strlen(dk) == dkl && !strncmp(dk, odk, dkl);
        if (!same_t && t[0]) text = t;
        else if (!same_d && d[0]) text = d;
        if (same_t && same_d) return 0;                     /* nothing to remember either */
        memmove(l, end, strlen(end) + 1);                   /* our old line goes; the new one is added below */
    }

    int ll = sq_snprintf(line, sizeof(line), "%s\t%s\t%s\n", save, tk, dk);
    int have = (int)strlen(g_state);
    while (have + ll > (int)sizeof(g_state) - 1 && have > 0) {      /* full: forget the oldest save folder */
        char *e = strchr(g_state, '\n');
        if (!e) { g_state[0] = 0; break; }
        memmove(g_state, e + 1, strlen(e + 1) + 1);
        have = (int)strlen(g_state);
    }
    memcpy(g_state + have, line, ll + 1);
    have += ll;
    sceIoMkdir(JR_DIR, 0777);
    fd = sceIoOpen(pn, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        int w = sceIoWrite(fd, g_state, have);
        sceIoClose(fd);
        if (w == have) { sceIoRemove(p); sceIoRename(pn, p); } else sceIoRemove(pn);
    }
    if (!text) return 0;
    jt_fit(out, size, text, JR_TEXT_MAX);
    return 1;
}

/* ---- time played ---------------------------------------------------------- */
static int read_number(const char *path, u32 *out)
{
    char b[16];
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return -1;
    int n = sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    if (n <= 0 || b[0] < '0' || b[0] > '9') return -1;
    b[n] = 0;
    const char *s = b;
    *out = number(&s);
    return 0;
}

/* Adds seconds to the game's total and returns the new total. add = 0 only reads. */
u32 journey_time_add(const char *game, u32 add)
{
    char p[96], pn[96], b[16];
    u32 total = 0;
    if (!vault_name_ok(game)) return 0;
    path_of(p, sizeof(p), game, "TIM");
    path_of(pn, sizeof(pn), game, "NEW");
    if (read_number(p, &total) < 0) read_number(pn, &total);    /* a write was cut short after the old file went */
    if (!add) return total;
    total += add;
    sceIoMkdir(JR_DIR, 0777);
    int len = sq_snprintf(b, sizeof(b), "%d\n", (int)total);
    SceUID fd = sceIoOpen(pn, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return total;
    int w = sceIoWrite(fd, b, len);
    sceIoClose(fd);
    if (w != len) { sceIoRemove(pn); return total; }
    sceIoRemove(p);
    sceIoRename(pn, p);
    return total;
}

/* ---- writing an entry ----------------------------------------------------- */
typedef struct { const char *save; char last[JR_TEXT_MAX]; int have; } LastOf;
static void last_cb(const JrEntry *e, void *arg)
{
    LastOf *l = arg;
    if (!strcmp(e->save, l->save)) { strcpy(l->last, e->text); l->have = 1; }
}

/* Returns 1 if a line was added, 0 if the description has not changed (or is
 * empty), below 0 if the file could not be written. stamp is YYYYMMDD_HHMMSS. */
int journey_note(const char *game, const char *save, const char *stamp, const char *text, u32 secs)
{
    char p[96], clean[JR_TEXT_MAX];
    static LastOf l;
    if (!vault_name_ok(game) || !vault_name_ok(save) || strlen(stamp) != BT_STAMP_LEN) return -1;
    journey_clean(clean, sizeof(clean), text);
    if (!clean[0]) return 0;
    l.save = save; l.last[0] = 0; l.have = 0;
    for_each(game, last_cb, &l);
    if (l.have && !strcmp(l.last, clean)) return 0;

    sceIoMkdir(JR_DIR, 0777);
    path_of(p, sizeof(p), game, "TXT");
    SceIoStat st;
    int fresh = sceIoGetstat(p, &st) < 0;
    SceUID fd = sceIoOpen(p, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd < 0) return -2;
    int len = 0;
    if (fresh)
        len = sq_snprintf(g_line, sizeof(g_line), "# Journey log of %s, kept by Checkpoint: date | time played | description | save\r\n", game);
    len += sq_snprintf(g_line + len, sizeof(g_line) - len, "%.4s-%.2s-%.2s %.2s:%.2s | %dh %02dm | %s | %s\r\n",
                       stamp, stamp + 4, stamp + 6, stamp + 9, stamp + 11,
                       (int)(secs / 3600), (int)(secs / 60 % 60), clean, save);
    int w = sceIoWrite(fd, g_line, len);
    sceIoClose(fd);
    return w == len ? 1 : -3;
}

/* ---- reading for the diary ------------------------------------------------ */
typedef struct { JrEntry *out; int max, seen; char first[BT_STAMP_LEN + 1]; } Loader;
static void load_cb(const JrEntry *e, void *arg)
{
    Loader *l = arg;
    if (!l->seen) strcpy(l->first, e->stamp);
    if (l->seen >= l->max) memmove(l->out, l->out + 1, (l->max - 1) * sizeof(JrEntry));   /* keep the newest */
    l->out[l->seen < l->max ? l->seen : l->max - 1] = *e;
    l->seen++;
}

/* Fills out[] with the newest `max` entries, oldest of those first. *total is
 * the number in the file; first gets the stamp of the very first entry. */
int journey_load(const char *game, JrEntry *out, int max, int *total, char *first)
{
    static Loader l;
    *total = 0; first[0] = 0;
    if (!vault_name_ok(game) || max < 1) return 0;
    l.out = out; l.max = max; l.seen = 0; l.first[0] = 0;
    for_each(game, load_cb, &l);
    *total = l.seen;
    strcpy(first, l.first);
    return l.seen < max ? l.seen : max;
}

/* ---- laying it out -------------------------------------------------------- */
/* Breaks text into at most two lines of `cols` characters: at a space where
 * there is one, otherwise between characters (Japanese has no spaces). The
 * text is UTF-8 and is only ever cut between whole characters. */
int journey_wrap(const char *text, int cols, char lines[2][JR_LINE_MAX])
{
    if (cols > JR_COLS_MAX) cols = JR_COLS_MAX;
    lines[0][0] = lines[1][0] = 0;
    if (jt_cols(text) <= cols) { jt_fit(lines[0], JR_LINE_MAX, text, cols); return 1; }
    const char *p = text, *brk = 0;
    for (int n = 0; n <= cols; n++) {                   /* a space at position cols also ends the line */
        const char *at = p;
        unsigned cp = jt_next(&p);
        if (!cp) break;
        if (cp == ' ' && n > 0) brk = at;
    }
    if (brk) {
        int len = (int)(brk - text);
        memcpy(lines[0], text, len); lines[0][len] = 0;
        text = brk;
    } else {
        jt_fit(lines[0], JR_LINE_MAX, text, cols);
        text += strlen(lines[0]);
    }
    while (*text == ' ') text++;
    jt_fit(lines[1], JR_LINE_MAX, text, cols);
    return lines[1][0] ? 2 : 1;
}

/* An entry takes a date line, one or two text lines, a time line and a gap. */
static int rows_of(const JrEntry *e, int cols)
{
    char l[2][JR_LINE_MAX];
    return 2 + journey_wrap(e->text, cols, l);
}

/* first[k] = index of the first entry on entry page k. Returns the number of pages. */
int journey_paginate(const JrEntry *e, int n, int cols, int rows, u16 *first, int maxpages)
{
    int pages = 0, used = rows + 1;
    for (int i = 0; i < n; i++) {
        int need = rows_of(&e[i], cols);
        if (used + need > rows) {
            if (pages == maxpages) return pages;
            first[pages++] = (u16)i;
            used = 0;
        }
        used += need + 1;
    }
    return pages;
}

/* 0 = Sunday. */
int journey_weekday(int y, int m, int d)
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 1 || m > 12) return 0;
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
