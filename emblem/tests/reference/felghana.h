#ifndef AC_FELGHANA_H
#define AC_FELGHANA_H
#include <psptypes.h>

/* Ys: The Oath in Felghana (USA), ULUS-10558. Every address here is from
 * RetroAchievements' public code notes for the game (set 3132); RA address
 * + 0x08000000 = PSP address. */
#define FG_GAME_ID  "ULUS10558"
#define FG_TITLE    "Ys: The Oath in Felghana"
#define FG_TOTAL    95

typedef struct { int id; const char *title; int points; } FgDef;
extern const FgDef fg_defs[FG_TOTAL];           /* index = id - 1 */

/* Memory reads, supplied by the caller (the plugin reads the game; the PC
 * tests read a fake memory). Addresses are PSP addresses. */
u32 fg_r8(u32 a);
u32 fg_r16(u32 a);
u32 fg_r32(u32 a);

typedef struct {
    int  ok;                                    /* main game: values looked sane last tick */
    int  stable;                                /* ticks in a row they have */
    u32  gold, elixir;                          /* Smart Water */
    u32  t_drop, t_gain;
    u32  gildias;                               /* last value of the Gildias flag (Spiritual Advantage) */
    int  gildias_seen;
    /* Time Attack */
    int  ta_ok;
    u32  ta_state, ta_fight;
    int  ta_damaged, ta_fell, ta_hp, ta_hp2, ta_ticks;
    u32  ta_best[6];                            /* this fight's best times, per difficulty, at its start */
    u32  rush_best[6], rush_bits;
    int  rush_fell, rush_armed;
    u32  berhardt, pikkard;
} FgState;

/* One look at the game. Writes the ids whose condition holds (or whose
 * event happened) right now into out[], and returns how many. The caller
 * keeps track of which are already unlocked. now_ms must never be 0. */
int fg_step(FgState *s, u32 now_ms, int *out, int max);

/* 1 if the plugin has a condition for this id; 0 for the ones the code notes do not cover. */
int fg_included(int id);
#endif
