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
int  blit_copy_out(int x, int y, int w, int h, void *buf);     /* screen -> buf; returns bytes used */
int  blit_copy_in(int x, int y, int w, int h, const void *buf); /* buf -> screen; returns bytes used */
void blit_bar(int x, int y, int w, int h, u32 done, u32 total, u32 fg, u32 bg);
#define THUMB_W 120
#define THUMB_H 68
int  blit_grab_thumb(u16 *out);                 /* quarter-size copy of the screen, RGB565 */
void blit_thumb(int x, int y, const u16 *img);
#endif
