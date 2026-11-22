/* PC render: reads a 480x272 RGB888 picture, draws the plugin's own card or
 * card on it with src/draw.c in a PSP pixel format, writes RGB888 back.
 * usage: render_tool in.rgb out.rgb fmt font.bin card <t_ms> <head> <title> <foot>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/draw.h"
int main(int argc, char **argv)
{
    if (argc < 6) return 2;
    int fmt = atoi(argv[3]);
    static u8 rgb[DR_W * DR_H * 3], font[2048];
    static u32 fb[512 * DR_H];
    FILE *f = fopen(argv[1], "rb"); if (!f || fread(rgb, 1, sizeof rgb, f) != sizeof rgb) return 3; fclose(f);
    f = fopen(argv[4], "rb"); if (!f || fread(font, 1, 2048, f) != 2048) return 4; fclose(f);
    u16 *fb16 = (u16 *)fb;
    for (int y = 0; y < DR_H; y++) for (int x = 0; x < DR_W; x++) {
        u8 *p = &rgb[(y * DR_W + x) * 3]; u32 c = p[0] | p[1] << 8 | p[2] << 16;
        if (fmt == DR_8888) fb[y * 512 + x] = dr_conv(fmt, c); else fb16[y * 512 + x] = (u16)dr_conv(fmt, c);
    }
    DrSurface s = { (u8 *)fb, 512, fmt };
    if (!strcmp(argv[5], "card") && argc >= 10) {
        int h = dr_card_height(argv[8]);
        int y = dr_card_y(h, (u32)atoi(argv[6]));
        if (y < DR_H) dr_card(&s, DR_W - DR_CARD_W - 8, y, argv[6 + 1], argv[8], argv[9], font);
    }
    for (int y = 0; y < DR_H; y++) for (int x = 0; x < DR_W; x++) {
        u8 *p = &rgb[(y * DR_W + x) * 3];
        if (fmt == DR_8888) { u32 v = fb[y * 512 + x]; p[0] = v; p[1] = v >> 8; p[2] = v >> 16; }
        else { u32 v = fb16[y * 512 + x]; p[0] = (u8)((v & 31) << 3 | (v & 31) >> 2); p[1] = (u8)(((v >> 5) & 63) << 2 | ((v >> 5) & 63) >> 4); p[2] = (u8)(((v >> 11) & 31) << 3 | ((v >> 11) & 31) >> 2); }
    }
    f = fopen(argv[2], "wb"); fwrite(rgb, 1, sizeof rgb, f); fclose(f);
    return 0;
}
