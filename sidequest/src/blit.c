/*
 * Sidequest - draw text and boxes straight onto the displayed framebuffer.
 * Supports all four PSP pixel formats. Uses the 8x8 MSX font from pspdebug.
 */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspdisplay_kernel.h>
#include <string.h>
#include "blit.h"

extern u8 msx[];

static u8  *g_vram;
static int  g_bw, g_fmt;

int blit_setup(void)
{
    void *top = NULL;
    int bw = 0, fmt = 0;
    sceDisplayGetFrameBuf(&top, &bw, &fmt, PSP_DISPLAY_SETBUF_IMMEDIATE);
    if (!top || bw == 0) return -1;
    g_vram = (u8 *)((u32)top | 0x40000000u);   /* uncached alias */
    g_bw = bw;
    g_fmt = fmt;
    return 0;
}

static inline u32 conv(u32 c)   /* c is 0x00BBGGRR */
{
    u32 r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    switch (g_fmt) {
    case PSP_DISPLAY_PIXEL_FORMAT_565:  return (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11);
    case PSP_DISPLAY_PIXEL_FORMAT_5551: return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | 0x8000;
    case PSP_DISPLAY_PIXEL_FORMAT_4444: return (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | 0xF000;
    default:                            return c | 0xFF000000u;
    }
}

static inline void pset(int x, int y, u32 pc)
{
    if (x < 0 || y < 0 || x >= 480 || y >= 272) return;
    if (g_fmt == PSP_DISPLAY_PIXEL_FORMAT_8888) ((u32 *)g_vram)[y * g_bw + x] = pc;
    else ((u16 *)g_vram)[y * g_bw + x] = (u16)pc;
}

void blit_rect(int x, int y, int w, int h, u32 color)
{
    if (!g_vram) return;
    u32 pc = conv(color);
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) pset(i, j, pc);
}

void blit_text(int x, int y, const char *s, u32 fg, u32 bg)
{
    if (!g_vram) return;
    u32 f = conv(fg), b = conv(bg);
    for (; *s; s++, x += 8) {
        const u8 *glyph = &msx[(u8)*s * 8];
        for (int row = 0; row < 8; row++) {
            u8 bits = glyph[row];
            for (int col = 0; col < 8; col++)
                pset(x + col, y + row, (bits & (0x80 >> col)) ? f : b);
        }
    }
}

/* Same as blit_text, but shows a-z as A-Z. The whole menu uses it. */
void blit_text_uc(int x, int y, const char *s, u32 fg, u32 bg)
{
    char buf[80];
    int i = 0;
    for (; s[i] && i < (int)sizeof(buf) - 1; i++)
        buf[i] = (s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i];
    buf[i] = 0;
    blit_text(x, y, buf, fg, bg);
}

void blit_bar(int x, int y, int w, int h, u32 done, u32 total, u32 fg, u32 bg)
{
    u32 t = total >> 12, d = done >> 12;   /* keep it in 32 bits */
    int fillw = t ? (int)(d * (u32)w / t) : 0;
    if (fillw > w) fillw = w;
    blit_rect(x, y, w, h, bg);
    blit_rect(x, y, fillw, h, fg);
}

/* One screen pixel as 8-bit r, g, b whatever the game's display format is. */
static inline void getrgb(int x, int y, u32 *r, u32 *g, u32 *b)
{
    if (g_fmt == PSP_DISPLAY_PIXEL_FORMAT_8888) {
        u32 p = ((u32 *)g_vram)[y * g_bw + x];
        *r = p & 0xFF; *g = (p >> 8) & 0xFF; *b = (p >> 16) & 0xFF;
        return;
    }
    u32 p = ((u16 *)g_vram)[y * g_bw + x];
    switch (g_fmt) {
    case PSP_DISPLAY_PIXEL_FORMAT_565:  *r = (p & 31) << 3; *g = ((p >> 5) & 63) << 2; *b = ((p >> 11) & 31) << 3; break;
    case PSP_DISPLAY_PIXEL_FORMAT_5551: *r = (p & 31) << 3; *g = ((p >> 5) & 31) << 3; *b = ((p >> 10) & 31) << 3; break;
    default:                            *r = (p & 15) << 4; *g = ((p >> 4) & 15) << 4; *b = ((p >> 8) & 15) << 4; break;
    }
}

/* Shrink the 480x272 screen to 240x136 by averaging 2x2 blocks. */
int blit_grab_shot(u16 *out)
{
    if (blit_setup() < 0 || g_bw < 480) return -1;
    for (int ty = 0; ty < SHOT_H; ty++)
        for (int tx = 0; tx < SHOT_W; tx++) {
            u32 sr = 0, sg = 0, sb = 0, r, g, b;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    getrgb(tx * 2 + dx, ty * 2 + dy, &r, &g, &b);
                    sr += r; sg += g; sb += b;
                }
            /* sums of four 8-bit values: >>5 gives 5 bits, >>4 gives 6 */
            out[ty * SHOT_W + tx] = (u16)((sr >> 5) | ((sg >> 4) << 5) | ((sb >> 5) << 11));
        }
    return 0;
}

void blit_span565(int x, int y, const u16 *px, int count, int mode)
{
    int step = mode == 0 ? 2 : 1;
    for (int i = 0; i < count; i += step) {
        u32 p = px[i];
        u32 c = conv(((p & 31) << 3) | (((p >> 5) & 63) << 10) | (((p >> 11) & 31) << 19));
        if (mode == 2) { pset(x, y, c); pset(x + 1, y, c); x += 2; }
        else           { pset(x, y, c); x++; }
    }
}
