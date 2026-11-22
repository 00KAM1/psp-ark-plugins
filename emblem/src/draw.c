/*
 * Emblem - drawing the pop-up card and the viewer.
 * No PSP calls: the same file is built into the plugin and into the PC
 * render tool, so the pictures made on the PC are what the PSP draws.
 *
 * Layout (approved by the user from the mock, 2026-11-22):
 *   card 288 px wide, 8 px from the bottom-right corner of the screen;
 *   2 px bar along the top; 32x32 cup at (6, 6); text from x = 46:
 *   "ACHIEVEMENT UNLOCKED" in green at y 7, the title in white below it
 *   (up to two lines of 29 characters, 11 px apart), then points and
 *   progress in grey.
 */
#include "draw.h"

static const u32 g_cup[32] = {
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x007FFE00u, 0x007FFE00u, 0x07FFFFE0u,
    0x04FFFF20u, 0x04FFFF20u, 0x04FFFF20u, 0x04FFFF20u, 0x07FFFFE0u, 0x007FFE00u, 0x007FFE00u, 0x007FFE00u,
    0x007FFE00u, 0x007FFE00u, 0x003FFC00u, 0x001FF800u, 0x000FF000u, 0x0007E000u, 0x0003C000u, 0x0003C000u,
    0x0003C000u, 0x003FFC00u, 0x003FFC00u, 0x003FFC00u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
};

u32 dr_conv(int fmt, u32 c)                     /* c is 0x00BBGGRR */
{
    u32 r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    switch (fmt) {
    case DR_565:  return (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11);
    case DR_5551: return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | 0x8000;
    case DR_4444: return (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | 0xF000;
    default:      return c | 0xFF000000u;
    }
}

static inline void pset(const DrSurface *s, int x, int y, u32 pc)
{
    if (x < 0 || y < 0 || x >= DR_W || y >= DR_H) return;
    if (s->fmt == DR_8888) ((u32 *)s->base)[y * s->stride + x] = pc;
    else ((u16 *)s->base)[y * s->stride + x] = (u16)pc;
}

void dr_rect(const DrSurface *s, int x, int y, int w, int h, u32 color)
{
    u32 pc = dr_conv(s->fmt, color);
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > DR_W ? DR_W : x + w, y1 = y + h > DR_H ? DR_H : y + h;
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) pset(s, i, j, pc);
}

/* Upper-case 8x8 text, nothing drawn behind it. */
void dr_text(const DrSurface *s, int x, int y, const char *t, u32 fg, const u8 *font)
{
    u32 pc = dr_conv(s->fmt, fg);
    if (y <= -8 || y >= DR_H) return;
    for (; *t; t++, x += 8) {
        u8 ch = (u8)*t;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        const u8 *g = &font[ch * 8];
        for (int r = 0; r < 8; r++) {
            u8 bits = g[r];
            if (!bits) continue;
            for (int c = 0; c < 8; c++)
                if (bits & (0x80 >> c)) pset(s, x + c, y + r, pc);
        }
    }
}

static void icon(const DrSurface *s, int x, int y)
{
    u32 bg = dr_conv(s->fmt, DR_C_ICONBG), fg = dr_conv(s->fmt, DR_C_GOLD);
    for (int r = 0; r < 32; r++)
        for (int c = 0; c < 32; c++) pset(s, x + c, y + r, (g_cup[r] & (0x80000000u >> c)) ? fg : bg);
}

/* Breaks a title into at most two lines of `cols` characters, at spaces
 * where it can; a word longer than a line is cut. Returns the line count. */
int dr_wrap(const char *t, int cols, char lines[2][DR_LINE_MAX])
{
    int n = 0;
    if (cols > DR_LINE_MAX - 1) cols = DR_LINE_MAX - 1;
    lines[0][0] = lines[1][0] = 0;
    while (*t == ' ') t++;
    while (*t && n < 2) {
        int len = 0, brk = -1;
        while (t[len] && len < cols) { if (t[len] == ' ') brk = len; len++; }
        if (t[len] && t[len] != ' ' && brk > 0 && n == 0) len = brk;   /* the second line is cut, not wrapped again */
        for (int i = 0; i < len; i++) lines[n][i] = t[i];
        while (len > 0 && lines[n][len - 1] == ' ') len--;
        lines[n][len] = 0;
        t += len;
        while (*t == ' ') t++;
        n++;
    }
    return n ? n : 1;
}

int dr_card_height(const char *title)
{
    char l[2][DR_LINE_MAX];
    return 32 + 11 * dr_wrap(title, DR_CARD_COLS, l) + 3;
}

/* The card with its top-left corner at (x, y); rows off the screen are skipped. */
void dr_card(const DrSurface *s, int x, int y, const char *head, const char *title, const char *foot, const u8 *font)
{
    char l[2][DR_LINE_MAX];
    int n = dr_wrap(title, DR_CARD_COLS, l);
    int h = 32 + 11 * n + 3;
    dr_rect(s, x, y, DR_CARD_W, h, DR_C_BG);
    dr_rect(s, x, y, DR_CARD_W, 2, DR_C_HI);
    icon(s, x + 6, y + 6);
    dr_text(s, x + 46, y + 7, head, DR_C_OK, font);
    for (int i = 0; i < n; i++) dr_text(s, x + 46, y + 19 + 11 * i, l[i], DR_C_WHITE, font);
    dr_text(s, x + 46, y + 21 + 11 * n, foot, DR_C_GREY, font);
}

/* Where the card is `t_ms` after it was first shown: it slides up from the
 * bottom edge, stays, and slides back down. Returns the top row, or a value
 * of DR_H or more when it is entirely below the screen. */
int dr_card_y(int h, u32 t_ms)
{
    int rest = DR_H - 8 - h, hidden = DR_H;
    u32 in = DR_SLIDE_MS, out = DR_SLIDE_MS + DR_HOLD_MS, end = out + DR_SLIDE_MS;
    if (t_ms >= end) return hidden;
    if (t_ms < in)  return hidden - (int)((u32)(hidden - rest) * t_ms / in);
    if (t_ms < out) return rest;
    return rest + (int)((u32)(hidden - rest) * (t_ms - out) / DR_SLIDE_MS);
}

/* ---- the viewer ------------------------------------------------------------ */
/* Layout approved by the user from the mock (emblem_viewer_v2.png, 2026-11-22). */
static const u16 g_cup16[16] = {
    0x0000, 0x0000, 0x0FF0, 0x3FFC, 0x3FFC, 0x3FFC, 0x3FFC, 0x0FF0,
    0x0FF0, 0x07E0, 0x03C0, 0x0180, 0x0180, 0x07E0, 0x07E0, 0x0000,
};
static const u8 g_tri[8] = { 0x10, 0x28, 0x28, 0x44, 0x44, 0xFE, 0x00, 0x00 };
static const u8 g_circ[8] = { 0x38, 0x44, 0x82, 0x82, 0x82, 0x44, 0x38, 0x00 };

static int slen(const char *t) { int n = 0; while (t && t[n]) n++; return n; }

static void cup16(const DrSurface *s, int x, int y, u32 color)
{
    u32 pc = dr_conv(s->fmt, color);
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 16; c++) if (g_cup16[r] & (0x8000 >> c)) pset(s, x + c, y + r, pc);
}

static void glyph8(const DrSurface *s, int x, int y, const u8 *g, u32 color)
{
    u32 pc = dr_conv(s->fmt, color);
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) if (g[r] & (0x80 >> c)) pset(s, x + c, y + r, pc);
}

/* Text cut to n characters. */
static void text_n(const DrSurface *s, int x, int y, const char *t, int n, u32 fg, const u8 *font)
{
    char b[64];
    int i = 0;
    if (n > 63) n = 63;
    for (; t[i] && i < n; i++) b[i] = t[i];
    b[i] = 0;
    dr_text(s, x, y, b, fg, font);
}
static void text_right(const DrSurface *s, int right, int y, const char *t, u32 fg, const u8 *font)
{
    dr_text(s, right - 8 * slen(t), y, t, fg, font);
}

void dr_dim(const DrSurface *s, int x, int y, int w, int h)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > DR_W ? DR_W : x + w, y1 = y + h > DR_H ? DR_H : y + h;
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) {
            if (s->fmt == DR_8888) {
                u32 *p = &((u32 *)s->base)[j * s->stride + i], v = *p;
                *p = (v & 0xFF000000u) | (((v >> 2) & 0x3F3F3Fu) + ((v >> 3) & 0x1F1F1Fu));
                continue;
            }
            u16 *p = &((u16 *)s->base)[j * s->stride + i];
            u32 v = *p, r, g, b, a;
            switch (s->fmt) {
            case DR_565:  r = v & 31; g = (v >> 5) & 63; b = (v >> 11) & 31; a = 0; break;
            case DR_5551: r = v & 31; g = (v >> 5) & 31; b = (v >> 10) & 31; a = v & 0x8000; break;
            default:      r = v & 15; g = (v >> 4) & 15; b = (v >> 8) & 15; a = v & 0xF000; break;
            }
            r = (r >> 2) + (r >> 3); g = (g >> 2) + (g >> 3); b = (b >> 2) + (b >> 3);
            switch (s->fmt) {
            case DR_565:  *p = (u16)(r | (g << 5) | (b << 11)); break;
            case DR_5551: *p = (u16)(r | (g << 5) | (b << 10) | a); break;
            default:      *p = (u16)(r | (g << 4) | (b << 8) | a); break;
            }
        }
}

void dr_view_dim_around(const DrSurface *s)
{
    dr_dim(s, 0, 0, DR_W, DR_VY);
    dr_dim(s, 0, DR_VY + DR_VH, DR_W, DR_H - DR_VY - DR_VH);
    dr_dim(s, 0, DR_VY, DR_VX, DR_VH);
    dr_dim(s, DR_VX + DR_VW, DR_VY, DR_W - DR_VX - DR_VW, DR_VH);
}

static void footer(const DrSurface *s, int n, const char *const *keys, const char *const *labels, const u8 *font)
{
    int x = DR_VX + 8, y = DR_VY + DR_VH - 12;
    dr_rect(s, DR_VX, DR_VY + DR_VH - 18, DR_VW, 1, DR_C_HI);
    for (int i = 0; i < n; i++) {
        if (keys[i][0] == '^') { glyph8(s, x, y, g_tri, DR_C_WHITE); x += 13; }
        else if (keys[i][0] == 'o' && !keys[i][1]) { glyph8(s, x, y, g_circ, DR_C_WHITE); x += 13; }
        else { dr_text(s, x, y, keys[i], DR_C_WHITE, font); x += 8 * slen(keys[i]) + 4; }
        dr_text(s, x, y, labels[i], DR_C_GREY, font);
        x += 8 * slen(labels[i]) + 14;
    }
}

void dr_view(const DrSurface *s, const DrView *v, const u8 *font)
{
    int x = DR_VX, y = DR_VY, w = DR_VW, h = DR_VH;
    dr_rect(s, x, y, w, h, DR_C_BG);
    dr_rect(s, x, y, w, 2, DR_C_HI);
    /* header */
    text_n(s, x + 8, y + 8, v->game, 40, DR_C_WHITE, font);
    text_right(s, x + w - 8, y + 8, v->count, DR_C_OK, font);
    dr_text(s, x + 8, y + 20, "Emblem", DR_C_GOLD, font);
    text_right(s, x + w - 8, y + 20, v->points, DR_C_GREY, font);
    dr_rect(s, x + 8, y + 32, w - 16, 4, DR_C_ICONBG);
    if (v->total > 0 && v->got > 0) {
        int fw = (w - 16) * v->got / v->total;
        dr_rect(s, x + 8, y + 32, fw < 2 ? 2 : fw, 4, DR_C_GOLD);
    }
    if (v->page == 0) {
        int top = y + 42, rh = 24;
        for (int r = 0; r < v->nvis && r < DR_VROWS; r++) {
            const DrRow *row = &v->rows[r];
            int ry = top + r * rh;
            if (r == v->cursor) dr_rect(s, x + 4, ry - 2, w - 14, rh - 1, DR_C_HI);
            cup16(s, x + 8, ry + 1, row->earned ? DR_C_GOLD : DR_C_LOCK);
            text_n(s, x + 30, ry + 1, row->title, 42, row->earned ? DR_C_WHITE : DR_C_DIM, font);
            text_right(s, x + w - 14, ry + 1, row->pts, row->earned ? DR_C_GOLD : DR_C_DIM, font);
            text_n(s, x + 30, ry + 12, row->sub, 42, row->earned ? DR_C_GREY : DR_C_LOCK, font);
        }
        if (v->nall > DR_VROWS) {                                    /* where you are in the list */
            int bx = x + w - 6, by = top - 2, bh = DR_VROWS * rh;
            int th = bh * DR_VROWS / v->nall;
            dr_rect(s, bx, by, 2, bh, DR_C_ICONBG);
            dr_rect(s, bx, by + bh * v->first / v->nall, 2, th < 6 ? 6 : th, DR_C_GREY);
        }
        static const char *const k[] = { "UP/DOWN", "L/R", "^", "o" }, *const l[] = { "Scroll", "Page", "Values", "Close" };
        footer(s, 4, k, l, font);
    } else {
        dr_text(s, x + 8, y + 48, "Values Emblem reads", DR_C_OK, font);
        for (int k = 0; k < v->nval; k++) {
            int cx = x + 8 + (k % 2) * 216, cy = y + 64 + (k / 2) * 14;
            text_n(s, cx, cy, v->vname[k], 7, DR_C_GREY, font);
            text_n(s, cx + 64, cy, v->vval[k], 18, DR_C_WHITE, font);
        }
        int ly = y + 64 + ((v->nval + 1) / 2) * 14 + 10;
        dr_text(s, x + 8, ly, "Ready", DR_C_GREY, font);
        dr_text(s, x + 72, ly, v->ready, v->ready_ok ? DR_C_OK : DR_C_GOLD, font);
        dr_text(s, x + 8, ly + 20, "File", DR_C_GREY, font);
        text_n(s, x + 72, ly + 20, v->file, 46, DR_C_WHITE, font);
        text_n(s, x + 72, ly + 32, v->file2, 46, DR_C_GREY, font);
        dr_text(s, x + 8, ly + 52, "Build", DR_C_GREY, font);
        text_n(s, x + 72, ly + 52, v->build, 46, DR_C_WHITE, font);
        static const char *const k[] = { "^", "o" }, *const l[] = { "List", "Close" };
        footer(s, 2, k, l, font);
    }
}
