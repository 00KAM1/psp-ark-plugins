/*
 * Checkpoint - draw text and boxes straight onto the displayed framebuffer.
 * Supports all four PSP pixel formats. Uses the 8x8 MSX font from pspdebug.
 */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspdisplay_kernel.h>
#include <string.h>
#include "blit.h"
#include "jtext.h"

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

/* Keep a piece of the screen and put it back later, in the screen's own
 * pixel format. The rectangle must lie on the screen. Returns the bytes used
 * (4 a pixel at most), 0 if there is no screen. */
static int copy_rect(int x, int y, int w, int h, u8 *buf, int out)
{
    if (!g_vram || x < 0 || y < 0 || x + w > 480 || y + h > 272) return 0;
    int bpp = g_fmt == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 4 : 2;
    for (int j = 0; j < h; j++) {
        u8 *row = g_vram + ((y + j) * g_bw + x) * bpp;
        if (out) memcpy(buf + j * w * bpp, row, w * bpp);
        else     memcpy(row, buf + j * w * bpp, w * bpp);
    }
    return w * h * bpp;
}
int blit_copy_out(int x, int y, int w, int h, void *buf)      { return copy_rect(x, y, w, h, buf, 1); }
int blit_copy_in(int x, int y, int w, int h, const void *buf) { return copy_rect(x, y, w, h, (u8 *)buf, 0); }

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
    if (!g_vram) return;
    u32 f = conv(fg), b = conv(bg), cp;
    /* The text is UTF-8. English letters come from the MSX font, Japanese from
     * the 8x8 table; each character takes one 8-dot cell. */
    for (; (cp = jt_next(&s)) != 0; x += 8) {
        const u8 *glyph;
        cp = jt_fold(cp);
        if (cp >= 'a' && cp <= 'z') cp -= 32;
        if (cp < 0x20) cp = ' ';
        if (cp < 0x80) glyph = &msx[cp * 8];
        else if (!(glyph = jt_glyph(cp))) glyph = &msx['?' * 8];
        for (int row = 0; row < 8; row++) {
            u8 bits = glyph[row];
            for (int col = 0; col < 8; col++)
                pset(x + col, y + row, (bits & (0x80 >> col)) ? f : b);
        }
    }
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

/* Shrink the 480x272 screen to 120x68 by averaging 4x4 blocks. */
int blit_grab_thumb(u16 *out)
{
    if (blit_setup() < 0 || g_bw < 480) return -1;
    for (int ty = 0; ty < THUMB_H; ty++)
        for (int tx = 0; tx < THUMB_W; tx++) {
            u32 sr = 0, sg = 0, sb = 0, r, g, b;
            for (int dy = 0; dy < 4; dy++)
                for (int dx = 0; dx < 4; dx++) {
                    getrgb(tx * 4 + dx, ty * 4 + dy, &r, &g, &b);
                    sr += r; sg += g; sb += b;
                }
            out[ty * THUMB_W + tx] = (u16)((sr >> 7) | ((sg >> 6) << 5) | ((sb >> 7) << 11));
        }
    return 0;
}

void blit_thumb(int x, int y, const u16 *img)
{
    for (int ty = 0; ty < THUMB_H; ty++)
        for (int tx = 0; tx < THUMB_W; tx++) {
            u32 p = img[ty * THUMB_W + tx];
            u32 c = ((p & 31) << 3) | (((p >> 5) & 63) << 10) | (((p >> 11) & 31) << 19);
            pset(x + tx, y + ty, conv(c));
        }
}
