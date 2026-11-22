/* PC tests: the earned file, moving Achieve's progress over, and the viewer's drawing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/ach.h"
#include "../src/draw.h"
extern int g_fail_write;
static int fails;
#define CHECK(c, m) do { int _ok = (c); printf("%s %s\n", _ok ? "PASS" : "FAIL", m); if (!_ok) fails++; } while (0)
u32 em_r8(u32 a) { (void)a; return 0; }
u32 em_r16(u32 a) { (void)a; return 0; }
u32 em_r32(u32 a) { (void)a; return 0; }

int main(void)
{
    if (system("rm -rf ms0 && mkdir -p ms0/EMBLEM/earned ms0/ACHIEVE/ULUS10558")) return 2;
    const char *p = "ms0:/EMBLEM/earned/ULUS10558.txt";
    static AcSet set;
    CHECK(ac_load(p, &set) == 0 && ac_count(&set) == 0, "no file: nothing earned");
    CHECK(ac_append(p, "ULUS10558", "Ys: The Oath in Felghana", 73, "20261122_140312", "Smart Water", 10) == 0 &&
          ac_append(p, "ULUS10558", "Ys: The Oath in Felghana", 200, "20261122_150000", "A big number", 5) == 0, "two unlocks written");
    CHECK(ac_load(p, &set) == 2 && set.has[73] && set.has[200] && !set.has[1] && !strcmp(set.date[73], "2026-11-22 14:03"), "both read back, with their dates");
    FILE *f = fopen("ms0/EMBLEM/earned/ULUS10558.txt", "r"); char txt[400] = ""; if (fread(txt, 1, sizeof txt - 1, f)) {} fclose(f);
    const char *head = "# Achievements earned in Ys: The Oath in Felghana (ULUS10558), kept by Emblem\r\n";
    CHECK(!strncmp(txt, head, strlen(head)) &&
          strstr(txt, "73 | 2026-11-22 14:03 | Smart Water | 10 pts\r\n"), "the file is plain readable text");
    f = fopen("ms0/EMBLEM/earned/ULUS10558.txt", "a"); fputs("garbage\r\n999 | x\r\n12 nope\r\n0 | zero\r\n5|\r\n", f); fclose(f);
    CHECK(ac_load(p, &set) == 3 && set.has[5] && !set.date[5][0] && ac_count(&set) == 3, "foreign lines are skipped; a line without a date still counts");
    g_fail_write = 1; CHECK(ac_append(p, "X", "X", 1, "20261122_160000", "X", 5) < 0, "a short write is reported"); g_fail_write = 0;
    f = fopen("ms0/EMBLEM/earned/BIG.txt", "w");
    for (int i = 1; i <= 255; i++) fprintf(f, "%d | 2026-11-%02d 10:%02d | An achievement with quite a long title, number %d of them | 10 pts\r\n", i, 1 + i % 28, i % 60, i);
    fputs("7 | 2027-01-01 00:00 | earned again after an old save state | 5 pts\r\n200 | 2027", f); fclose(f);
    CHECK(ac_load("ms0:/EMBLEM/earned/BIG.txt", &set) == 255 && ac_count(&set) == 255 && !strcmp(set.date[255], "2026-11-04 10:15") && !strcmp(set.date[7], "2026-11-08 10:07"),
          "a long file (255 lines, 20 KB) is read whole; a number listed twice keeps its first date");
    /* Achieve 0.2's file, moved over */
    f = fopen("ms0/ACHIEVE/ULUS10558/unlocked.txt", "w");
    fputs("# Achievements unlocked in Ys: The Oath in Felghana (ULUS10558), kept by Achieve\r\n47 | 2026-11-22 12:31 | Getting to Know You | 3 pts\r\n", f); fclose(f);
    CHECK(ac_copy_file("ms0:/ACHIEVE/ULUS10558/unlocked.txt", "ms0:/EMBLEM/earned/NEW.txt") == 0, "Achieve's file is copied");
    CHECK(ac_load("ms0:/EMBLEM/earned/NEW.txt", &set) == 1 && set.has[47] && !strcmp(set.date[47], "2026-11-22 12:31"), "  ... and Getting to Know You stays earned, with its date");
    CHECK(ac_copy_file("ms0:/ACHIEVE/NONE/unlocked.txt", "ms0:/EMBLEM/earned/X.txt") < 0, "no old file: nothing copied");
    if (system("rm -rf ms0")) {}

    /* the viewer never draws outside the panel, in any pixel format; the dimming only touches the edges */
    static u32 fb[512 * 272];
    for (int fmt = 0; fmt < 4; fmt++) {
        u8 font[2048]; memset(font, 0xFF, sizeof font);
        memset(fb, 0x5A, sizeof fb);
        DrSurface s = { (u8 *)fb, 512, fmt };
        static DrRow rows[DR_VROWS];
        for (int i = 0; i < DR_VROWS; i++) { rows[i].title = "A Harder Sinister Marauder from Afar and more words"; rows[i].pts = "100 pts"; rows[i].sub = "Earned 2026-11-22 12:31"; rows[i].earned = i & 1; }
        static const char *vn[9] = { "LEVEL", "GOLD", "ELIXIR", "MODE", "DIFF", "COMBO", "A", "B", "C" }, *vv[9] = { "8", "1585", "-1", "0", "1", "37", "-2147483648", "4294967295", "x" };
        DrView v; memset(&v, 0, sizeof v);
        v.game = "A very long game title that keeps going and going"; v.count = "89 of 89"; v.points = "1170 / 1170 pts"; v.got = 89; v.total = 89;
        v.rows = rows; v.nvis = DR_VROWS; v.cursor = 3; v.first = 81; v.nall = 89; v.vname = vn; v.vval = vv; v.nval = 9;
        v.ready = "Yes"; v.ready_ok = 1; v.file = "games/ULUS10558.txt"; v.file2 = "89 achievements"; v.build = "0.3  Nov 22 2026 01:50:00";
        int bpp = fmt == 3 ? 4 : 2, ok = 1;
        for (int page = 0; page < 2; page++) { v.page = page; dr_view(&s, &v, font); }
        for (int y = 0; y < 272; y++) for (int x = 0; x < 512; x++) {
            int in = x >= DR_VX && x < DR_VX + DR_VW && y >= DR_VY && y < DR_VY + DR_VH;
            u8 *px = (u8 *)fb + (y * 512 + x) * bpp;
            int untouched = 1; for (int k = 0; k < bpp; k++) if (px[k] != 0x5A) untouched = 0;
            if (!in && !untouched) ok = 0;
        }
        dr_view_dim_around(&s);
        int dimmed = 1;
        for (int y = 0; y < 272; y++) for (int x = 0; x < 480; x++) {
            int in = x >= DR_VX && x < DR_VX + DR_VW && y >= DR_VY && y < DR_VY + DR_VH;
            u8 *px = (u8 *)fb + (y * 512 + x) * bpp;
            if (!in && px[0] == 0x5A) dimmed = 0;
        }
        u8 *pad = (u8 *)fb + (100 * 512 + 500) * bpp;
        char m[96]; snprintf(m, sizeof m, "format %d: the viewer stays inside its panel; only the edges around it are dimmed", fmt);
        CHECK(ok && dimmed && pad[0] == 0x5A, m);
    }
    printf("\n%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
