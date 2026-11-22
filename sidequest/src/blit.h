#ifndef PS_BLIT_H
#define PS_BLIT_H
#include <psptypes.h>
#define C_WHITE  0x00FFFFFF
#define C_GREY   0x00A0A0A0
#define C_BG     0x00301A10
#define C_HI     0x00805020
#define C_OK     0x0060E060
#define C_WARN   0x0040C0FF
#define C_ERR    0x004040FF
int  blit_setup(void);
void blit_rect(int x, int y, int w, int h, u32 color);
void blit_text(int x, int y, const char *s, u32 fg, u32 bg);
void blit_text_uc(int x, int y, const char *s, u32 fg, u32 bg);
void blit_bar(int x, int y, int w, int h, u32 done, u32 total, u32 fg, u32 bg);
/* Slot pictures: the 480x272 screen at half size, RGB565. */
#define SHOT_W 240
#define SHOT_H 136
int  blit_grab_shot(u16 *out);
/* One row of RGB565 pixels. mode 1: as is; 2: each pixel twice as wide;
 * 0: every second pixel (half width). */
void blit_span565(int x, int y, const u16 *px, int count, int mode);

#endif
