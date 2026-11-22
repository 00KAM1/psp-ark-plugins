/* Host render: reads a 480x272 RGB888 file, converts to a PSP pixel format at
 * stride 512, runs the plugin's own sl_frame()/sl_osd(), writes RGB888 back.
 * usage: render_tool in.rgb out.rgb fmt level spacing font.bin [label lines] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../scanline/src/lines.h"
int main(int argc, char **argv)
{
    if (argc < 7) return 2;
    int fmt = atoi(argv[3]), level = atoi(argv[4]), spacing = atoi(argv[5]);
    static u8 rgb[SL_W * SL_H * 3], font[2048];
    static u32 fb[512 * SL_H];
    FILE *f = fopen(argv[1], "rb"); if (!f || fread(rgb, 1, sizeof rgb, f) != sizeof rgb) return 3; fclose(f);
    f = fopen(argv[6], "rb"); if (!f || fread(font, 1, 2048, f) != 2048) return 4; fclose(f);
    u16 *fb16 = (u16 *)fb;
    for (int y = 0; y < SL_H; y++) for (int x = 0; x < SL_W; x++) {
        u8 *p = &rgb[(y * SL_W + x) * 3]; u32 r = p[0], g = p[1], b = p[2];
        if (fmt == SL_8888) fb[y * 512 + x] = r | g << 8 | b << 16 | 0xFF000000u;
        else if (fmt == SL_565) fb16[y * 512 + x] = (u16)((r >> 3) | (g >> 2) << 5 | (b >> 3) << 11);
    }
    static SlState st; SlStats stats = { 0, 0, 0 };
    SlSettings s = { level, spacing };
    if (level >= 0) sl_frame((u8 *)fb, 0x04000000, 512, fmt, &s, 1, &st, &stats);
    for (int i = 7; i < argc; i++)                      /* label lines, top right corner */
        if (argv[i][0]) sl_osd_corner((u8 *)fb, 512, fmt, i - 7, argv[i], font);
    for (int y = 0; y < SL_H; y++) for (int x = 0; x < SL_W; x++) {
        u8 *p = &rgb[(y * SL_W + x) * 3];
        if (fmt == SL_8888) { u32 v = fb[y * 512 + x]; p[0] = v; p[1] = v >> 8; p[2] = v >> 16; }
        else { u32 v = fb16[y * 512 + x]; p[0] = (u8)((v & 31) << 3 | (v & 31) >> 2); p[1] = (u8)(((v >> 5) & 63) << 2 | ((v >> 5) & 63) >> 4); p[2] = (u8)(((v >> 11) & 31) << 3 | ((v >> 11) & 31) >> 2); }
    }
    f = fopen(argv[2], "wb"); fwrite(rgb, 1, sizeof rgb, f); fclose(f);
    return 0;
}
