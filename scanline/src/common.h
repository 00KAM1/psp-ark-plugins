#ifndef SL_COMMON_H
#define SL_COMMON_H

#include <psptypes.h>
#include "lines.h"

#define SL_VERSION_STR  "1.2"
#define SL_BUILD_STAMP  __DATE__ " " __TIME__

#define SL_DIR          "ms0:/SCANLINE"
#define SL_CONFIG_PATH  SL_DIR "/config.txt"
#define SL_LOG_PATH     SL_DIR "/scanline.log"
#define SL_GAMES_PATH   SL_DIR "/games.txt"
#define SL_KEY_MAX      32

/* hook.c */
extern volatile int  g_ready, g_enabled, g_osd_frames;
extern volatile u32  g_gen;
extern char          g_osd_text[40];
extern const char *volatile g_osd_line2;
extern volatile u32  g_now_us, g_now_busy, g_now_wait, g_now_intr, g_now_valid;
/* What the GE figure of the last 64 frames stands on. */
#define GE_OK      0                            /* asked and answered: g_now_busy is a real percentage */
#define GE_UNASKED 1                            /* not asked in this game (frames drawn after an interrupt flip) */
#define GE_ERROR   2                            /* the status call returned an error every time */
#define GE_MISSING 3                            /* the status call was not found */
extern volatile u32  g_now_ge, g_st_geasked, g_st_geerr;
extern volatile u32  g_st_nowbusy;
extern volatile u32  g_st_intr, g_st_now;
extern SlSettings    g_set;
extern int           g_fast, g_ge_wait;
extern volatile u32  g_st_flips, g_st_gebusy, g_st_gewait, g_st_gemax, g_st_gegiveup;
int  hook_ge_found(void);
int  hook_intr_found(void);
int  hook_hcount_found(void);
extern volatile u32 g_st_mid;
extern int g_defer;
extern int g_mode2;                             /* 1: lines before the swap (mode 2) */
extern volatile u32  g_st_frames, g_st_us, g_st_max, g_st_rows, g_st_skipped, g_st_segs, g_st_reject;
extern volatile u32  g_last_addr, g_last_fmt, g_last_stride;
extern const char   *g_hook_how;
int  hook_install(void);
void hook_remove(void);
u32  hook_target(void);
u32  hook_word(int i);

/* util.c */
typedef struct {
    int enabled;
    int strength;       /* percent, snapped to a step */
    int spacing;
    int mode;           /* 1: lines after the swap, 2: lines before it */
    int hotkeys;
    int fast;
    int ge_wait;
    int defer;
} SlConfig;

void sl_log(const char *fmt, ...);
int  sl_card_busy(void);                     /* Sidequest: 0 write, 1 wait, 2 do not write */
int  sl_card_begin(void);                    /* before a write: 0 = go ahead, then sl_card_end() */
void sl_card_end(void);
int  sl_card_wait(void);                     /* the same, waiting while a state is taken or loaded */
int  sl_snprintf(char *buf, int size, const char *fmt, ...);
int  config_load(SlConfig *c);      /* 1 = read from the file, 0 = defaults */
int  config_save(const SlConfig *c);
void config_defaults(SlConfig *c);

/* Per-game settings: on/off, strength, spacing, mode. */
void game_key(char *out, const char *path);          /* out: SL_KEY_MAX bytes */
int  games_get(const char *key, SlConfig *c);        /* 1 = this game has an entry; c is updated */
int  games_set(const char *key, const SlConfig *c);  /* 0 = written */

#endif
