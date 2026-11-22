#ifndef AC_DRAW_H
#define AC_DRAW_H
#include <psptypes.h>

#define DR_W 480
#define DR_H 272
#define DR_565  0
#define DR_5551 1
#define DR_4444 2
#define DR_8888 3

/* colours 0x00BBGGRR, the same as Sidequest's and Checkpoint's */
#define DR_C_WHITE  0x00FFFFFFu
#define DR_C_GREY   0x00A0A0A0u
#define DR_C_BG     0x00301A10u
#define DR_C_HI     0x00805020u
#define DR_C_OK     0x0060E060u
#define DR_C_GOLD   0x0030C0F0u
#define DR_C_ICONBG 0x0018100Au
#define DR_C_DIM    0x00707070u                 /* locked titles */
#define DR_C_LOCK   0x00404040u                 /* locked cups and "LOCKED" */

#define DR_CARD_W    288
#define DR_CARD_COLS 29                         /* (288 - 52) / 8 */
#define DR_LINE_MAX  40
#define DR_SLIDE_MS  250
#define DR_HOLD_MS   5000
#define DR_SHOW_MS   (DR_SLIDE_MS * 2 + DR_HOLD_MS)

typedef struct { u8 *base; int stride; int fmt; } DrSurface;

u32  dr_conv(int fmt, u32 bgr);
void dr_rect(const DrSurface *s, int x, int y, int w, int h, u32 color);
void dr_text(const DrSurface *s, int x, int y, const char *t, u32 fg, const u8 *font);
int  dr_wrap(const char *title, int cols, char lines[2][DR_LINE_MAX]);
int  dr_card_height(const char *title);
void dr_card(const DrSurface *s, int x, int y, const char *head, const char *title, const char *foot, const u8 *font);
int  dr_card_y(int h, u32 t_ms);

/* ---- the viewer (L+R+LEFT): a panel over the frozen game ---- */
#define DR_VX 16
#define DR_VY 10
#define DR_VW 448
#define DR_VH 252
#define DR_VROWS 8                              /* achievements on one page */
typedef struct { const char *title, *pts, *sub; int earned; } DrRow;
typedef struct {
    const char *game, *count, *points;          /* "YS: THE OATH IN FELGHANA", "4 OF 89", "18 / 1170 PTS" */
    int got, total;                             /* the progress bar */
    int page;                                   /* 0 list, 1 values */
    const DrRow *rows; int nvis, cursor, first, nall;   /* the rows on screen; cursor -1: none */
    const char *const *vname, *const *vval; int nval;   /* the values page */
    const char *ready; int ready_ok;
    const char *file, *file2, *build;
} DrView;
void dr_dim(const DrSurface *s, int x, int y, int w, int h);   /* darkens to 3/8 */
void dr_view_dim_around(const DrSurface *s);                  /* dims the game around the panel */
void dr_view(const DrSurface *s, const DrView *v, const u8 *font);
#endif
