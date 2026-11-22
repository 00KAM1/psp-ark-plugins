/*
 * Checkpoint - notices when the game saves.
 *
 * Games save through the system's savedata utility: they call
 * sceUtilitySavedataInitStart with a parameter block, and
 * sceUtilitySavedataShutdownStart when it has finished. Both calls are
 * redirected through here. Nothing is changed in what the game or the
 * system sees; we only read which save folder was written and whether it
 * succeeded. The copy itself is made a few seconds later by the plugin's own
 * thread, so the game is never held up while it saves.
 */
#include <pspkernel.h>
#include <pspsdk.h>
#include <psputility.h>
#include <psprtc.h>
#include <systemctrl.h>
#include <string.h>
#include "common.h"

#define QUIET_US   (3 * 1000 * 1000)    /* wait this long after the last write before copying */
#define MAX_SEEN   8

volatile int g_copying;
int g_last_result;

static int (*orig_init)(SceUtilitySavedataParam *);
static int (*orig_shutdown)(void);

static SceUtilitySavedataParam *volatile g_params;
static volatile int  g_in_dialog;
static volatile u32  g_pending_at;           /* time of the last finished write */
static char          g_seen[MAX_SEEN][BT_NAME_MAX];  /* save folders written since the game started */
static volatile int  g_pend[MAX_SEEN];       /* 1 = that folder still needs copying */
static volatile int  g_lead[MAX_SEEN];       /* 1 = its last write was the main save, not a follow-up */
static int           g_nseen;
static char          g_game[14];             /* the name the game saves under, e.g. ULUS10558 */
static char          g_last_name[BT_NAME_MAX];   /* the save written most recently this session */
static char          g_last_stamp[BT_STAMP_LEN + 1];
static u32           g_last_at;
static volatile int  g_have_last;

const char *hook_game(void) { return g_game; }

int hook_busy(void) { return g_in_dialog || g_copying; }

static int user_ptr(const void *p, u32 size)
{
    u32 a = (u32)p;
    return a >= 0x08800000u && a + size <= 0x0C000000u && !(a & 3);
}

/* Modes that write a save: AUTOSAVE, SAVE, LISTSAVE, and the "make/write data" family. */
static int writes(int mode)
{
    return mode == 1 || mode == 3 || mode == 5 || mode == 13 || mode == 14 || mode == 17 || mode == 18;
}

/* A game often writes several folders in a row (system data, then the save
 * itself), so each one is queued separately. */
static void queue(const char *save)
{
    int i = 0;
    while (i < g_nseen && strcmp(g_seen[i], save)) i++;
    if (i == g_nseen) {
        if (g_nseen == MAX_SEEN) return;
        strcpy(g_seen[g_nseen++], save);
    }
    g_pend[i] = 1;
    g_pending_at = sceKernelGetSystemTimeLow();

    /* For the quit screen. Games often write a second, small "system data"
     * folder right after the real save; within ten seconds the first name is
     * kept and only the time moves on. */
    if (!g_have_last || g_pending_at - g_last_at > 10 * 1000 * 1000)
        strcpy(g_last_name, save);
    /* The journey log follows the same rule: only the main save gets a line,
     * so a game's "system data" does not show up in the diary. A game that
     * writes nothing else still gets its entry. */
    g_lead[i] = !strcmp(g_last_name, save);
    g_last_at = g_pending_at;
    now_stamp(g_last_stamp);
    g_have_last = 1;
}

int hook_last_save(char *name, char *stamp)
{
    if (!g_have_last) return 0;
    strcpy(name, g_last_name);
    strcpy(stamp, g_last_stamp);
    return 1;
}

int hook_seen(char names[][BT_NAME_MAX], int max)
{
    int n = g_nseen < max ? g_nseen : max;
    for (int i = 0; i < n; i++) strcpy(names[i], g_seen[i]);
    return n;
}

static int hook_init(SceUtilitySavedataParam *p)
{
    u32 k1 = pspSdkSetK1(0);
    for (int t = 0; g_copying && t < 600; t++) sceKernelDelayThread(50 * 1000);  /* never write while we read */
    g_params = user_ptr(p, sizeof(*p)) ? p : NULL;
    g_in_dialog = 1;
    pspSdkSetK1(k1);
    return orig_init(p);
}

static int hook_shutdown(void)
{
    u32 k1 = pspSdkSetK1(0);
    SceUtilitySavedataParam *p = g_params;
    if (p && g_cfg.enabled && p->base.result == 0 && writes((int)p->mode)) {
        char save[BT_NAME_MAX];
        char game[14], name[21];
        memcpy(game, p->gameName, 13); game[13] = 0;
        memcpy(name, p->saveName, 20); name[20] = 0;
        sq_snprintf(save, sizeof(save), "%s%s", game, name);
        if (vault_name_ok(save)) {
            queue(save);
            if (!g_game[0] && vault_name_ok(game)) strcpy(g_game, game);
        }
    }
    g_params = NULL;
    g_in_dialog = 0;
    pspSdkSetK1(k1);
    return orig_shutdown();
}

int hook_install(void)
{
    orig_init = (void *)sctrlHENFindFunction("sceUtility_Driver", "sceUtility", 0x50C4CD57);
    orig_shutdown = (void *)sctrlHENFindFunction("sceUtility_Driver", "sceUtility", 0x9790B33C);
    if (!orig_init || !orig_shutdown) return -1;
    sctrlHENPatchSyscall(orig_init, hook_init);
    sctrlHENPatchSyscall(orig_shutdown, hook_shutdown);
    return 0;
}

void now_stamp(char *out)
{
    ScePspDateTime t;
    memset(&t, 0, sizeof(t));
    sceRtcGetCurrentClockLocalTime(&t);
    sq_snprintf(out, BT_STAMP_LEN + 1, "%04d%02d%02d_%02d%02d%02d", t.year, t.month, t.day, t.hour, t.minute, t.second);
}

/* Free space on the Memory Stick in KB. */
u32 free_kb(void)
{
    u32 info[5] = { 0, 0, 0, 0, 0 };            /* max clusters, free clusters, max sectors, sector size, sectors per cluster */
    u32 *pinfo = info;
    if (sceIoDevctl("ms0:", 0x02425818, &pinfo, sizeof(pinfo), NULL, 0) < 0 || !info[3] || !info[4])
        return 0xFFFFFFFFu;
    u32 cluster = info[3] * info[4];
    return (u32)(((u64)info[1] * cluster) >> 10);
}

int hook_backup_now(const char *save)
{
    char stamp[BT_STAMP_LEN + 1];
    now_stamp(stamp);
    /* one more than usual may stay: the caller may be about to restore the oldest */
    return vault_backup(save, stamp, g_cfg.keep + 1, 0, 1, free_kb());
}

/* ---- journey log ---------------------------------------------------------- */
/* Time played is counted here in small steps. A step longer than two seconds
 * is not play: the menu was open, the PSP slept, or a save state was loaded. */
static u32 g_play_prev, g_play_us, g_play_secs;

void hook_play_tick(void)
{
    u32 now = sceKernelGetSystemTimeLow(), d = now - g_play_prev;
    g_play_prev = now;
    if (d >= 2 * 1000 * 1000) return;
    g_play_us += d;
    while (g_play_us >= 1000 * 1000) { g_play_us -= 1000 * 1000; g_play_secs++; }
}

u32 hook_play_flush(void)
{
    if (!g_cfg.journey || !g_game[0]) return 0;
    u32 total = journey_time_add(g_game, g_play_secs);
    g_play_secs = 0;
    return total;
}

/* The game has just saved into `save`: if its description is new, add a line. */
static void journal(const char *save, const char *stamp)
{
    char p[160];
    static char title[160], detail[160], text[JR_TEXT_MAX];
    if (!g_cfg.journey || !g_game[0]) return;
    u32 total = hook_play_flush();
    sq_snprintf(p, sizeof(p), BT_SAVEROOT "/%s/PARAM.SFO", save);
    /* both texts as the game wrote them; journey_pick decides which one is news */
    vault_sfo_text(p, "SAVEDATA_TITLE", title, sizeof(title));
    vault_sfo_text(p, "SAVEDATA_DETAIL", detail, sizeof(detail));
    if (!journey_pick(g_game, save, title, detail, g_cfg.journey_detail, text, sizeof(text))) return;
    int r = journey_note(g_game, save, stamp, text, total);
    if (r) sq_log("journey %s '%s': %d", save, text, r);
}

/* Copy every queued folder. Returns how many were handled. */
static int run_queue(void)
{
    char stamp[BT_STAMP_LEN + 1];
    int done = 0;
    g_copying = 1;                              /* from here a new save waits for us in hook_init */
    if (g_in_dialog) { g_copying = 0; return 0; }   /* one started in that instant: let it finish first */
    for (int i = 0; i < g_nseen; i++) {
        if (!g_pend[i]) continue;
        g_pend[i] = 0;
        now_stamp(stamp);
        g_last_result = vault_backup(g_seen[i], stamp, g_cfg.keep, g_cfg.min_minutes, 0, free_kb());
        sq_log("backup %s %s: %d", g_seen[i], stamp, g_last_result);
        if (g_last_result == BT_OK || g_last_result == BT_SAME) {
            if (g_lead[i]) journal(g_seen[i], stamp);
            else sq_log("journey: %s follows %s, not logged", g_seen[i], g_last_name);
        }
        done++;
    }
    g_copying = 0;
    return done;
}

static int any_pending(void)
{
    for (int i = 0; i < g_nseen; i++) if (g_pend[i]) return 1;
    return 0;
}

/* Called regularly by the plugin's thread while the menu is closed. */
void hook_poll(void)
{
    if (!any_pending() || g_in_dialog) return;
    if (sceKernelGetSystemTimeLow() - g_pending_at < QUIET_US) return;
    run_queue();
}

/* When the menu opens: copy anything still waiting, so the lists are current. */
void hook_flush(void)
{
    if (any_pending() && !g_in_dialog) run_queue();
}
