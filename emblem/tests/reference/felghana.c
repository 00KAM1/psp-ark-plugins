/*
 * Achieve - the conditions for Ys: The Oath in Felghana.
 *
 * Titles and points are RetroAchievements' list; addresses are from
 * RetroAchievements' public code notes for the game. The conditions are
 * written here from those notes and the achievements' descriptions. Where
 * the notes have no address for something, the achievement is left out
 * (fg_included() says which).
 *
 * No PSP calls: the same file is built into the plugin and the PC tests.
 */
#include "felghana.h"
#include "felghana_data.inc"

/* ---- addresses (code notes) ------------------------------------------------ */
#define A_MAXHP      0x08BB9AFCu   /* float */
#define A_HP         0x08BB9B00u   /* float */
#define A_GOLD       0x08BB9B2Cu
#define A_LEVEL      0x08BB9B30u
#define A_EASY       0x08BB9EF0u   /* 1 on Very Easy or Easy */
#define A_DIFF       0x08BB9EF4u   /* 0 Normal, 1 Hard, 2 Nightmare (Inferno: + A_INFERNO) */
#define A_INFERNO    0x08BB9FDCu
#define A_LOWERED    0x08BBA468u   /* difficulty lowered after retries */
#define A_COMBO      0x08BBA5DCu
#define A_MODE       0x08BC4CCCu   /* 16-bit: 0 main game, 1 Time Attack, 2 Museum */
#define A_TA_STATE   0x08BC4CCEu   /* 16-bit, in Time Attack: 0x28 menu, 0x29 in fight, 0x2a after fight */
#define A_TA_FIGHT   0x08BC4CD8u   /* 16-bit: 0 Boss Rush, 1 Dularn1 ... 15 Galbalan */
#define A_TA_DIFF    0x08BC4E36u   /* difficulty select: 0 Very Easy ... 4 Nightmare */
#define A_TA_LOST    0x089FC932u   /* 1: Adol cannot move, when you lose in Time Attack */
#define A_TA_HP      0x089F8D48u   /* float: Adol's current HP (actual) */

#define A_NIGHTFIRE  0x08BB9D84u
#define A_STONESHOES 0x08BB9D8Cu
#define A_SPIRITCAPE 0x08BB9D90u
#define A_BRAVESWORD 0x08BB9D50u
#define A_TABLET     0x08BB9DFCu
#define A_MOONSTAR   0x08BB9E0Cu
#define A_SUNSET     0x08BB9E10u
#define A_DARKNESS   0x08BB9E14u
#define A_LIGHT      0x08BB9E18u
#define A_ELIXIR     0x08BB9E38u
#define A_TALISMAN   0x08BB9E40u
#define A_AUGITE     0x08BB9E44u
#define A_UPGRADES   0x08BB9AB4u   /* 18 words: swords, shields, armour, tier 1 to 6 each; 0-based level */

#define A_DULARN1    0x08BBA020u   /* "Fight Dularn" */
#define A_ELLEFALE   0x08BBA028u   /* "Win Ellefale fight" */
#define A_ISTERSIVA  0x08BBA060u
#define A_SUCCUBI    0x08BBA068u   /* "After Succubi fight" */
#define A_GILDIAS    0x08BBA07Cu
#define A_FALEON     0x08BBA098u
#define A_SCHULTIGER 0x08BBA09Cu
#define A_ZIRDUROS   0x08BBA0B0u
#define A_CHESTER2   0x08BBA0BCu
#define A_DULARN2    0x08BBA0C8u
#define A_GARLAND    0x08BBA0CCu
#define A_GALBALAN   0x08BBA0E4u
#define A_ELENA      0x08BBA0F4u   /* bit 0: Elena back in town */
#define A_ANYA       0x08BBA1B4u   /* "Show Anya the 3 Raval Ore from well" */
#define A_ANTONIO    0x08BBA168u   /* 4: 10000 gold and the Jade Ring from Antonio */
#define A_CYNTHIA    0x08BBA1A4u   /* "Show Cynthia the Jade Ring" */
#define A_FIONA      0x08BBA458u   /* "Talk to Fiona ... (get amulet)" */

#define A_RUSH_BITS  0x08BBAB48u   /* 16 bits: 6 Very Easy, 7 Easy, 8 Normal, 9 Hard, 10 Nightmare, 11 Inferno */
#define A_BEST       0x08BBA6A0u   /* best times: + 0xC0 per difficulty (Very Easy .. Inferno), + 0xC per fight (0 = Boss Rush) */
#define A_BERHARDT   0x08BBAB20u
#define A_PIKKARD    0x08BBAB2Cu

static int fint(u32 bits)                       /* float to whole number, no FPU */
{
    int e = (int)((bits >> 23) & 0xFF) - 127;
    u32 m = (bits & 0x7FFFFFu) | 0x800000u;
    int v;
    if (e < 0) return 0;
    if (e > 30) v = 0x7FFFFFFF;
    else v = (int)(e >= 23 ? m << (e - 23) : m >> (23 - e));
    return (bits & 0x80000000u) ? -v : v;
}

/* The game keeps "not had / not done" as -1 (0xFFFFFFFF) as well as 0: on the
 * PSP-3000 the Katol Elixir slot read -1 with none carried (0.1's address
 * check, 2026-11-22). So -1 never counts as set, and counts are signed. */
static int isset(u32 v) { return v != 0 && v != 0xFFFFFFFFu; }
static int count_set(const u32 *a, int n) { int c = 0; for (int i = 0; i < n; i++) c += isset(fg_r32(a[i])); return c; }
static int count_bit0(const u32 *a, int n) { int c = 0; for (int i = 0; i < n; i++) { u32 b = fg_r8(a[i]); c += b != 0xFFu && (b & 1); } return c; }
#define ALL(a)   (count_set(a, (int)(sizeof(a) / sizeof(a[0]))) == (int)(sizeof(a) / sizeof(a[0])))
#define N(a)     ((int)(sizeof(a) / sizeof(a[0])))

static u32 best(int diff, int fight) { return fg_r32(A_BEST + 0xC0u * (u32)diff + 0xCu * (u32)fight); }

/* All three pieces of a tier at least at this 0-based level (shown in the game as one more). */
static int tier_at(int tier, u32 lv)
{
    return (int)fg_r32(A_UPGRADES + 4u * (u32)(tier - 1)) >= (int)lv &&
           (int)fg_r32(A_UPGRADES + 4u * (u32)(tier + 5)) >= (int)lv &&
           (int)fg_r32(A_UPGRADES + 4u * (u32)(tier + 11)) >= (int)lv;
}

static const int g_left_out[] = { 4, 5, 6, 13, 38, 95 };
int fg_included(int id)
{
    if (id < 1 || id > FG_TOTAL) return 0;
    for (int i = 0; i < N(g_left_out); i++) if (g_left_out[i] == id) return 0;
    return 1;
}

/* The fight number in Time Attack, and the achievement for winning it without damage. */
static const signed char g_ta_ach[16] = { 0, 82, 83, 84, 0, 85, 86, 87, 0, 88, 89, 90, 91, 92, 93, 94 };

#define PUT(id) do { if (n < max) out[n++] = (id); } while (0)
#define PAIR_MS 3000

int fg_step(FgState *s, u32 now_ms, int *out, int max)
{
    int n = 0;
    u32 mode = fg_r16(A_MODE);

    /* ---- main game ----------------------------------------------------------- */
    u32 level = fg_r32(A_LEVEL), gold = fg_r32(A_GOLD), diff = fg_r32(A_DIFF), elixir = fg_r32(A_ELIXIR);
    if (elixir == 0xFFFFFFFFu) elixir = 0;      /* none carried */
    int maxhp = fint(fg_r32(A_MAXHP));
    int sane = mode == 0 && level >= 1 && level <= 99 && maxhp >= 1 && maxhp <= 9999 && diff <= 2 &&
               gold <= 9999999u && elixir <= 99u;
    if (!sane) { s->ok = 0; s->stable = 0; s->gildias_seen = 0; }
    else if (s->stable < 60) s->stable++;       /* a second of sane values before anything counts */
    if (sane && s->stable >= 60) {
        int easy = isset(fg_r32(A_EASY)), lowered = isset(fg_r32(A_LOWERED)), inf = isset(fg_r32(A_INFERNO));
        int strict = !easy && !lowered;         /* the difficulty picked at the start, never lowered */
        int normal = 1;                         /* bosses, bestiary and combos count on any difficulty */
        int hard = strict && diff >= 1;
        int nightmare = strict && diff == 2 && !inf, inferno = strict && diff == 2 && inf;
#define F(a) isset(fg_r32(a))
        if (normal && F(A_DULARN1))   PUT(1);
        if (normal && F(A_ELLEFALE))  PUT(2);
        if (F(A_FIONA))               PUT(3);
        if (F(A_NIGHTFIRE))           PUT(7);
        if (normal && F(A_AUGITE))    PUT(8);
        if (normal && F(A_ISTERSIVA)) PUT(9);
        if (F(A_ANYA))                PUT(10);
        if (normal && isset(fg_r32(A_ELENA)) && (fg_r32(A_ELENA) & 1)) PUT(11);
        if (normal && F(A_SUCCUBI))   PUT(12);
        if ((int)fg_r32(A_ANTONIO) >= 4) PUT(14);
        if (F(A_CYNTHIA))             PUT(15);
        if (F(A_TALISMAN))            PUT(16);
        /* Spiritual Advantage: the Spirit Cape is held at the moment Gildias falls. */
        u32 gil = (u32)isset(fg_r32(A_GILDIAS));
        if (s->gildias_seen && !s->gildias && gil && normal && F(A_SPIRITCAPE)) PUT(17);
        s->gildias = gil; s->gildias_seen = 1;
        if (normal && gil && !F(A_STONESHOES)) PUT(18);
        if (normal && gil)                PUT(19);
        if (normal && F(A_FALEON))        PUT(20);
        if (normal && F(A_SCHULTIGER))    PUT(21);
        if (normal && F(A_ZIRDUROS))      PUT(22);
        if (normal && F(A_CHESTER2))      PUT(23);
        if (normal && F(A_DULARN2))       PUT(24);
        if (normal && F(A_GARLAND))       PUT(25);
        if (normal && F(A_GALBALAN))      PUT(26);
        if (hard && F(A_GALBALAN))        PUT(27);
        if (nightmare && F(A_MOONSTAR))   PUT(28);
        if (nightmare && F(A_SUNSET))     PUT(29);
        if (nightmare && F(A_DARKNESS))   PUT(30);
        if (nightmare && F(A_LIGHT))      PUT(31);
        if (nightmare && F(A_GALBALAN))   PUT(32);
        if (inferno && F(A_MOONSTAR))     PUT(33);
        if (inferno && F(A_SUNSET))       PUT(34);
        if (inferno && F(A_DARKNESS))     PUT(35);
        if (inferno && F(A_LIGHT))        PUT(36);
        if (inferno && F(A_GALBALAN))     PUT(37);
        if (ALL(FG_CHESTS_I))   PUT(39);
        if (ALL(FG_CHESTS_II))  PUT(40);
        if (ALL(FG_CHESTS_III)) PUT(41);
        if (ALL(FG_CHESTS_IV))  PUT(42);
        if (ALL(FG_CHESTS_V))   PUT(43);
        int urns = count_set(FG_URNS, N(FG_URNS));
        if (urns >= 10) PUT(44);
        if (urns >= 25) PUT(45);
        if (urns >= N(FG_URNS)) PUT(46);
        int chars = count_bit0(FG_CHARS, N(FG_CHARS));
        if (chars * 2 >= N(FG_CHARS)) PUT(47);
        if (chars >= N(FG_CHARS))     PUT(48);
        int beasts = count_bit0(FG_MONSTERS, N(FG_MONSTERS));
        if (normal && beasts >= 16) PUT(49);
        if (normal && beasts >= 32) PUT(50);
        if (normal && beasts >= 64) PUT(51);
        if (normal && beasts >= N(FG_MONSTERS)) PUT(52);
        int combo = (int)fg_r32(A_COMBO);
        if (normal && combo >= 75  && !F(A_MOONSTAR)) PUT(53);
        if (normal && combo >= 125 && F(A_MOONSTAR) && !F(A_SUNSET)) PUT(54);
        if (normal && combo >= 250 && F(A_SUNSET) && !F(A_DARKNESS)) PUT(55);
        if (normal && combo >= 500 && F(A_BRAVESWORD)) PUT(56);
        if (level >= 10) PUT(57);
        if (level >= 30) PUT(58);
        if (level >= 60) PUT(59);
        for (int t = 1; t <= 6; t++) {
            if (tier_at(t, 2)) PUT(59 + t);     /* level 3 */
            if (tier_at(t, 8)) PUT(65 + t);     /* level 9 */
        }
        /* Redmont Riches: Raval Ore given by townspeople before the Mission Tablet. */
        int given = (F(0x08BBA11Cu) ? 7 : 0) + (F(0x08BBA120u) ? 3 : 0) + (F(0x08BBA140u) ? 20 : 0) +
                    (F(0x08BBA45Cu) ? 12 : 0) + (F(0x08BBA460u) ? 100 : 0);
        if (given >= 40 && !F(A_TABLET)) PUT(72);
#undef F
        /* Smart Water: gold drops by exactly 999,999 and a Katol Elixir is gained, within 3 s. */
        if (s->ok) {
            if (s->gold >= 999999u && gold == s->gold - 999999u) s->t_drop = now_ms;
            if (elixir == s->elixir + 1) s->t_gain = now_ms;
            if (s->t_drop && s->t_gain) {
                u32 d = s->t_drop > s->t_gain ? s->t_drop - s->t_gain : s->t_gain - s->t_drop;
                if (d <= PAIR_MS) { PUT(73); s->t_drop = s->t_gain = 0; }
            }
            if (s->t_drop && now_ms - s->t_drop > PAIR_MS) s->t_drop = 0;
            if (s->t_gain && now_ms - s->t_gain > PAIR_MS) s->t_gain = 0;
        }
        s->gold = gold; s->elixir = elixir; s->ok = 1;
    }

    /* ---- Time Attack ------------------------------------------------------------ */
    u32 st = fg_r16(A_TA_STATE), fight = fg_r16(A_TA_FIGHT);
    int ta = mode == 1 && (st == 0x28 || st == 0x29 || st == 0x2a) && fight <= 0x20;
    if (!ta) { s->ta_ok = 0; return n; }
    int hp = fint(fg_r32(A_TA_HP)), hp2 = fint(fg_r32(A_HP));
    if (s->ta_ok) {
        if (st == 0x29 && s->ta_state != 0x29) {               /* a fight begins */
            s->ta_fight = fight; s->ta_damaged = 0; s->ta_fell = 0; s->ta_ticks = 0;
            for (int d = 0; d < 6; d++) s->ta_best[d] = fight <= 15 ? best(d, (int)fight) : 0;
            s->berhardt = fg_r32(A_BERHARDT); s->pikkard = fg_r32(A_PIKKARD);
            if (fight == 0 && s->ta_state == 0x28) {           /* a Boss Rush begins */
                for (int d = 0; d < 6; d++) s->rush_best[d] = best(d, 0);
                s->rush_bits = fg_r16(A_RUSH_BITS); s->rush_fell = 0; s->rush_armed = 1;
            }
        }
        if (st == 0x29) {
            /* HP is watched from half a second in, once the fight has set it up. */
            if (s->ta_ticks < 30) { if (++s->ta_ticks == 30) { s->ta_hp = hp; s->ta_hp2 = hp2; } }
            else {
                if (hp < s->ta_hp || hp2 < s->ta_hp2) s->ta_damaged = 1;
                s->ta_hp = hp; s->ta_hp2 = hp2;
            }
            if (fg_r8(A_TA_LOST) == 1) { s->ta_fell = 1; s->rush_fell = 1; }
        }
        if (s->ta_state == 0x29 && st == 0x2a && !s->ta_fell) {     /* a fight is won */
            u32 f = s->ta_fight;
            if (f >= 1 && f <= 15 && g_ta_ach[f]) {
                int d = -1;
                for (int k = 0; k < 6; k++) if (best(k, (int)f) != s->ta_best[k]) d = k;   /* the record that moved says the difficulty */
                if (d < 0) d = (int)fg_r8(A_TA_DIFF);
                if (d >= 2 && !s->ta_damaged) PUT(g_ta_ach[f]);
            }
            u32 b = fg_r32(A_BERHARDT), p = fg_r32(A_PIKKARD);
            if (b && b != s->berhardt) PUT(80);
            if (p && p != s->pikkard) PUT(81);
        }
        if (s->rush_armed) {                                     /* a Boss Rush is finished when its record or its "beaten" bit moves */
            u32 bits = fg_r16(A_RUSH_BITS);
            for (int d = 0; d < 6; d++) {
                int done = ((bits & ~s->rush_bits) >> (6 + d)) & 1;
                u32 bt = best(d, 0);
                if (bt && bt != s->rush_best[d]) done = 1;
                if (!done) continue;
                if (d <= 3 && !s->rush_fell) PUT(74 + d);
                if (d == 4) PUT(78);
                if (d == 5) PUT(79);
                s->rush_armed = 0;
            }
            if (st == 0x28 && s->ta_state != 0x28) s->rush_armed = 0;   /* back to the menu: over */
        }
    }
    s->ta_state = st; s->ta_ok = 1;
    return n;
}
