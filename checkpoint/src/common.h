/*
 * Checkpoint - automatic history of game saves for PSP on ARK-5.
 * Every time a game writes its save data, a dated copy is kept; a menu
 * puts any earlier copy back.
 */
#ifndef BT_COMMON_H
#define BT_COMMON_H
#include <psptypes.h>

#define BT_VERSION_STR  "1.0"
#define BT_BUILD_STAMP   __DATE__ " " __TIME__
#define BT_DIR          "ms0:/CHECKPOINT"
#define BT_LOG_PATH     BT_DIR "/checkpoint.log"
#define BT_CONFIG_PATH  BT_DIR "/config.txt"
#define BT_SAVEROOT     "ms0:/PSP/SAVEDATA"

#define BT_NAME_MAX     40      /* a save folder name: game name (12) + save name (19) */
#define BT_STAMP_LEN    15      /* YYYYMMDD_HHMMSS */
#define BT_MAX_VERSIONS 48
#define BT_MAX_PINS     10      /* pinned versions per save */
#define BT_MAX_FILES    32
#define BT_FILE_MAX     64

typedef struct {
    int  enabled;
    int  keep;          /* versions kept per save */
    int  min_minutes;   /* a newer copy replaces the newest one if that is younger than this */
    u32  hotkey;
    int  hold_ms;
    int  quit_screen;   /* 1 = the PS button opens Checkpoint's quit screen */
    int  warn_minutes;  /* a save older than this is shown in amber */
    int  journey;       /* 1 = keep the journey log */
    int  journey_detail;/* 0 = follow the title, or the detail when the title never changes; 1 = detail only */
} BtConfig;
extern BtConfig g_cfg;

typedef struct {
    char stamp[BT_STAMP_LEN + 1];
    u32  minutes;       /* comparable time, see vault_stamp_minutes */
    int  pinned;        /* never removed automatically */
} BtVersion;

/* util.c */
int  sq_snprintf(char *buf, int size, const char *fmt, ...);
void sq_strcpy(char *dst, int size, const char *src);
void sq_log(const char *fmt, ...);
void config_load(void);

/* vault.c: file work only, no kernel calls besides sceIo*, tested on a PC */
enum { BT_OK = 0, BT_SAME = 1, BT_NOTHING = 2, BT_E_SPACE = -2, BT_E_COPY = -3, BT_E_MISSING = -4, BT_E_TOOMANY = -5, BT_E_PINS = -6 };
u32  vault_stamp_minutes(const char *stamp);
int  vault_name_ok(const char *name);
int  vault_list_versions(const char *save, BtVersion *out, int max);         /* newest first */
int  vault_list_saves(const char *prefix, char names[][BT_NAME_MAX], int max);
int  vault_backup(const char *save, const char *stamp, int keep, int min_minutes, int force, u32 free_kb);
int  vault_restore(const char *save, const char *stamp);
int  vault_delete_version(const char *save, const char *stamp);
int  vault_set_pin(const char *save, const char *stamp, int on);
int  vault_sfo_string(const char *sfo_path, const char *key, char *out, int len);
int  vault_sfo_text(const char *sfo_path, const char *key, char *out, int len);
u32  vault_dir_kb(const char *dir);

/* journey.c: the diary. File work only, tested on a PC */
#define JR_TEXT_MAX  96                     /* bytes: Japanese text takes three per character */
#define JR_COLS_MAX  24
#define JR_LINE_MAX  (JR_COLS_MAX * 4 + 1)   /* bytes for one line of the book: up to four per character */
typedef struct {
    char stamp[BT_STAMP_LEN + 1];
    u32  secs;                  /* time played when the entry was made */
    char save[BT_NAME_MAX];
    char text[JR_TEXT_MAX];
} JrEntry;
void journey_clean(char *out, int size, const char *text);
void journey_ascii(char *out, int size, const char *text);
u32  journey_time_add(const char *game, u32 add);
void journey_key(char *out, int size, const char *text);
int  journey_pick(const char *game, const char *save, const char *title, const char *detail, int detail_only, char *out, int size);
int  journey_note(const char *game, const char *save, const char *stamp, const char *text, u32 secs);
int  journey_load(const char *game, JrEntry *out, int max, int *total, char *first);
int  journey_wrap(const char *text, int cols, char lines[2][JR_LINE_MAX]);
int  journey_paginate(const JrEntry *e, int n, int cols, int rows, u16 *first, int maxpages);
int  journey_weekday(int y, int m, int d);

/* hook.c */
int  hook_install(void);
void hook_poll(void);                       /* called by the UI thread: runs a due backup */
int  hook_backup_now(const char *save);     /* forced, from the menu */
int  hook_seen(char names[][BT_NAME_MAX], int max);
void now_stamp(char *out);
int  hook_last_save(char *name, char *stamp);
void hook_play_tick(void);                  /* called often by the UI thread: counts time played */
u32  hook_play_flush(void);                 /* stores the count; returns the game's total in seconds */   /* 1 if the game has saved since it started */
u32  free_kb(void);
extern volatile int g_copying;
extern int g_last_result;

#endif
