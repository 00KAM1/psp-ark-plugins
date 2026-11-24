#ifndef AC_COMMON_H
#define AC_COMMON_H
#include <psptypes.h>

#define AC_VERSION_STR  "1.1"                   /* the user sets the version */
#define AC_BUILD_STAMP  __DATE__ " " __TIME__
#define AC_DIR          "ms0:/EMBLEM"
#define AC_LOG_PATH     AC_DIR "/emblem.log"
#define AC_OLD_DIR      "ms0:/ACHIEVE"          /* Achieve 0.1/0.2: progress is moved over from here */

/* util.c */
int  ac_snprintf(char *buf, int size, const char *fmt, ...);
void ac_log(const char *fmt, ...);
int  ac_card_busy(void);                     /* Sidequest: 0 write, 1 wait, 2 do not write */
int  ac_card_begin(void);                    /* before a write: 0 = go ahead, then ac_card_end() */
void ac_card_end(void);
int  ac_card_wait(void);                     /* the same, waiting while a state is taken or loaded */

/* hook.c: what the display hook draws, set by the control thread */
typedef struct {
    char head[24];
    char title[48];
    char foot[40];
    int  h;                                     /* card height */
} AcToast;
extern volatile int g_ready;
extern volatile int g_view_open;                /* the viewer owns the screen: draw nothing */
extern AcToast *volatile g_toast;               /* the card on screen, or 0 */
extern volatile u32 g_toast_t0;                 /* when it started (system time, us) */
extern volatile u32 g_st_frames, g_st_intr, g_st_now, g_st_reject;
extern const char *g_hook_how;
int  hook_install(void);
void hook_remove(void);

#endif
