/*
 * Scanline - the pixel work. See lines.h.
 *
 * Every dark row is processed a 32-bit word at a time. In the 16-bit formats
 * a word holds two pixels, and both are darkened by the same few operations:
 * shifting the word right by k divides every channel by 2^k, and a mask then
 * clears the bits that slid in from the channel (or pixel) above. Sums of
 * such terms never carry from one channel into the next, because the
 * fractions they add up to are below one. Alpha bits are kept as they were.
 */
#include "lines.h"

const u8 sl_level_percent[SL_LEVELS] = { 13, 25, 38, 50, 63, 75, 100 };
const char *const sl_level_text[SL_LEVELS] = { "12.5", "25", "37.5", "50", "62.5", "75", "100" };

int sl_level_from_percent(int percent)
{
    int best = 0;
    for (int i = 1; i < SL_LEVELS; i++) {
        int d = percent - sl_level_percent[i], db = percent - sl_level_percent[best];
        if (d < 0) d = -d;
        if (db < 0) db = -db;
        if (d < db) best = i;
    }
    return best;
}

typedef struct {
    u32 m1, m2, m3;     /* what is left of each channel after >>1, >>2, >>3 */
    u32 a;              /* alpha bits */
    u32 l[3];           /* lowest bit of each colour, one pixel */
    u32 sm;             /* the bits a stamp may set, whole word */
    int two;            /* two pixels per word */
    int words;          /* 32-bit words in SL_W pixels */
} Masks;

static void masks_for(int fmt, Masks *m)
{
    static const u32 ch[4][4] = {
        { 0x001F, 0x07E0, 0xF800, 0x0000 },             /* 565  */
        { 0x001F, 0x03E0, 0x7C00, 0x8000 },             /* 5551 */
        { 0x000F, 0x00F0, 0x0F00, 0xF000 },             /* 4444 */
        { 0x0000FF, 0x00FF00, 0xFF0000, 0xFF000000u },  /* 8888 */
    };
    const u32 *c = ch[fmt & 3];
    u32 mk[4] = { 0, 0, 0, 0 };
    for (int k = 1; k <= 3; k++)
        for (int i = 0; i < 3; i++) mk[k] |= (c[i] >> k) & c[i];
    m->m1 = mk[1]; m->m2 = mk[2]; m->m3 = mk[3]; m->a = c[3];
    for (int i = 0; i < 3; i++) m->l[i] = c[i] & (0u - c[i]);
    m->two = fmt != SL_8888;
    m->sm = m->two ? (m->l[0] | m->l[1] | m->l[2]) * 0x10001u : 0x00030303u;
    if (fmt != SL_8888) {                               /* two pixels per word */
        m->m1 |= m->m1 << 16; m->m2 |= m->m2 << 16; m->m3 |= m->m3 << 16; m->a |= m->a << 16;
        m->words = SL_W / 2;
    } else {
        m->words = SL_W;
    }
}

/* The seven steps. `p` is a word, the result keeps (100 - percent)% of it. */
#define DIM0(p) ((p) - (((p) >> 3) & m3))                               /* 13% darker */
#define DIM1(p) ((p) - (((p) >> 2) & m2))                               /* 25% */
#define DIM2(p) (((((p) >> 1) & m1) + (((p) >> 3) & m3)) | ((p) & a))   /* 38% */
#define DIM3(p) ((((p) >> 1) & m1) | ((p) & a))                         /* 50% */
#define DIM4(p) (((((p) >> 2) & m2) + (((p) >> 3) & m3)) | ((p) & a))   /* 63% */
#define DIM5(p) ((((p) >> 2) & m2) | ((p) & a))                         /* 75% */
#define DIM6(p) ((p) & a)                                               /* black */

/* Plain: darken r[0..w) in place. Nothing else in the loop: load, shift,
 * mask, store. The "_na" versions are for 565, which has no alpha: a = 0
 * lets the compiler drop the alpha work entirely. */
#define ROW_PLAIN(N, SUF, AVAL)                                                         \
static void row_plain_##N##SUF(u32 *r, int w, const Masks *mk)                         \
{                                                                                       \
    const u32 m1 = mk->m1, m2 = mk->m2, m3 = mk->m3, a = AVAL;                         \
    (void)m1; (void)m2; (void)m3; (void)a;                                              \
    _Pragma("GCC unroll 4")                                                             \
    for (int i = 0; i < w; i++) { u32 p = r[i]; r[i] = DIM##N(p); }                     \
}

#define BOTH(N) ROW_PLAIN(N, , mk->a) ROW_PLAIN(N, _na, 0)
BOTH(0) BOTH(1) BOTH(2) BOTH(3) BOTH(4) BOTH(5) BOTH(6)

typedef void (*PlainFn)(u32 *, int, const Masks *);
static const PlainFn plain_fn[2][SL_LEVELS] = {
    { row_plain_0, row_plain_1, row_plain_2, row_plain_3, row_plain_4, row_plain_5, row_plain_6 },
    { row_plain_0_na, row_plain_1_na, row_plain_2_na, row_plain_3_na, row_plain_4_na, row_plain_5_na, row_plain_6_na } };
/* The bits that tell whether a plain pass would still change a word. A
 * channel is left as it is by the 13% step when it is below 8, by the 25%
 * step when below 4, by the steps up to 75% only when 0, and by the black
 * step always (that step is idempotent and needs no marks at all). */
static u32 changeable_bits(const Masks *mk, int fmt, int level)
{
    u32 all = fmt == SL_8888 ? 0x00FFFFFFu : ~mk->a;
    if (level == 0) return mk->m3 << 3;
    if (level == 1) return mk->m2 << 2;
    return level == SL_LEVELS - 1 ? 0 : all;
}

/* ---- stamps -------------------------------------------------------------------
 * A remembered word has to be told apart from anything the game might draw
 * there next. The darkened value alone is not enough: pictures are full of
 * pairs of shades where one is exactly the other after darkening (shading,
 * gradients), and when the edge between two such shades moves across a
 * remembered word the game's new pixel equals our old darkened one. The
 * segment then looked "still dark", was skipped, and showed for a frame
 * without its scanline.
 *
 * So the lowest bit of each colour in a remembered word is overwritten with
 * a pattern (a change of one brightness step in 32, at two words per
 * segment, the same every frame):
 *   - in the 16-bit formats the word's two pixels get opposite patterns, so
 *     no word of one flat colour can ever equal it;
 *   - in 8888, where a word is one pixel, the low two bits of each colour,
 *     and the pixel beside it gets the opposite bits; both are looked at.
 * And the stamped word is never what the game itself had there, so a game
 * redrawing the very same picture is always seen as a redraw. */
static u32 stamp_bits(const Masks *mk, u32 n)
{
    u32 a = n & 7, b = mk->two ? ~n & 7 : (n >> 3) & 7;
    u32 pa = (a & 1 ? mk->l[0] : 0) | (a & 2 ? mk->l[1] : 0) | (a & 4 ? mk->l[2] : 0);
    u32 pb = (b & 1 ? mk->l[0] : 0) | (b & 2 ? mk->l[1] : 0) | (b & 4 ? mk->l[2] : 0);
    return pa | (mk->two ? pb << 16 : pb << 1);
}

/* The 64 patterns of each format, worked out once. pat[n ^ 63] is always
 * different from pat[n]. */
static const u32 *patterns(const Masks *mk, int fmt)
{
    static u32 tab[4][64];
    static u8 ready[4];
    if (!ready[fmt]) {
        for (u32 n = 0; n < 64; n++) tab[fmt][n] = stamp_bits(mk, n);
        ready[fmt] = 1;
    }
    return tab[fmt];
}

#define STAMP(o, n) (((o) & ~sm) | pat[(n) & 63])

/* One word through a step, for checking a stamp. */
static u32 dim_word(const Masks *mk, int level, u32 p)
{
    const u32 m1 = mk->m1, m2 = mk->m2, m3 = mk->m3, a = mk->a;
    switch (level) {
    case 0: return DIM0(p);
    case 1: return DIM1(p);
    case 2: return DIM2(p);
    case 3: return DIM3(p);
    case 4: return DIM4(p);
    case 5: return DIM5(p);
    default: return DIM6(p);
    }
}

/* Plain rows, when the game's own word is not at hand: `o` is the darkened
 * word. Whatever the game had there darkens to o, so a stamp that does not
 * darken to o cannot be what the game had. */
static u32 stamp_checked(const Masks *mk, const u32 *pat, int level, u32 o, u32 n)
{
    const u32 sm = mk->sm;
    u32 s = STAMP(o, n);
    if (dim_word(mk, level, s) != o) return s;
    s = STAMP(o, n ^ 63);
    return dim_word(mk, level, s) != o ? s : o;
}

/* Write a stamped word. In 8888 its neighbour (the other word of the pair)
 * is given the opposite low bits. */
static inline void mark_put(const Masks *mk, u32 *r, int i, u32 v)
{
    r[i] = v;
    if (!mk->two) r[i ^ 1] = (r[i ^ 1] & ~mk->sm) | (~v & mk->sm);
}

/* Is the stamped word still there? */
static inline int mark_kept(const Masks *mk, const u32 *r, u32 i, u32 v)
{
    return r[i] == v && (mk->two || ((r[i] ^ r[i ^ 1]) & mk->sm) == mk->sm);
}

/* The two words beside a remembered one (towards the middle of its segment),
 * folded into 16 bits.
 *
 * The stamped word alone was not enough for a dark, slightly noisy picture
 * that is redrawn every frame (a video): there the darkened value is nearly
 * the game's own, and now and then the game's new pixels came out exactly
 * equal to the stamped word. That segment was then taken for "still dark" and
 * shown for one frame without its line (reproduced on the PC: 80 of 600
 * frames; seen on the PSP in Jeanne d'Arc's videos, in her hair). With the
 * neighbours in the comparison the game must hit six pixels, not two. */
static inline u16 near_hash(const u32 *r, int i, int b, int segw)
{
    int d = i + 2 < b + segw ? 1 : -1;
    u32 h = r[i + d] * 0x9E3779B1u + r[i + 2 * d] * 0x85EBCA6Bu;
    return (u16)(h ^ (h >> 16));
}

void sl_forget(SlState *st)
{
    for (int i = 0; i < SL_SLOTS; i++) st->slot[i].addr = 0;
}

static SlSlot *slot_for(SlState *st, u32 addr, u32 key)
{
    SlSlot *old = &st->slot[0];
    st->clock++;
    for (int i = 0; i < SL_SLOTS; i++) {
        SlSlot *s = &st->slot[i];
        if (s->addr == addr && s->key == key) { s->used = st->clock; return s; }
        if (s->used < old->used) old = s;
    }
    /* A buffer we have not darkened yet: nothing remembered, everything is done. */
    old->addr = addr;
    old->key = key;
    old->used = st->clock;
    for (int n = 0; n < SL_DARK_ROWS; n++)
        for (int k = 0; k < SL_SEGS; k++) old->row[n].off[2 * k + 1] = SL_NOMARK;
    return old;
}

void sl_frame(u8 *base, u32 addr, int stride, int fmt, const SlSettings *s, u32 gen,
              SlState *st, SlStats *stats)
{
    sl_frame_rows(base, addr, stride, fmt, s, gen, st, stats, 0, SL_H);
}

void sl_frame_rows(u8 *base, u32 addr, int stride, int fmt, const SlSettings *s, u32 gen,
                   SlState *st, SlStats *stats, int y0, int y1)
{
    Masks mk;
    masks_for(fmt, &mk);
    int level = s->level < 0 ? 0 : s->level >= SL_LEVELS ? SL_LEVELS - 1 : s->level;
    int spacing = s->spacing < 2 ? 2 : s->spacing > 4 ? 4 : s->spacing;
    int sw = fmt == SL_8888 ? stride : stride / 2;      /* row stride in words */
    int segw = mk.words / SL_SEGS;                      /* 60 or 120 words */
    u32 segs = 0;
    u32 key = (u32)fmt | ((u32)stride << 2) | ((u32)level << 14) | ((u32)spacing << 17) | (gen << 21);
    SlSlot *slot = slot_for(st, addr, key);
    PlainFn pf = plain_fn[fmt == SL_565][level];
    u32 cbits = changeable_bits(&mk, fmt, level);
    const u32 *pat = patterns(&mk, fmt);
    const u32 sm = mk.sm;
    int first = y0 <= spacing - 1 ? 0 : (y0 - (spacing - 1) + spacing - 1) / spacing;   /* first dark row in the band */
    SlRowMark *m = slot->row + first;

    for (int y = spacing - 1 + first * spacing; y < y1 && y < SL_H; y += spacing, m++) {
        u32 *r = (u32 *)base + y * sw;
        u32 nonce = (u32)y * 37 + 13;                   /* by place, not by frame: no shimmer */

        /* Which segments has the game drawn over since we darkened them? */
        u32 need = 0;
        for (int k = 0; k < SL_SEGS; k++) {
            u32 ol = m->off[2 * k + 1];
            if (ol == SL_NOMARK ||
                (!(mark_kept(&mk, r, ol, m->val[2 * k + 1]) && near_hash(r, (int)ol, k * segw, segw) == m->hsh[2 * k + 1]) &&
                 !(mark_kept(&mk, r, m->off[2 * k], m->val[2 * k]) && near_hash(r, m->off[2 * k], k * segw, segw) == m->hsh[2 * k])))
                need |= 1u << k;
        }
        if (!need) { stats->rows_skipped++; continue; }
        stats->rows_done++;

        /* What the game has at the two ends of each segment: nearly always
         * where the marks end up, and then the stamp is checked against it. */
        u32 was[SL_SEGS][2];
        for (int k = 0, b = 0; k < SL_SEGS; k++, b += segw) { was[k][0] = r[b]; was[k][1] = r[b + segw - 1]; }

        if (need == (1u << SL_SEGS) - 1) pf(r, mk.words, &mk);          /* the usual case: the whole row */
        else
            for (int k = 0; k < SL_SEGS; ) {                            /* runs of segments */
                int k0 = k;
                while (k < SL_SEGS && (need & (1u << k))) k++;
                if (k > k0) pf(r + k0 * segw, (k - k0) * segw, &mk); else k++;
            }
        for (int k = 0, b = 0; k < SL_SEGS; k++, b += segw) {
            if (!(need & (1u << k))) continue;
            segs++;
            int l = b + segw - 1, f = b;
            while (l >= b && !(r[l] & cbits)) l--;
            if (l < b) { m->off[2 * k + 1] = SL_NOMARK; continue; }
            while (!(r[f] & cbits)) f++;
            if (!mk.two && l == (f ^ 1)) l = f;                         /* a pair is one mark */
            u32 n = nonce + (u32)k * 22, v;
            if (f == b) { v = STAMP(r[f], n); if (v == was[k][0]) v = STAMP(r[f], n ^ 63); }
            else v = stamp_checked(&mk, pat, level, r[f], n);
            mark_put(&mk, r, f, v);
            if (l != f) {
                n += 11;
                if (l == b + segw - 1) { v = STAMP(r[l], n); if (v == was[k][1]) v = STAMP(r[l], n ^ 63); }
                else v = stamp_checked(&mk, pat, level, r[l], n);
                mark_put(&mk, r, l, v);
            }
            m->off[2 * k] = (u16)f;     m->val[2 * k] = r[f];
            m->off[2 * k + 1] = (u16)l; m->val[2 * k + 1] = r[l];
            m->hsh[2 * k] = near_hash(r, f, b, segw); m->hsh[2 * k + 1] = near_hash(r, l, b, segw);
        }
    }
    stats->segs_done += segs;
}

/* ---- label ---------------------------------------------------------------- */
static u32 colour(int fmt, int white)
{
    switch (fmt) {
    case SL_565:  return white ? 0xFFFF : 0x0000;
    case SL_5551: return white ? 0xFFFF : 0x8000;
    case SL_4444: return white ? 0xFFFF : 0xF000;
    default:      return white ? 0xFFFFFFFFu : 0xFF000000u;
    }
}

static void pset(u8 *base, int stride, int fmt, int x, int y, u32 c)
{
    if (x < 0 || y < 0 || x >= SL_W || y >= SL_H) return;
    if (fmt == SL_8888) ((u32 *)base)[y * stride + x] = c;
    else ((u16 *)base)[y * stride + x] = (u16)c;
}

/* Units are drawn small, as on the user's MissyHUD: a 4x5 glyph on the
 * baseline of the 8x8 digits (rows 2 to 6 of the cell), advancing 5 pixels.
 * In a label text, a '%' and the lower-case letters 'm' and 's' are units;
 * everything else is the 8x8 font. Each row holds four pixels, leftmost in
 * bit 3. The three glyphs are the ones the MissyHUD patch uses. */
#define SMALL_ADVANCE 5
static const u8 *small_glyph(char ch)
{
    static const u8 M[5] = { 0x9, 0xF, 0xF, 0x9, 0x9 };
    static const u8 S[5] = { 0x7, 0x8, 0x6, 0x1, 0xE };
    static const u8 PERCENT[5] = { 0x9, 0x2, 0x4, 0x8, 0x9 };
    return ch == 'm' ? M : ch == 's' ? S : ch == '%' ? PERCENT : 0;
}

/* Width of a label text in pixels. */
static int osd_width(const char *text)
{
    int w = 0;
    for (; *text; text++) w += small_glyph(*text) ? SMALL_ADVANCE : 8;
    return w;
}

void sl_osd(u8 *base, int stride, int fmt, int x, int y, const char *text, const u8 *font)
{
    int w = osd_width(text);
    u32 fg = colour(fmt, 1), bg = colour(fmt, 0);
    for (int j = -3; j < 11; j++)                       /* solid box, 3 px around the text */
        for (int i = -4; i < w + 3; i++) pset(base, stride, fmt, x + i, y + j, bg);
    for (; *text; text++) {
        const u8 *sg = small_glyph(*text);
        if (sg) {
            for (int row = 0; row < 5; row++)
                for (int col = 0; col < 4; col++)
                    if (sg[row] & (0x8 >> col)) pset(base, stride, fmt, x + col, y + 2 + row, fg);
            x += SMALL_ADVANCE;
            continue;
        }
        const u8 *g = &font[(u8)*text * 8];
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] & (0x80 >> col)) pset(base, stride, fmt, x + col, y + row, fg);
        x += 8;
    }
}

/* The box reaches 3 pixels past the text on the right and 3 above it, so the
 * text starts 3 in from the edge and 3 down; lines are 13 rows apart. */
void sl_osd_corner(u8 *base, int stride, int fmt, int line, const char *text, const u8 *font)
{
    sl_osd(base, stride, fmt, SL_W - 3 - osd_width(text), 3 + 13 * line, text, font);
}
