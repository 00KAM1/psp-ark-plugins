/* PC render of the viewer with the plugin's own drawing code (src/draw.c) and the real
 * game file read by the real engine (src/em.c). Sample progress only.
 * usage: render_view game.txt font.bin in.rgb out.rgb page first cursor file-label build [id=date ...]
 * Each id=date marks an achievement as earned (date as "2026-11-22 18:40"). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/em.h"
#include "../src/draw.h"
u32 em_r8(u32 a) { (void)a; return 0; }
u32 em_r16(u32 a) { (void)a; return 0; }
u32 em_r32(u32 a) { (void)a; return 0; }
int main(int argc, char **argv)
{
    if (argc < 10) return 2;
    static char text[65536]; static EmGame g; static u8 rgb[480 * 272 * 3], font[2048]; static u16 fb[512 * 272];
    FILE *f = fopen(argv[1], "rb"); int n = (int)fread(text, 1, sizeof text - 1, f); fclose(f); text[n] = 0;
    em_load(&g, text, n, NULL);
    f = fopen(argv[2], "rb"); if (fread(font, 1, 2048, f) != 2048) return 3; fclose(f);
    f = fopen(argv[3], "rb"); if (fread(rgb, 1, sizeof rgb, f) != sizeof rgb) return 4; fclose(f);
    for (int i = 0; i < 480 * 272; i++) fb[(i / 480) * 512 + i % 480] = (u16)((rgb[i * 3] >> 3) | ((rgb[i * 3 + 1] >> 2) << 5) | ((rgb[i * 3 + 2] >> 3) << 11));
    int page = atoi(argv[5]), first = atoi(argv[6]), cursor = atoi(argv[7]);
    static struct { int id; const char *date; } earned[64]; int ne = 0;
    for (int i = 10; i < argc && ne < 64; i++) { char *eq = strchr(argv[i], '='); if (!eq) continue; *eq = 0; earned[ne].id = atoi(argv[i]); earned[ne].date = eq + 1; ne++; }
    int ids[256], nall = 0, got = 0, pts = 0, all = 0;
    for (int id = 1; id <= EM_MAX_ID; id++) if (g.ach[id].included) { ids[nall++] = id; all += g.ach[id].points; }
    for (int k = 0; k < ne; k++) if (g.ach[earned[k].id].included) { got++; pts += g.ach[earned[k].id].points; }
    char count[24], points[32], ptsb[8][12], sub[8][32], file2[48]; static DrRow rows[8];
    snprintf(count, sizeof count, "%d of %d", got, nall); snprintf(points, sizeof points, "%d / %d pts", pts, all);
    DrView v; memset(&v, 0, sizeof v);
    v.game = g.title; v.count = count; v.points = points; v.got = got; v.total = nall; v.page = page; v.rows = rows; v.nall = nall;
    v.first = first; v.cursor = cursor - first;
    for (int r = 0; r < 8 && first + r < nall; r++) {
        int id = ids[first + r]; const char *date = 0;
        for (int k = 0; k < ne; k++) if (earned[k].id == id) date = earned[k].date;
        snprintf(ptsb[r], 12, "%d pts", g.ach[id].points);
        if (date) snprintf(sub[r], 32, "Earned %s", date); else snprintf(sub[r], 32, "Locked");
        rows[r].title = g.ach[id].title; rows[r].pts = ptsb[r]; rows[r].sub = sub[r]; rows[r].earned = date != 0; v.nvis++;
    }
    static const char *vn[EM_MAX_SHOW], *vv[EM_MAX_SHOW]; static const char *sample[] = { "8", "1585", "-1", "0", "1", "37" };
    for (int i = 0; i < g.nshow; i++) { vn[i] = g.show_name[i]; vv[i] = i < 6 ? sample[i] : "0"; }
    v.vname = vn; v.vval = vv; v.nval = g.nshow; v.ready = "Yes"; v.ready_ok = 1; v.file = argv[8];
    snprintf(file2, sizeof file2, "%d achievements", nall); v.file2 = file2; v.build = argv[9];
    DrSurface s = { (u8 *)fb, 512, DR_565 };
    dr_view_dim_around(&s); dr_view(&s, &v, font);
    for (int i = 0; i < 480 * 272; i++) { u16 p = fb[(i / 480) * 512 + i % 480]; rgb[i * 3] = (u8)((p & 31) << 3); rgb[i * 3 + 1] = (u8)(((p >> 5) & 63) << 2); rgb[i * 3 + 2] = (u8)(((p >> 11) & 31) << 3); }
    f = fopen(argv[4], "wb"); fwrite(rgb, 1, sizeof rgb, f); fclose(f);
    return 0;
}
