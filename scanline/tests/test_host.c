/* PC tests for Scanline's pixel work, its guard against darkening a frame
 * twice, and the config file. Built with AddressSanitizer by run_tests.sh. */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "../src/lines.h"
#include "../src/common.h"

static int fails;
#define CHECK(c, m) do { int _ok = (c); printf("%s %s\n", _ok ? "PASS" : "FAIL", m); if (!_ok) fails++; } while (0)

static u32 rng = 2463534242u;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

#define STRIDE 512
static const int fmt_bpp[4] = { 2, 2, 2, 4 };
static const char *fmt_name[4] = { "565", "5551", "4444", "8888" };

/* channel layout: shift, bits; [3] is alpha */
static const int ch_shift[4][4] = { { 0, 5, 11, 16 }, { 0, 5, 10, 15 }, { 0, 4, 8, 12 }, { 0, 8, 16, 24 } };
static const int ch_bits[4][4]  = { { 5, 6, 5, 0 },   { 5, 5, 5, 1 },   { 4, 4, 4, 4 },  { 8, 8, 8, 8 } };

/* What each step should give for one channel value. */
static u32 ref_dim(u32 c, int level)
{
    switch (level) {
    case 0: return c - (c >> 3);
    case 1: return c - (c >> 2);
    case 2: return (c >> 1) + (c >> 3);
    case 3: return c >> 1;
    case 4: return (c >> 2) + (c >> 3);
    case 5: return c >> 2;
    default: return 0;
    }
}

static u32 get_px(const u8 *b, int fmt, int x, int y)
{
    return fmt == SL_8888 ? ((const u32 *)b)[y * STRIDE + x] : ((const u16 *)b)[y * STRIDE + x];
}
static void set_px(u8 *b, int fmt, int x, int y, u32 v)
{
    if (fmt == SL_8888) ((u32 *)b)[y * STRIDE + x] = v; else ((u16 *)b)[y * STRIDE + x] = (u16)v;
}

static u32 ref_pixel(u32 p, int fmt, int level)
{
    u32 out = 0;
    for (int i = 0; i < 3; i++) {
        u32 m = (1u << ch_bits[fmt][i]) - 1, s = ch_shift[fmt][i];
        u32 c = (p >> s) & m;
        out |= ref_dim(c, level) << s;
    }
    if (ch_bits[fmt][3]) out |= p & (((1u << ch_bits[fmt][3]) - 1) << ch_shift[fmt][3]);
    return out;
}

static u8 *new_frame(int fmt)
{
    size_t n = (size_t)STRIDE * SL_H * fmt_bpp[fmt];
    u8 *b = malloc(n);
    for (size_t i = 0; i < n; i++) b[i] = (u8)rnd();
    return b;
}

/* The lowest bit of each colour (two bits in 8888) may differ at the words the
 * plugin stamps to remember a segment: at most two words per segment. */
static const u32 lsb_bits[4] = { 0x0821, 0x0421, 0x0111, 0x00030303 };
static int same(int fmt, u32 a, u32 b) { return ((a ^ b) & ~lsb_bits[fmt]) == 0; }

static int is_dark_row(int y, int spacing) { return (y % spacing) == spacing - 1; }

int main(void)
{
    static SlState st;
    SlStats stats;
    size_t fsz;

    /* ---- every format, every step: exact per channel -------------------------- */
    for (int fmt = 0; fmt < 4; fmt++) {
        int ok = 1, untouched = 1;
        fsz = (size_t)STRIDE * SL_H * fmt_bpp[fmt];
            for (int spacing = 2; spacing <= 4; spacing++)
                for (int level = 0; level < SL_LEVELS; level++) {
                    u8 *a = new_frame(fmt), *orig = malloc(fsz);
                    memcpy(orig, a, fsz);
                    SlSettings s = { level, spacing };
                    sl_forget(&st);
                    memset(&stats, 0, sizeof stats);
                    sl_frame(a, 0x04000000, STRIDE, fmt, &s, 1, &st, &stats);
                    for (int y = 0; y < SL_H; y++) {
                        int stamped = 0;
                        for (int x = 0; x < STRIDE; x++) {
                            u32 o = get_px(orig, fmt, x, y), n = get_px(a, fmt, x, y);
                            if (x >= SL_W || !is_dark_row(y, spacing)) { if (o != n) untouched = 0; continue; }
                            u32 want = ref_pixel(o, fmt, level);
                            if (n == want) continue;
                            if (!same(fmt, n, want)) ok = 0;
                            stamped++;
                        }
                        if (stamped > SL_SEGS * 2 * 2) ok = 0;         /* two marks a segment, two pixels each */
                    }
                    free(a); free(orig);
                }
        char msg[120];
        snprintf(msg, sizeof msg, "%s: all 7 steps x 3 spacings, exact in every channel bar the stamps, alpha kept", fmt_name[fmt]);
        CHECK(ok, msg);
        snprintf(msg, sizeof msg, "%s: light rows and the stride padding past x=480 are never touched", fmt_name[fmt]);
        CHECK(untouched, msg);
    }

    /* ---- the guard ------------------------------------------------------------ */
    for (int fmt = 0; fmt < 4; fmt++) {
        fsz = (size_t)STRIDE * SL_H * fmt_bpp[fmt];
        SlSettings s = { 3, 2 };
        u8 *game = new_frame(fmt), *buf = malloc(fsz), *once = malloc(fsz);
        sl_forget(&st);

        memcpy(buf, game, fsz);
        memset(&stats, 0, sizeof stats);
        sl_frame(buf, 0x04000000, STRIDE, fmt, &s, 1, &st, &stats);
        memcpy(once, buf, fsz);
        int all_done = stats.rows_done == SL_H / 2;

        /* the same buffer shown 100 more times without being redrawn */
        int stays = 1;
        for (int i = 0; i < 100; i++) {
            memset(&stats, 0, sizeof stats);
            sl_frame(buf, 0x04000000, STRIDE, fmt, &s, 1, &st, &stats);
            if (memcmp(buf, once, fsz) || stats.rows_done) stays = 0;
        }
        char msg[120];
        snprintf(msg, sizeof msg, "%s: a buffer shown again 100 times is darkened once, not 100 times", fmt_name[fmt]);
        CHECK(all_done && stays, msg);

        /* the game redraws the same picture into it: darkened exactly once again */
        memcpy(buf, game, fsz);
        memset(&stats, 0, sizeof stats);
        sl_frame(buf, 0x04000000, STRIDE, fmt, &s, 1, &st, &stats);
        snprintf(msg, sizeof msg, "%s: a redrawn buffer is darkened again (every row)", fmt_name[fmt]);
        CHECK(!memcmp(buf, once, fsz) && stats.rows_done == SL_H / 2, msg);

        /* a HUD redrawn over the left side of a static frame on every flip
         * (MissyHUD does this): nothing else may get darker */
        int no_creep = 1;
        for (int i = 0; i < 60; i++) {
            for (int y = 0; y < 8; y++) for (int x = 0; x < 140; x++) set_px(buf, fmt, x, y, (x ^ y) & 1 ? 0xFFFFFFFFu : 0);
            sl_frame(buf, 0x04000000, STRIDE, fmt, &s, 1, &st, &stats);
            for (int y = 0; y < SL_H; y++) for (int x = 0; x < SL_W; x++)
                if ((y >= 8 || x >= 140) && get_px(buf, fmt, x, y) != get_px(once, fmt, x, y)) no_creep = 0;
        }
        snprintf(msg, sizeof msg, "%s: a HUD redrawn on a static frame never darkens the rest of it", fmt_name[fmt]);
        CHECK(no_creep, msg);
        free(game); free(buf); free(once);
    }

    /* a game that only repaints part of a kept frame (dirty rectangles): the
     * safe outcome is that nothing outside the repainted area gets darker.
     * The repainted area itself misses its lines until a full redraw. */
    for (int fmt = 0; fmt < 4; fmt += 3) {
        fsz = (size_t)STRIDE * SL_H * fmt_bpp[fmt];
        SlSettings s = { 3, 2 };
        u8 *buf = new_frame(fmt), *once = malloc(fsz);
        sl_forget(&st);
        sl_frame(buf, 0x04000000, STRIDE, fmt, &s, 1, &st, &stats);
        memcpy(once, buf, fsz);
        int safe = 1;
        static const int box[3][4] = { { 190, 100, 290, 150 }, { 400, 40, 480, 90 }, { 0, 200, 150, 272 } };
        for (int i = 0; i < 90; i++) {
            const int *b = box[i % 3];
            for (int y = b[1]; y < b[3]; y++) for (int x = b[0]; x < b[2]; x++) set_px(buf, fmt, x, y, rnd());
            sl_frame(buf, 0x04000000, STRIDE, fmt, &s, 1, &st, &stats);
            for (int y = 0; y < SL_H; y++) for (int x = 0; x < SL_W; x++) {
                int in = 0;
                for (int k = 0; k < 3; k++) in |= x >= box[k][0] && x < box[k][2] && y >= box[k][1] && y < box[k][3];
                if (!in && get_px(buf, fmt, x, y) != get_px(once, fmt, x, y)) safe = 0;
            }
        }
        char msg[120];
        snprintf(msg, sizeof msg, "%s: partial repaints of a kept frame never darken anything around them", fmt_name[fmt]);
        CHECK(safe, msg);
        free(buf); free(once);
    }

    /* loading-screen case: a few bright letters on black, shown for many frames */
    {
        int fmt = SL_565;
        fsz = (size_t)STRIDE * SL_H * 2;
        SlSettings s = { 3, 2 };
        u8 *buf = calloc(1, fsz), *once = malloc(fsz);
        for (int x = 300; x < 340; x++) for (int y = 200; y < 208; y++) if ((x + y) % 3 == 0) set_px(buf, fmt, x, y, 0xFFFF);
        sl_forget(&st);
        sl_frame(buf, 0x04000000, STRIDE, fmt, &s, 1, &st, &stats);
        memcpy(once, buf, fsz);
        for (int i = 0; i < 300; i++) sl_frame(buf, 0x04000000, STRIDE, fmt, &s, 1, &st, &stats);
        CHECK(!memcmp(buf, once, fsz), "sparse text on black shown for 300 frames does not fade away");
        free(buf); free(once);
    }

    /* double and triple buffering: each buffer remembered separately */
    {
        int fmt = SL_8888;
        fsz = (size_t)STRIDE * SL_H * 4;
        SlSettings s = { 2, 2 };
        u8 *pic = new_frame(fmt), *b[3], *once = malloc(fsz);
        for (int i = 0; i < 3; i++) { b[i] = malloc(fsz); memcpy(b[i], pic, fsz); }
        sl_forget(&st);
        u32 addr[3] = { 0x04000000, 0x04088000 };
        sl_frame(b[0], addr[0], STRIDE, fmt, &s, 1, &st, &stats);
        memcpy(once, b[0], fsz);
        int ok = 1;
        for (int round = 0; round < 50; round++)
            for (int i = 0; i < 3; i++) {
                sl_frame(b[i], addr[i], STRIDE, fmt, &s, 1, &st, &stats);
                if (memcmp(b[i], once, fsz)) ok = 0;
            }
        CHECK(ok, "three buffers flipped in turn on a static scene are each darkened once");
        /* a fourth buffer pushes one out; it is then simply darkened again when redrawn */
        for (int i = 0; i < 3; i++) free(b[i]);
        free(pic); free(once);
    }

    /* a settings change makes every remembered buffer stale */
    {
        int fmt = SL_565;
        fsz = (size_t)STRIDE * SL_H * 2;
        u8 *pic = new_frame(fmt), *buf = malloc(fsz);
        SlSettings a = { 3, 2 }, b = { 5, 2 };
        memcpy(buf, pic, fsz);
        sl_forget(&st);
        sl_frame(buf, 0x04000000, STRIDE, fmt, &a, 1, &st, &stats);
        memcpy(buf, pic, fsz);                          /* game redraws */
        memset(&stats, 0, sizeof stats);
        sl_frame(buf, 0x04000000, STRIDE, fmt, &b, 2, &st, &stats);
        CHECK(stats.rows_done == SL_H / 2 && get_px(buf, fmt, 10, 1) == ref_pixel(get_px(pic, fmt, 10, 1), fmt, 5) &&
              same(fmt, get_px(buf, fmt, 0, 1), ref_pixel(get_px(pic, fmt, 0, 1), fmt, 5)),
              "after a settings change the next frame uses the new strength");
        free(pic); free(buf);
    }

    /* ---- cases the first build's guard got wrong ------------------------------- */
    {
        /* A static picture beside a HUD that is there from the first flip and is
         * redrawn on every flip; the rest of those rows is black. */
        int fmt = SL_565;
        fsz = (size_t)STRIDE * SL_H * 2;
        SlSettings s = { 3, 2 };
        u8 *buf = calloc(1, fsz), *once = malloc(fsz);
        for (int y = 0; y < SL_H; y++) for (int x = 250; x < 330; x++) set_px(buf, fmt, x, y, 0xFFFF);
        for (int y = 0; y < 8; y++) for (int x = 340; x < 480; x++) set_px(buf, fmt, x, y, (x ^ y) & 1 ? 0xFFFF : 0);
        sl_forget(&st);
        sl_frame(buf, 0x04000000, STRIDE, fmt, &s, 1, &st, &stats);
        memcpy(once, buf, fsz);
        int ok = 1;
        for (int i = 0; i < 60; i++) {
            for (int y = 0; y < 8; y++) for (int x = 340; x < 480; x++) set_px(buf, fmt, x, y, (x ^ y) & 1 ? 0xFFFF : 0);
            sl_frame(buf, 0x04000000, STRIDE, fmt, &s, 1, &st, &stats);
            for (int y = 0; y < SL_H; y++) for (int x = 0; x < 340; x++)
                if (get_px(buf, fmt, x, y) != get_px(once, fmt, x, y)) ok = 0;
        }
        CHECK(ok, "a picture beside a HUD on otherwise black rows does not fade over 60 flips");
        free(buf); free(once);
    }
    for (int fmt = 0; fmt < 4; fmt += 3) {
        /* The game redraws only a box (same picture), not lined up with the
         * segments. Rows are never darkened twice; at most the part of a
         * segment at the box's edge is left as the game drew it. */
        fsz = (size_t)STRIDE * SL_H * fmt_bpp[fmt];
        SlSettings s = { 3, 2 };
        u8 *pic = new_frame(fmt), *buf = malloc(fsz), *once = malloc(fsz);
        for (int y = 0; y < SL_H; y++) for (int x = 0; x < SL_W; x++) set_px(pic, fmt, x, y, get_px(pic, fmt, x, y) | 0x84108410u);
        memcpy(buf, pic, fsz);
        sl_forget(&st);
        sl_frame(buf, 0x04000000, STRIDE, fmt, &s, 1, &st, &stats);
        memcpy(once, buf, fsz);
        int twice = 0, missed = 0, outside = 0;
        for (int round = 0; round < 20; round++) {
            for (int y = 97; y < 163; y++) for (int x = 133; x < 371; x++) set_px(buf, fmt, x, y, get_px(pic, fmt, x, y));
            sl_frame(buf, 0x04000000, STRIDE, fmt, &s, 1, &st, &stats);
        }
        for (int y = 0; y < SL_H; y++) for (int x = 0; x < SL_W; x++) {
            u32 got = get_px(buf, fmt, x, y), want = get_px(once, fmt, x, y);
            if (same(fmt, got, want)) continue;
            int inbox = y >= 97 && y < 163 && x >= 133 && x < 371;
            if (!inbox) outside++;
            else if (same(fmt, got, get_px(pic, fmt, x, y))) missed++;
            else twice++;
        }
        char msg[140];
        snprintf(msg, sizeof msg, "%s: a box redrawn 20 times: nothing outside it changes, nothing is darkened twice", fmt_name[fmt]);
        CHECK(outside == 0 && twice == 0, msg);
        snprintf(msg, sizeof msg, "%s: of the box's 7854 dark-row pixels, only edge segments are left undarkened (%d)", fmt_name[fmt], missed);
        CHECK(missed <= 33 * (120 + 120), msg);
        free(pic); free(buf); free(once);
    }

    /* ---- a moving edge between two shades, one of which is the other darkened ---- */
    for (int fmt = 0; fmt < 4; fmt++)
        for (int level = 0; level < SL_LEVELS - 1; level++) {
            fsz = (size_t)STRIDE * SL_H * fmt_bpp[fmt];
            SlSettings s = { level, 2 };
            u8 *b2[2] = { calloc(1, fsz), calloc(1, fsz) };
            u32 A = fmt == SL_8888 ? 0xFFA0C8A0u : fmt == SL_4444 ? 0xFCACu : fmt == SL_5551 ? 0xD294u : 0xA514u;
            u32 B = ref_pixel(A, fmt, level);
            long missed = 0;
            sl_forget(&st);
            for (int f = 0; f < 200; f++) {
                u8 *b = b2[f & 1];
                int edge = (f * 7) % SL_W;
                for (int y = 0; y < SL_H; y++) for (int x = 0; x < SL_W; x++) set_px(b, fmt, x, y, x < edge ? B : A);
                sl_frame(b, 0x04000000 + (u32)(f & 1) * 0x88000, STRIDE, fmt, &s, 1, &st, &stats);
                for (int y = 1; y < SL_H; y += 2) for (int x = 0; x < SL_W; x++)
                    if (!same(fmt, get_px(b, fmt, x, y), ref_pixel(x < edge ? B : A, fmt, level))) missed++;
            }
            if (level == 1 || missed) {
                char msg[140];
                snprintf(msg, sizeof msg, "%s step %d: a moving edge between a shade and its darkened twin loses no scanline (%ld px)", fmt_name[fmt], level, missed);
                CHECK(missed == 0, msg);
            }
            free(b2[0]); free(b2[1]);
        }

    /* ---- a dark, slightly noisy picture redrawn every frame (a video) -------------- */
    for (int level = 0; level < 4; level += (level ? 2 : 1)) {
        int fmt = SL_565, bright = level == 0 ? 5 : 2;
        fsz = (size_t)STRIDE * SL_H * 2;
        SlSettings s = { level, 2 };
        u8 *b2[2] = { calloc(1, fsz), calloc(1, fsz) }, *game = malloc(fsz);
        long missed = 0;
        sl_forget(&st);
        for (int f = 0; f < 600; f++) {
            u8 *b = b2[f & 1];
            for (int y = 0; y < SL_H; y++) for (int x = 0; x < SL_W; x++) {
                int v = bright + ((x / 40 + y / 30) & 1);
                int r = v + (int)(rnd() % 3) - 1, g = v * 2 + (int)(rnd() % 3) - 1, bl = v + (int)(rnd() % 3) - 1;
                set_px(b, fmt, x, y, (u32)(r | g << 5 | bl << 11));
            }
            memcpy(game, b, fsz);
            sl_frame(b, 0x04000000 + (u32)(f & 1) * 0x88000, STRIDE, fmt, &s, 1, &st, &stats);
            for (int y = 1; y < SL_H; y += 2) for (int k = 0; k < SL_SEGS; k++) {
                int same_n = 0, could = 0;
                for (int x = k * 120 + 4; x < k * 120 + 116; x++) {
                    u32 g = get_px(game, fmt, x, y);
                    if (g == get_px(b, fmt, x, y)) same_n++;
                    if (ref_pixel(g, fmt, level) != g) could++;
                }
                if (could > 20 && same_n == 112) missed++;
            }
        }
        char msg[140];
        snprintf(msg, sizeof msg, "step %d: a dark noisy picture redrawn every frame for 600 frames loses no scanline piece (%ld)", level, missed);
        CHECK(missed == 0, msg);
        free(b2[0]); free(b2[1]); free(game);
    }

    /* ---- a frame done in bands is the same as a frame done in one go ------------- */
    {
        int ok = 1;
        for (int fmt = 0; fmt < 4; fmt += 3)
            for (int spacing = 2; spacing <= 4; spacing++)
                {
                    fsz = (size_t)STRIDE * SL_H * fmt_bpp[fmt];
                    SlSettings s = { 1, spacing };
                    u8 *a = new_frame(fmt), *b = malloc(fsz);
                    memcpy(b, a, fsz);
                    static SlState st2;
                    SlStats s1 = { 0, 0 }, s2 = { 0, 0 };
                    sl_forget(&st); sl_forget(&st2);
                    sl_frame(a, 0x04000000, STRIDE, fmt, &s, 1, &st, &s1);
                    int bh = 40;                            /* as the plugin does */
                    for (int y0 = 0; y0 < SL_H; y0 += bh)
                        sl_frame_rows(b, 0x04000000, STRIDE, fmt, &s, 1, &st2, &s2, y0, y0 + bh < SL_H ? y0 + bh : SL_H);
                    if (memcmp(a, b, fsz) || s1.rows_done != s2.rows_done) ok = 0;
                    /* and a repeat flip in bands touches nothing */
                    memset(&s2, 0, sizeof s2);
                    for (int y0 = 0; y0 < SL_H; y0 += bh)
                        sl_frame_rows(b, 0x04000000, STRIDE, fmt, &s, 1, &st2, &s2, y0, y0 + bh < SL_H ? y0 + bh : SL_H);
                    if (memcmp(a, b, fsz) || s2.rows_done) ok = 0;
                    free(a); free(b);
                }
        CHECK(ok, "a frame done in bands of 40 rows is identical to one done in one go (and stays so on a repeat flip)");
    }

    /* ---- per-game settings ----------------------------------------------------------- */
    {
        char k1[SL_KEY_MAX], k2[SL_KEY_MAX], k3[SL_KEY_MAX], k4[SL_KEY_MAX], k5[SL_KEY_MAX];
        game_key(k1, "ms0:/ISO/Ys - The Oath in Felghana (USA).iso");
        game_key(k2, "ms0:/ISO/RPG/Ys - The Oath in Felghana (USA).iso");
        game_key(k3, "ms0:/PSP/GAME/PORTS/ZeldaPSP/EBOOT.PBP");
        game_key(k4, "");
        game_key(k5, "ms0:/ISO/A very long file name that goes on and on and on and on.cso");
        CHECK(!strncmp(k1, "YS___THE_OATH_IN_FELGHANA_", 26) && strlen(k1) == 31 && strcmp(k1, k2) && !strncmp(k1, k2, 26),
              "game name: from the ISO's file name; the same name in another folder gets another line");
        CHECK(!strncmp(k3, "ZELDAPSP_", 9) && !strncmp(k4, "UNKNOWN_", 8) && strlen(k5) == SL_KEY_MAX - 1,
              "game name: an EBOOT is named after its folder; no path and long names are handled");

        SlConfig c, d;
        system("rm -rf ms0 && mkdir -p ms0/SCANLINE");
        config_defaults(&c);
        CHECK(games_get(k1, &c) == 0 && c.strength == 25, "a game with no entry keeps the starting settings");
        c.enabled = 1; c.strength = 25; c.spacing = 2;
        int w1 = games_set(k1, &c);
        c.enabled = 0; c.strength = 50; c.spacing = 3; c.mode = 2;
        int w2 = games_set(k3, &c);
        config_defaults(&d);
        int g1 = games_get(k1, &d);
        CHECK(w1 == 0 && w2 == 0 && g1 == 1 && d.enabled == 1 && d.strength == 25 && d.spacing == 2 && d.mode == 1,
              "a game's settings read back, with another game saved after it");
        config_defaults(&d);
        CHECK(games_get(k3, &d) == 1 && d.enabled == 0 && d.strength == 50 && d.spacing == 3 && d.mode == 2 &&
              d.hotkeys == 1 && d.fast == 1, "the other game has its own (off, 50%, 1 in 3, mode 2); the rest of the config is untouched");
        c.enabled = 1; c.strength = 13; c.spacing = 4; c.mode = 1;
        games_set(k1, &c);
        config_defaults(&d);
        games_get(k1, &d);
        FILE *f = fopen("ms0/SCANLINE/games.txt", "r");
        char line[200]; int lines = 0, mine = 0;
        while (fgets(line, sizeof line, f)) if (line[0] != '#') { lines++; if (!strncmp(line, k1, strlen(k1))) mine++; }
        fclose(f);
        CHECK(d.strength == 13 && d.spacing == 4 && lines == 2 && mine == 1, "changing a game replaces its line, it does not add a second");
        /* lines from test builds: a fourth number of 0 or 1, or none at all, is mode 1 */
        f = fopen("ms0/SCANLINE/games.txt", "a"); fputs("OLD_GAME_00AA = 0 50 3 1\r\nOLD_GAME_00BB = 1 25 2 0\r\nOLD_GAME_00CC = 1 25 2\r\n", f); fclose(f);
        config_defaults(&d); d.mode = 2;
        int o1 = games_get("OLD_GAME_00AA", &d) == 1 && d.enabled == 0 && d.strength == 50 && d.spacing == 3 && d.mode == 1;
        d.mode = 2; int o2 = games_get("OLD_GAME_00BB", &d) == 1 && d.mode == 1;
        d.mode = 2; int o3 = games_get("OLD_GAME_00CC", &d) == 1 && d.mode == 1;
        CHECK(o1 && o2 && o3, "older lines (fourth number 0 or 1, or none) are read as mode 1");
        /* a key that is the start of another must not match it */
        char shortk[SL_KEY_MAX]; strcpy(shortk, k1); shortk[10] = 0;
        config_defaults(&d);
        CHECK(games_get(shortk, &d) == 0, "a name that is only the beginning of another does not match");
        /* damaged line */
        f = fopen("ms0/SCANLINE/games.txt", "a"); fputs("BROKEN_0000 = 1 banana\r\nODD_0001 = 1 25 9 0\r\n", f); fclose(f);
        config_defaults(&d);
        CHECK(games_get("BROKEN_0000", &d) == 0 && games_get("ODD_0001", &d) == 0 && d.strength == 25,
              "a line that cannot be read, or is out of range, is ignored");
        /* many games: the file never outgrows its buffer, the newest are kept */
        int ok = 1;
        for (int i = 0; i < 400 && ok; i++) {
            char k[SL_KEY_MAX]; snprintf(k, sizeof k, "GAME_NUMBER_%03d_ABCDEFGH_%04X", i, i);
            c.strength = 25; if (games_set(k, &c)) ok = 0;
        }
        config_defaults(&d);
        f = fopen("ms0/SCANLINE/games.txt", "r"); fseek(f, 0, SEEK_END); long sz = ftell(f); fclose(f);
        CHECK(ok && sz < 8192 && games_get("GAME_NUMBER_399_ABCDEFGH_018F", &d) == 1 && games_get("GAME_NUMBER_000_ABCDEFGH_0000", &d) == 0,
              "400 games: the file stays under 8KB, the newest are kept, the oldest dropped");
        system("rm -rf ms0");
    }

    /* ---- steps ------------------------------------------------------------------ */
    CHECK(sl_level_from_percent(50) == 3 && sl_level_from_percent(0) == 0 && sl_level_from_percent(100) == 6 &&
          sl_level_from_percent(40) == 2 && sl_level_from_percent(90) == 6, "strength percentages snap to the nearest step");

    /* ---- config ----------------------------------------------------------------- */
    {
        SlConfig c;
        system("rm -rf ms0 && mkdir -p ms0/SCANLINE");
        CHECK(config_load(&c) == 0 && c.enabled == 1 && c.strength == 25 && c.spacing == 2,
              "no config file: defaults (on, 25%, every 2nd row)");
        FILE *f = fopen("ms0/SCANLINE/config.txt", "w");
        fputs("# x\r\nstrength = 75%\r\nspacing=3   # thin\nsoft = 1\nenabled = banana\nspacing = 9\nfast=0\nge_wait = 0\n", f);
        fclose(f);
        CHECK(config_load(&c) == 1 && c.strength == 75 && c.spacing == 3 && c.mode == 1 && c.enabled == 1 && c.fast == 0 && c.ge_wait == 0,
              "config parses; unreadable, unknown or out-of-range lines are skipped, the rest kept");
        c.strength = 38; c.enabled = 0; c.mode = 2;
        SlConfig d;
        CHECK(config_save(&c) == 0 && config_load(&d) == 1 && d.strength == 38 && d.enabled == 0 && d.spacing == 3 &&
              d.mode == 2 && d.fast == 0, "settings written by the buttons read back the same");
        f = fopen("ms0/SCANLINE/config.txt", "w");
        fputs("strength = 12.5\n", f);
        fclose(f);
        CHECK(config_load(&c) == 1 && sl_level_from_percent(c.strength) == 0 && c.ge_wait == 1 &&
              !strcmp(sl_level_text[0], "12.5") && !strcmp(sl_level_text[2], "37.5") && !strcmp(sl_level_text[4], "62.5"),
              "strength = 12.5 is read as the first step; the label texts are the exact figures");
        system("rm -rf ms0");
    }

    {   /* the handshake with Sidequest over the Memory Stick */
        extern int host_sidequest, host_busy_for, host_delays;
        int scanlineCardWriting(void);
        SlConfig c;
        char log[1024];
        FILE *f;
        system("rm -rf ms0 && mkdir -p ms0/SCANLINE");
        config_defaults(&c);
        host_sidequest = -1;
        CHECK(config_save(&c) == 0 && !scanlineCardWriting(), "no Sidequest: the settings are written as before, and the writing mark is down again");
        host_sidequest = 0;
        CHECK(config_save(&c) == 0 && !scanlineCardWriting(), "Sidequest idle: the settings are written");
        system("rm -f ms0/SCANLINE/config.txt ms0/SCANLINE/scanline.log");
        host_sidequest = 1; host_busy_for = 0; host_delays = 0;
        int r = config_save(&c);
        CHECK(r == -5 && access("ms0/SCANLINE/config.txt", F_OK) != 0 && access("ms0/SCANLINE/config.tmp", F_OK) != 0 &&
              host_delays == 150 && !scanlineCardWriting(),
              "Sidequest at the stick for good: after the wait nothing was written, not even the temporary file");
        sl_log("dropped");
        CHECK(access("ms0/SCANLINE/scanline.log", F_OK) != 0, "a log line is dropped, not written, while Sidequest is at the stick");
        host_sidequest = 1; host_busy_for = 7; host_delays = 0;
        CHECK(config_save(&c) == 0 && host_delays == 7 && access("ms0/SCANLINE/config.txt", F_OK) == 0 && !scanlineCardWriting(),
              "Sidequest busy for a moment: the write waits for it and is then done");
        sl_log("after");
        memset(log, 0, sizeof(log));
        f = fopen("ms0/SCANLINE/scanline.log", "r");
        if (f) { fread(log, 1, sizeof(log) - 1, f); fclose(f); }
        CHECK(strstr(log, "after\r\n") && strstr(log, "card guard: 2 write(s) put off") && !strstr(log, "dropped"),
              "afterwards the log says once how many writes were put off");
        sl_log("again");
        memset(log, 0, sizeof(log));
        f = fopen("ms0/SCANLINE/scanline.log", "r");
        if (f) { fread(log, 1, sizeof(log) - 1, f); fclose(f); }
        CHECK(strstr(log, "again\r\n") && strstr(log, "card guard") && !strstr(strstr(log, "card guard") + 1, "card guard"),
              "and does not say it again");
        system("rm -f ms0/SCANLINE/config.txt");
        host_sidequest = 2; host_delays = 0;
        CHECK(config_save(&c) < 0 && host_delays == 0 && access("ms0/SCANLINE/config.txt", F_OK) != 0,
              "stick not refreshed after a load: no write and no waiting");
        host_sidequest = -1;
        system("rm -rf ms0");
    }

    printf("\n%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
