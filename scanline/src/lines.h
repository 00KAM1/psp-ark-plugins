/*
 * Scanline - the pixel work. No PSP dependencies, so the same file is built
 * into the plugin and into the PC tests and renders.
 */
#ifndef SL_LINES_H
#define SL_LINES_H

#include <psptypes.h>

#define SL_W 480
#define SL_H 272

/* Display pixel formats, numbered as the PSP numbers them. */
#define SL_565  0
#define SL_5551 1
#define SL_4444 2
#define SL_8888 3

/* How dark a scanline is, as steps. Each step is a sum of shifted copies of
 * the pixel, so a whole 32-bit word (two pixels in the 16-bit formats) is
 * done with a few shifts, masks and adds, never per channel. */
#define SL_LEVELS 7
extern const u8 sl_level_percent[SL_LEVELS];    /* 13 25 38 50 63 75 100: whole numbers, for the config file */
extern const char *const sl_level_text[SL_LEVELS];  /* the exact figures: 12.5 25 37.5 50 62.5 75 100 */
int sl_level_from_percent(int percent);         /* nearest step */

typedef struct {
    int level;      /* 0..SL_LEVELS-1 */
    int spacing;    /* one dark row every `spacing` rows: 2, 3 or 4 */
} SlSettings;

/* What the plugin remembers about a buffer it has darkened, so that a buffer
 * shown again without being redrawn is not darkened a second time.
 *
 * Each dark row is cut into SL_SEGS segments of 120 pixels (a finer cut was
 * measured: it costs more in bookkeeping, and in cache, than it is worth). For each segment
 * the first and the last word that a second pass would still change are
 * remembered, with the value left there. On the next flip of that buffer:
 *   - either value still there: the segment is already dark, leave it;
 *   - both gone: the game has drawn over it, darken it again.
 * Two marks, one at each end, because an overlay that is redrawn on every
 * flip (a HUD) usually covers one end of a segment at most: the end it does
 * not cover then says "still dark", and the picture beside the overlay is
 * not darkened over and over. Segments, because a game that redraws only
 * part of the screen gets that part darkened again and the rest left alone.
 * A segment with nothing to remember is black: doing it again changes
 * nothing, so it is simply done again. */
#define SL_SEGS 4
#define SL_NOMARK 0xFFFF

typedef struct {
    u32 val[SL_SEGS * 2];                       /* [2k] first, [2k+1] last */
    u16 off[SL_SEGS * 2];                       /* word in the row; [2k+1] is SL_NOMARK when there is none */
    u16 hsh[SL_SEGS * 2];                       /* the two words beside it, as a 16-bit sum */
} SlRowMark;

#define SL_SLOTS 3                              /* triple buffering at most */
#define SL_DARK_ROWS (SL_H / 2)                 /* the most there can be: spacing 2 */

typedef struct {
    u32 addr;                                   /* physical address of the buffer, 0 = unused */
    u32 key;                                    /* format, stride, settings, generation */
    u32 used;                                   /* for picking the oldest slot */
    SlRowMark row[SL_DARK_ROWS];                /* by dark row number, top to bottom */
} SlSlot;

typedef struct {
    SlSlot slot[SL_SLOTS];
    u32 clock;
} SlState;

typedef struct {
    u32 rows_done;
    u32 rows_skipped;                           /* every segment already dark */
    u32 segs_done;
} SlStats;

/* Darken one frame in place. `base` must point at the first pixel and be
 * writable; `stride` is in pixels and at least SL_W. `gen` changes whenever
 * the settings change, which makes every remembered buffer stale. */
void sl_frame(u8 *base, u32 addr, int stride, int fmt, const SlSettings *s, u32 gen,
              SlState *st, SlStats *stats);

/* The same for the dark rows from y0 up to (not including) y1 only, so a
 * frame can be done from the top in bands. */
void sl_frame_rows(u8 *base, u32 addr, int stride, int fmt, const SlSettings *s, u32 gen,
                   SlState *st, SlStats *stats, int y0, int y1);

/* Forget every buffer (used when the settings change). */
void sl_forget(SlState *st);

/* A small label box drawn over a frame, 8x8 font; (x, y) is where the text starts. */
void sl_osd(u8 *base, int stride, int fmt, int x, int y, const char *text, const u8 *font);

/* Label line `line` (0 or 1), its box flush against the top right corner. */
void sl_osd_corner(u8 *base, int stride, int fmt, int line, const char *text, const u8 *font);

#endif
