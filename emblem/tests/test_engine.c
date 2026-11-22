/* PC tests for Emblem's rule engine, run against the real Felghana file
 * (games/ULUS10558.txt) on a fake game memory. Achieve 0.2's own conditions
 * (tests/reference/felghana.c) run alongside on the same memory, so the two
 * can be compared tick by tick. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/em.h"
#include "reference/felghana.c"                 /* fg_step, fg_defs and Achieve's address lists */

/* ---- a fake game memory: PSP 0x089F0000 .. 0x08BD0000 ---- */
#define MEM_BASE 0x089F0000u
#define MEM_SIZE 0x001E0000u
static u8 g_mem[MEM_SIZE];
static u8 *at(u32 a) { static u8 dummy[4]; return (a >= MEM_BASE && a + 4 <= MEM_BASE + MEM_SIZE) ? g_mem + (a - MEM_BASE) : dummy; }
u32 em_r8(u32 a) { return *at(a); }
u32 em_r16(u32 a) { u16 v; memcpy(&v, at(a), 2); return v; }
u32 em_r32(u32 a) { u32 v; memcpy(&v, at(a), 4); return v; }
u32 fg_r8(u32 a) { return em_r8(a); }
u32 fg_r16(u32 a) { return em_r16(a); }
u32 fg_r32(u32 a) { return em_r32(a); }
static void w8(u32 a, u32 v) { *at(a) = (u8)v; }
static void w16(u32 a, u32 v) { u16 x = (u16)v; memcpy(at(a), &x, 2); }
static void w32(u32 a, u32 v) { memcpy(at(a), &v, 4); }

static int fails;
#define CHECK(c, m) do { int _ok = (c); printf("%s %s\n", _ok ? "PASS" : "FAIL", m); if (!_ok) fails++; } while (0)

static char g_file[65536];
static int g_flen;
static EmGame g_game;
static int g_errs;
static void on_err(int line, const char *msg, const char *text) { g_errs++; printf("  file line %d: %s: %.80s\n", line, msg, text); }

static void load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    g_flen = (int)fread(g_file, 1, sizeof(g_file) - 1, f);
    fclose(f);
    g_file[g_flen] = 0;
}
static int reload(void)
{
    static char copy[65536];
    memcpy(copy, g_file, g_flen + 1);
    g_errs = 0;
    int n = em_load(&g_game, copy, g_flen, on_err);
    g_game.state_every = 1;                     /* every rule every tick, as Achieve did: the comparisons below are tick for tick */
    return n;
}

static u32 g_ms = 1;
static int g_n, g_met[EM_MAX_ID + 4];
static int steps(int k) { int any = 0; for (int i = 0; i < k; i++) { g_n = em_step(&g_game, g_ms, NULL, g_met, EM_MAX_ID + 4); g_ms += 16; any += g_n; } return any; }
static int has(int id) { for (int i = 0; i < g_n; i++) if (g_met[i] == id) return 1; return 0; }
static void main_game(void) { w16(0x08BC4CCC, 0); w32(0x08BB9B30, 5); w32(0x08BB9AFC, 0x43480000u); w32(0x08BB9B00, 0x43480000u); w32(0x08BB9B2C, 100); }
#define RESET() do { memset(g_mem, 0, MEM_SIZE); reload(); main_game(); } while (0)

/* One Time Attack fight: diff >= 0 moves that difficulty's record; diff < 0 leaves the
 * records and puts -diff in the selection. hurt: HP drops mid-fight; lose: the "lost" flag. */
static void ta_fight(int fight, int diff, int hurt, int lose)
{
    w16(0x08BC4CD8, fight); w16(0x08BC4CCE, 0x28); steps(2);
    w32(0x089F8D48, 0); w16(0x08BC4CCE, 0x29); steps(3);           /* HP not set up yet */
    w32(0x089F8D48, 0x43480000u); steps(40);
    if (hurt) { w32(0x089F8D48, 0x43400000u); steps(2); }
    if (lose) { w8(0x089FC932, 1); steps(2); }
    if (diff >= 0) w32(0x08BBA6A0 + 0xC0 * diff + 0xC * fight, 1000 + (u32)g_ms);
    else w8(0x08BC4E36, (u32)-diff);
    w16(0x08BC4CCE, 0x2A); steps(1);
    w8(0x089FC932, 0); w8(0x08BC4E36, 0);
}

/* ---- the engine on small files of its own ---- */
static int g_every;
static int mini(const char *text, EmGame *g)
{
    static char buf[4096];
    strcpy(buf, text);
    g_errs = 0;
    int n = em_load(g, buf, (int)strlen(buf), on_err);
    g_every = g->state_every; g->state_every = 1;   /* tick for tick unless a test asks otherwise */
    return n;
}

int main(void)
{
    static EmGame m;
    /* ---- reading files ---- */
    {
        int n = mini("game T1 Test Game\nx = u32 08BB0000\n1 | One | 5 | x >= 3\n2 | Two | 10 | none\n", &m);
        CHECK(n == 2 && m.total == 1 && em_included(&m, 1) && !em_included(&m, 2) && !strcmp(m.game, "T1") && !strcmp(m.title, "Test Game") && !g_errs,
              "a small file: game line, a name, a rule; 'none' is listed but not counted");
        printf("  (the next lines are errors the test asks for)\n");
        n = mini("x = u32 08BB0000\n1 | One | 5 | y >= 3\n2 | Two | 5 | x >=\n3 | Three | 5 | x >= 3 junk\n4 | Four | 300 | x set\n"
                 "5 | Five | 5\n6 | Six | 5 | moved x\nlist = u32 1\nx = u8 08BB0001\n7 | Seven | 5 | x set\n7 | Again | 5 | x set\nwhen nosuch\n8 | Eight | 5 | x set\nwhen\n9 | Nine | 5 | x set\n", &m);
        CHECK(g_errs == 11 && em_included(&m, 7) && em_included(&m, 9) && !em_included(&m, 1) && !em_included(&m, 8) && m.total == 2,
              "bad lines are reported one by one and skipped; the rest of the file still works");
        n = mini("a=u32 08BB0000\nb = u32 08BB0004\n1|A|1|a==b\n2 | B | 1 | (a!=b)and(b<=2)\n3 | C | 1 | a>=0x10\n", &m);
        CHECK(!g_errs && m.total == 3, "operators need no spaces around them");
    }
    /* ---- the engine's words, on a tiny file ---- */
    {
        mini("v = s32 08BB0000\nw = u8 08BB0004\nf = u8 08BB0005 bit 2\nready = u32 08BB0008 == 1\n"
             "1 | a | 1 | v set\n2 | b | 1 | v off\n3 | c | 1 | w set\n4 | d | 1 | f set\n5 | e | 1 | v drops 5\n"
             "6 | f | 1 | v rises\n7 | g | 1 | just v set\n8 | h | 1 | was v == 9\n9 | i | 1 | v changes within 1s\n10 | j | 1 | v -3..3\n", &m);
        g_game = m;
        memset(g_mem, 0, MEM_SIZE);
        w32(0x08BB0000, 0xFFFFFFFFu); w8(0x08BB0004, 0xFF); w8(0x08BB0005, 0xFF);
        CHECK(steps(100) == 0, "nothing while ready does not hold");
        w32(0x08BB0008, 1); steps(59);
        CHECK(g_n == 0, "the first second of ready: still nothing");
        steps(1);
        CHECK(!has(1) && has(2) && !has(3) && !has(4) && !has(5) && !has(7) && has(10), "-1, 0xFF and a bit of 0xFF all count as 'not set'");
        w32(0x08BB0000, 9); w8(0x08BB0005, 0x04); steps(1);
        CHECK(has(1) && has(4) && has(6) && has(7) && has(9) && !has(8), "set, bit, rises, just, changes: on the tick it happens");
        w32(0x08BB0000, 4); steps(1);
        CHECK(has(5) && has(8) && !has(7) && has(9), "drops by exactly 5; was = last tick's value; just only once");
        w32(0x08BB0000, 3); steps(1);
        CHECK(!has(5) && has(9), "drops by 1 is not 'drops 5'");
        steps(70);
        CHECK(!has(9), "'within 1s' runs out");
        w32(0x08BB0008, 0); steps(1); w32(0x08BB0008, 1); steps(59); w32(0x08BB0000, 20); steps(1);
        CHECK(!has(6) && !has(5), "after ready drops, the first look has nothing to compare with");
        steps(1); w32(0x08BB0000, 21); steps(1);
        CHECK(has(6), "  ... and the next look does");
    }
    /* ---- what keeps the cost down, and the limits ---- */
    {
        mini("l = list u32 08BB0000 08BB0004 08BB0008\ng1 = u32 08BB0010 == 1\nv = u32 08BB0014\n"
             "1 | count | 1 | count l >= 2\n2 | all | 1 | all l   # a comment after a rule\nwhen g1\n3 | gated | 1 | v rises\n", &m);
        CHECK(!g_errs && m.total == 3 && g_every == 4 && m.nids == 3 && !m.ach[1].timed && !m.ach[2].timed && m.ach[3].timed,
              "a comment after a rule is allowed; rules with no over-time word are marked for every 4th tick");
        g_game = m; g_game.state_every = 4; memset(g_mem, 0, MEM_SIZE);
        w32(0x08BB0000, 1); w32(0x08BB0004, 1); steps(59);
        CHECK(g_n == 0, "  ... nothing in the first second");
        steps(1);
        CHECK(has(1) && !has(2), "  ... the first look after that takes every rule (a loaded save is caught up at once)");
        w32(0x08BB0000, 0); w32(0x08BB0004, 0); steps(8);
        w32(0x08BB0000, 1); w32(0x08BB0004, 1); w32(0x08BB0008, 1);
        int seen1 = 0, seen2 = 0, late = 0;
        for (int t = 0; t < 4; t++) { steps(1); if (has(1)) seen1 = 1; if (has(2)) seen2 = 1; } late = !(seen1 && seen2);
        CHECK(seen1 && seen2 && !late, "  ... afterwards such an achievement comes at most 3 ticks (0.05 s) late");
        w32(0x08BB0014, 5); steps(3); w32(0x08BB0014, 6); steps(1);
        CHECK(!has(3), "a rule under a 'when' that does not hold is not looked at");
        w32(0x08BB0010, 1); w32(0x08BB0014, 7); steps(1);
        CHECK(!has(3), "  ... and on its first look it has no 'tick ago'");
        w32(0x08BB0014, 8); steps(1);
        CHECK(has(3), "  ... from the second look on it does");
        printf("  (the next lines are errors the test asks for)\n");
        static char deep[6000]; int o = 0;
        o += sprintf(deep + o, "a0 = u32 08BB0000\n");
        for (int k = 1; k <= 60; k++) o += sprintf(deep + o, "a%d = a%d and a0\n", k, k - 1);
        o += sprintf(deep + o, "1 | ok | 1 | a30 set\n2 | deep | 1 | a59 set\n3 | brackets | 1 | ((((((((((((((((((((((((((((((a0))))))))))))))))))))))))))))))\n");
        mini(deep, &m);
        CHECK(em_included(&m, 1) && !em_included(&m, 2) && !em_included(&m, 3) && g_errs > 2, "rules nested too deeply are refused, never run");
        mini("f = float 08BB0000\n1 | f | 1 | f set\n", &m);
        g_game = m; memset(g_mem, 0, MEM_SIZE); w32(0x08BB0000, 0xFFFFFFFFu); steps(61);
        CHECK(!has(1), "a float of all ones (-1 as the games write it) is 'not set'");
    }
    /* ---- runs ---- */
    {
        mini("s = u8 08BB0000\nr = u32 08BB0004\nbad = u8 08BB0008\n"
             "1 | run | 1 | run from s == 1 to s == 2 and moved r unless bad == 1 cancel s == 0 grace 0.5s\n", &m);
        g_game = m; memset(g_mem, 0, MEM_SIZE);
        steps(61);
        w8(0x08BB0000, 1); steps(10); w32(0x08BB0004, 7); w8(0x08BB0000, 2); steps(1);
        CHECK(has(1), "run: start, the value moves, finish: met");
        w8(0x08BB0000, 1); steps(10); w8(0x08BB0000, 2); steps(1);
        CHECK(!has(1), "run: finished but nothing moved since it started: not met");
        w8(0x08BB0000, 1); steps(40); w8(0x08BB0008, 1); steps(1); w8(0x08BB0008, 0); w32(0x08BB0004, 8); w8(0x08BB0000, 2); steps(1);
        CHECK(!has(1), "run: spoiled by 'unless' after the grace time");
        w8(0x08BB0000, 1); steps(2); w8(0x08BB0008, 1); steps(1); w8(0x08BB0008, 0); steps(40); w32(0x08BB0004, 9); w8(0x08BB0000, 2); steps(1);
        CHECK(has(1), "run: 'unless' inside the grace time does not count");
        w8(0x08BB0000, 1); steps(5); w8(0x08BB0000, 0); steps(1); w32(0x08BB0004, 10); w8(0x08BB0000, 2); steps(1);
        CHECK(!has(1), "run: cancelled runs never finish");
        w8(0x08BB0000, 0); steps(3); w32(0x08BB0004, 11); w8(0x08BB0000, 1); steps(1); w32(0x08BB0004, 12); w8(0x08BB0000, 2); steps(1);
        CHECK(has(1), "run: what moved is counted from the tick the run started");
        w8(0x08BB0000, 0); steps(3); w8(0x08BB0000, 1); steps(3); w8(0x08BB0000, 3); steps(3); w8(0x08BB0000, 1); steps(1); w32(0x08BB0004, 13); w8(0x08BB0000, 2); steps(1);
        CHECK(has(1), "run: 'from' going off and on again starts it afresh");
    }

    /* ---- the Felghana file ---- */
    load_file("../../games/ULUS10558.txt");
    int n = reload();
    CHECK(n == 89 && g_game.total == 89 && g_errs == 0, "games/ULUS10558.txt: 89 achievements, no errors");
    {
        int ok = 1, pts = 0;
        for (int i = 1; i <= 95; i++) {
            int left = i == 4 || i == 5 || i == 6 || i == 13 || i == 38 || i == 95;
            if (em_included(&g_game, i) == left) ok = 0;
            if (!left && (strcmp(g_game.ach[i].title, fg_defs[i - 1].title) || g_game.ach[i].points != fg_defs[i - 1].points)) ok = 0;
            if (!left) pts += g_game.ach[i].points;
        }
        CHECK(ok && pts == 1170, "the same 89 as Achieve 0.2, same titles and points (1170 in all)");
        const u32 *a; int k;
        ok = 1;
        k = em_list(&g_game, "urns", &a);    if (k != 59 || memcmp(a, FG_URNS, sizeof(FG_URNS))) ok = 0;
        k = em_list(&g_game, "beasts", &a);  if (k != 79 || memcmp(a, FG_MONSTERS, sizeof(FG_MONSTERS))) ok = 0;
        k = em_list(&g_game, "chars", &a);   if (k != 32 || memcmp(a, FG_CHARS, sizeof(FG_CHARS))) ok = 0;
        k = em_list(&g_game, "chests5", &a); if (k != 9 || memcmp(a, FG_CHESTS_V, sizeof(FG_CHESTS_V))) ok = 0;
        CHECK(ok, "the file's lists hold exactly Achieve's addresses");
        printf("  (pool use: %d of %d nodes, %d of %d list addresses, %d runs, %d names, EmGame %d bytes)\n",
               g_game.nnodes, EM_MAX_NODES, g_game.naddrs, EM_MAX_ADDRS, g_game.nruns, g_game.ndefs, (int)sizeof(EmGame));
    }

    /* The Achieve 0.2 checks, now run against the file */
    RESET();
    CHECK(steps(59) == 0, "main game: nothing counts in the first second");
    CHECK(steps(2) == 0, "a fresh game with nothing done unlocks nothing");
    w32(0x08BB9B30, 0); w32(0x08BBA0E4, 1);
    CHECK(steps(200) == 0, "level 0 (title screen, loading): nothing is looked at");
    RESET(); w32(0x08BBA0E4, 1); w16(0x08BC4CCC, 1);
    CHECK(steps(200) == 0, "main-game achievements wait for the main game");
    RESET(); w32(0x08BBA0E4, 1); steps(61);
    CHECK(has(26) && !has(27) && !has(32) && !has(37), "Galbalan on Normal: 26 only");
    RESET(); w32(0x08BBA0E4, 1); w32(0x08BB9EF4, 1); steps(61);
    CHECK(has(26) && has(27) && !has(32), "on Hard: 26 and 27");
    RESET(); w32(0x08BBA0E4, 1); w32(0x08BB9EF4, 2); steps(61);
    CHECK(has(26) && has(27) && has(32) && !has(37), "on Nightmare: 26, 27 and 32");
    RESET(); w32(0x08BBA0E4, 1); w32(0x08BB9EF4, 2); w32(0x08BB9FDC, 1); steps(61);
    CHECK(has(27) && has(37) && !has(32), "on Inferno: 37, not 32");
    RESET(); w32(0x08BBA0E4, 1); w32(0x08BB9EF0, 1); w32(0x08BB9EF4, 2); w32(0x08BB9FDC, 1); steps(61);
    CHECK(has(26) && !has(27) && !has(32) && !has(37), "on Easy or Very Easy: the boss counts, the Hard, Nightmare and Inferno ones do not");
    RESET(); w32(0x08BBA0E4, 1); w32(0x08BBA468, 1); w32(0x08BB9EF4, 1); steps(61);
    CHECK(has(26) && !has(27), "difficulty lowered after retries: the boss counts, the Hard one does not");
    {
        static const unsigned boss[][2] = { { 0x08BBA020, 1 }, { 0x08BBA028, 2 }, { 0x08BBA060, 9 }, { 0x08BBA068, 12 }, { 0x08BBA07C, 19 },
            { 0x08BBA098, 20 }, { 0x08BBA09C, 21 }, { 0x08BBA0B0, 22 }, { 0x08BBA0BC, 23 }, { 0x08BBA0C8, 24 }, { 0x08BBA0CC, 25 } };
        int ok = 1;
        for (unsigned i = 0; i < sizeof boss / sizeof boss[0]; i++) {
            RESET(); w32(boss[i][0], 1); steps(61);
            if (!has((int)boss[i][1])) ok = 0;
            RESET(); w32(boss[i][0], 1); w32(0x08BB9EF0, 1); steps(61);
            if (!has((int)boss[i][1])) ok = 0;
        }
        CHECK(ok, "each boss flag gives its achievement, on Normal and on Easy");
        static const unsigned quest[][3] = { { 0x08BBA458, 1, 3 }, { 0x08BB9D84, 1, 7 }, { 0x08BB9E44, 1, 8 }, { 0x08BBA1B4, 1, 10 }, { 0x08BBA0F4, 1, 11 },
            { 0x08BBA168, 4, 14 }, { 0x08BBA1A4, 1, 15 }, { 0x08BB9E40, 1, 16 } };
        ok = 1;
        for (unsigned i = 0; i < sizeof quest / sizeof quest[0]; i++) { RESET(); w32(quest[i][0], quest[i][1]); steps(61); if (!has((int)quest[i][2])) ok = 0; }
        RESET(); w32(0x08BBA168, 3); steps(61); if (has(14)) ok = 0;
        RESET(); w32(0x08BBA0F4, 2); steps(61); if (has(11)) ok = 0;
        RESET(); w32(0x08BBA168, 0xFFFFFFFFu); w32(0x08BBA0F4, 0xFFFFFFFFu); steps(61); if (has(14) || has(11)) ok = 0;
        CHECK(ok, "side quests (and not half-done ones, nor -1 ones)");
    }
    RESET(); w32(0x08BB9D90, 1); steps(70); w32(0x08BBA07C, 1); steps(1);
    { int got17 = has(17); steps(1);
      CHECK(got17 && has(18) && has(19), "Gildias falls with the Spirit Cape held: 17; no Stone Shoes: 18"); }
    RESET(); w32(0x08BB9D90, 1); w32(0x08BBA07C, 1); w32(0x08BB9D8C, 1); steps(100);
    CHECK(!has(17) && !has(18) && has(19), "a save loaded after Gildias does not give 17; with the shoes, no 18");
    {
        int ok = 1;
        const u32 *areas[5] = { FG_CHESTS_I, FG_CHESTS_II, FG_CHESTS_III, FG_CHESTS_IV, FG_CHESTS_V };
        const int area_n[5] = { 11, 14, 11, 11, 9 };
        for (int a = 0; a < 5; a++) {
            RESET(); for (int i = 0; i < area_n[a]; i++) w32(areas[a][i], 1); steps(61);
            for (int b = 0; b < 5; b++) if (has(39 + b) != (a == b)) ok = 0;
            RESET(); for (int i = 1; i < area_n[a]; i++) w32(areas[a][i], 1); steps(61);
            if (has(39 + a)) ok = 0;
        }
        CHECK(ok, "Treasure Hunter I-V: every chest of the area, and only that area; one short: nothing");
    }
    RESET(); for (int i = 0; i < 25; i++) w32(FG_URNS[i], 1); steps(61);
    { int u25 = has(44) && has(45) && !has(46);
      for (int i = 25; i < 59; i++) w32(FG_URNS[i], 1); steps(1);
      CHECK(u25 && has(46), "urns: 10, 25, all 59"); }
    RESET(); for (int i = 0; i < 16; i++) w8(FG_CHARS[i], 1); steps(61);
    { int c16 = has(47) && !has(48);
      for (int i = 16; i < 32; i++) w8(FG_CHARS[i], 1); steps(1);
      CHECK(c16 && has(48), "characters: half of 32, then all"); }
    RESET(); for (int i = 0; i < 32; i++) w8(FG_CHARS[i], 0xFF); steps(61);
    CHECK(!has(47), "character bytes of 0xFF are 'not met'");
    RESET(); for (int i = 0; i < 64; i++) w8(FG_MONSTERS[i], 1); steps(61);
    { int b64 = has(49) && has(50) && has(51) && !has(52);
      for (int i = 64; i < 79; i++) w8(FG_MONSTERS[i], 1); steps(1);
      CHECK(b64 && has(52), "bestiary: 16, 32, 64, all 79"); }
    RESET(); w32(0x08BBA5DC, 80); steps(61);
    { int c1 = has(53);
      w32(0x08BB9E0C, 1); steps(1); int c1b = !has(53) && !has(54);
      w32(0x08BBA5DC, 130); steps(1); int c2 = has(54);
      w32(0x08BB9E10, 1); w32(0x08BBA5DC, 260); steps(1); int c3 = has(55) && !has(54);
      w32(0x08BB9D50, 1); w32(0x08BBA5DC, 500); steps(1);
      CHECK(c1 && c1b && c2 && c3 && has(56), "combos: 75 before Moonstar, 125 between, 250 after Sunset, 500 with the Brave Sword"); }
    RESET(); w32(0x08BB9B30, 30); steps(61);
    CHECK(has(57) && has(58) && !has(59), "level 30: 57 and 58");
    RESET(); for (int k = 0; k < 3; k++) w32(0x08BB9AB4 + 4 * (2 + 6 * k), 2); steps(61);
    { int t3 = has(62) && !has(60) && !has(68);
      for (int k = 0; k < 3; k++) w32(0x08BB9AB4 + 4 * (2 + 6 * k), 8); steps(1);
      int t9 = has(62) && has(68);
      w32(0x08BB9AB4 + 4 * 8, 7); steps(1);
      CHECK(t3 && t9 && !has(68), "upgrades: all three Tier-3 pieces at level 3, then 9; one short: not 9"); }
    RESET(); for (int k = 0; k < 18; k++) w32(0x08BB9AB4 + 4 * k, 0xFFFFFFFFu); w32(0x08BB9AB4, 2); steps(61);
    CHECK(g_n == 0, "upgrade pieces at -1 count as level 0");
    RESET(); w32(0x08BBA11C, 1); w32(0x08BBA120, 1); w32(0x08BBA140, 1); w32(0x08BBA45C, 1); steps(61);
    { int r1 = has(72);
      w32(0x08BB9DFC, 1); steps(1);
      CHECK(r1 && !has(72), "Redmont Riches: 42 ore from townspeople before the Mission Tablet; not after"); }
    RESET(); w32(0x08BB9B2C, 1500000); w32(0x08BB9E38, 2); steps(61);
    w32(0x08BB9B2C, 500001); w32(0x08BB9E38, 3); steps(1);
    CHECK(has(73) && g_n == 1, "Smart Water");
    RESET(); w32(0x08BB9B2C, 1999999); w32(0x08BB9E38, 0xFFFFFFFFu); steps(61);
    CHECK(steps(1) == 0, "-1 elixirs (none carried): the gate is open, nothing met");
    w32(0x08BB9B2C, 1000000); w32(0x08BB9E38, 1); steps(1);
    CHECK(has(73), "Smart Water from none carried (-1) to one elixir");
    RESET(); w32(0x08BB9B2C, 1500000); w32(0x08BB9E38, 2); steps(61); w32(0x08BB9B2C, 500001); steps(1); steps(200); w32(0x08BB9E38, 3); steps(1);
    CHECK(!has(73), "elixir more than 3 s after the price: not met");

    /* ---- Time Attack (a second in the menu first: Emblem waits a second after any start) ---- */
    RESET(); w16(0x08BC4CCC, 1); w32(0x08BB9B30, 0);
    w16(0x08BC4CCE, 0x28); steps(61);
    ta_fight(6, 2, 0, 0); CHECK(has(86), "Time Attack: Istersiva won on Normal without damage: 86");
    ta_fight(6, 2, 1, 0); CHECK(!has(86), "... with damage: no");
    ta_fight(6, 1, 0, 0); CHECK(!has(86), "... on Easy: no");
    ta_fight(6, 4, 0, 1); CHECK(!has(86), "... lost: no");
    ta_fight(15, -3, 0, 0); CHECK(has(94), "... no new record: the difficulty is read from the selection (Hard): 94");
    ta_fight(4, 2, 0, 0); CHECK(g_n == 0, "Guilen (no such achievement): nothing");
    w16(0x08BC4CD8, 0); w16(0x08BC4CCE, 0x28); steps(2);
    w16(0x08BC4CCE, 0x29); steps(40);
    w16(0x08BC4CCE, 0x2A); steps(2); w16(0x08BC4CCE, 0x29); steps(40);
    w16(0x08BBAB48, 1u << 8); w16(0x08BC4CCE, 0x2A); steps(1);
    CHECK(has(76), "Boss Rush on Normal, never fell: 76");
    w16(0x08BC4CCE, 0x28); steps(2); w16(0x08BC4CCE, 0x29); steps(40);
    w8(0x089FC932, 1); steps(2); w8(0x089FC932, 0);
    w32(0x08BBA6A0 + 0xC0 * 1, 9000); w16(0x08BC4CCE, 0x2A); steps(1);
    CHECK(!has(75), "Boss Rush on Easy after a fall: no");
    w16(0x08BC4CCE, 0x28); steps(2); w16(0x08BC4CCE, 0x29); steps(40);
    w8(0x089FC932, 1); steps(2); w8(0x089FC932, 0);
    w16(0x08BBAB48, (1u << 8) | (1u << 10)); w16(0x08BC4CCE, 0x2A); steps(1);
    CHECK(has(78), "Boss Rush on Nightmare: 78, falling or not");
    w16(0x08BC4CD8, 0x10); w16(0x08BC4CCE, 0x28); steps(2); w16(0x08BC4CCE, 0x29); steps(40);
    w32(0x08BBAB20, 5000); w16(0x08BC4CCE, 0x2A); steps(1);
    CHECK(has(80) && !has(81), "Berhardt beaten (a time recorded): 80");

    /* ---- side by side with Achieve 0.2 on random play ---- */
    {
        /* Every address a main-game rule reads, and values worth trying there. */
        static u32 addrs[256]; int na = 0;
        static const u32 single[] = { 0x08BBA020, 0x08BBA028, 0x08BBA060, 0x08BBA068, 0x08BBA07C, 0x08BBA098, 0x08BBA09C, 0x08BBA0B0,
            0x08BBA0BC, 0x08BBA0C8, 0x08BBA0CC, 0x08BBA0E4, 0x08BBA0F4, 0x08BBA1B4, 0x08BBA168, 0x08BBA1A4, 0x08BBA458, 0x08BB9D84,
            0x08BB9D8C, 0x08BB9D90, 0x08BB9D50, 0x08BB9DFC, 0x08BB9E0C, 0x08BB9E10, 0x08BB9E14, 0x08BB9E18, 0x08BB9E40, 0x08BB9E44,
            0x08BBA11C, 0x08BBA120, 0x08BBA140, 0x08BBA45C, 0x08BBA460, 0x08BB9EF0, 0x08BBA468, 0x08BB9FDC };
        for (unsigned i = 0; i < sizeof single / 4; i++) addrs[na++] = single[i];
        for (int k = 0; k < 18; k++) addrs[na++] = 0x08BB9AB4 + 4 * k;
        static const u32 vals[] = { 0, 1, 2, 3, 4, 8, 9, 0xFFFFFFFFu };
        srand(12345);
        int diffs = 0, ticks = 0, unlocks = 0;
        for (int run = 0; run < 300; run++) {
            memset(g_mem, 0, MEM_SIZE); reload(); main_game();
            FgState fs; memset(&fs, 0, sizeof fs);
            u8 seen_old[256] = { 0 }, seen_new[256] = { 0 };
            int elixir = -1;
            for (int t = 0; t < 400; t++, ticks++) {
                int r = rand() % 100;
                if (r < 10) w32(addrs[rand() % na], vals[rand() % 8]);
                else if (r < 13) w32(0x08BB9B30, rand() % 3 ? 1 + rand() % 70 : 0);          /* level, sometimes 0 (loading) */
                else if (r < 16) w32(0x08BB9EF4, rand() % 4 ? rand() % 3 : 7);               /* difficulty, sometimes nonsense */
                else if (r < 19) w32(0x08BBA5DC, rand() % 600);
                else if (r < 21) w32(0x08BB9B2C, (u32)(rand() % 2000000));
                else if (r < 23) { u32 gd = em_r32(0x08BB9B2C); if (gd >= 999999) w32(0x08BB9B2C, gd - 999999); }
                else if (r < 25) { elixir = elixir == -1 ? 1 : elixir + (rand() % 2 ? 1 : -1); if (elixir == 0) elixir = -1; w32(0x08BB9E38, (u32)elixir); }
                else if (r < 28) w8(FG_CHARS[rand() % 32], rand() % 4 ? 1 : 0xFF);
                else if (r < 31) w8(FG_MONSTERS[rand() % 79], rand() % 4 ? 1 : 0);
                else if (r < 34) w32(FG_URNS[rand() % 59], rand() % 4 ? 1 : 0xFFFFFFFFu);
                else if (r < 37) { const u32 *ch[5] = { FG_CHESTS_I, FG_CHESTS_II, FG_CHESTS_III, FG_CHESTS_IV, FG_CHESTS_V }; int a = rand() % 5, len[5] = { 11, 14, 11, 11, 9 };
                                   for (int i = 0; i < len[a]; i++) if (rand() % 3) w32(ch[a][i], 1); }
                int om[128], nm[128];
                int on = fg_step(&fs, g_ms, om, 128);
                int nn = em_step(&g_game, g_ms, seen_new, nm, 128);   /* as the plugin does: earned ones are skipped */
                g_ms += 16;
                u8 a[256] = { 0 }, b[256] = { 0 };
                for (int i = 0; i < on; i++) if (om[i] > 0 && om[i] < 128 && !seen_old[om[i]]) a[om[i]] = 1;
                for (int i = 0; i < nn; i++) if (nm[i] > 0 && nm[i] < 256) b[nm[i]] = 1;
                for (int i = 1; i < 256; i++) {
                    if (a[i] != b[i]) { if (diffs < 10) printf("  differs: run %d tick %d achievement %d: Achieve %d, Emblem %d\n", run, t, i, a[i], b[i]); diffs++; }
                    if (a[i]) { seen_old[i] = 1; unlocks++; }
                    if (b[i]) seen_new[i] = 1;
                }
            }
        }
        printf("  (%d ticks of random play, %d first unlocks)\n", ticks, unlocks);
        CHECK(diffs == 0 && unlocks > 500, "random main-game play: Emblem and Achieve 0.2 unlock the same achievements on the same ticks");
    }

    printf("\n%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
