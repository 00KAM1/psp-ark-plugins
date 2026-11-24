/*
 * Emblem - local achievements for PSP games, shown and logged on the Memory
 * Stick only. Nothing goes online.
 *
 * Kernel plugin for the GAME context:  game, ms0:/SEPLUGINS/emblem.prx, on
 *
 *   ms0:/EMBLEM/games/<GAMEID>.txt    what to watch in a game (one file per game; see em.h)
 *   ms0:/EMBLEM/earned/<GAMEID>.txt   what has been earned
 *   ms0:/EMBLEM/emblem.log
 *
 *   L + R + LEFT (hold 0.5 s): the viewer
 *   Checkpoint's quit screen can open it too: bit 0 of the event flag "EmblemReq"
 */
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspinit.h>
#include <psprtc.h>
#include <string.h>
#include <systemctrl.h>
#include <systemctrl_se.h>
#include "common.h"
#include "ach.h"
#include "draw.h"
#include "em.h"
#include "view.h"

PSP_MODULE_INFO("Emblem", 0x1007, 1, 0);

#define KEYS     (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)
#define BTN_ALL  0x0000F3F9u
#define HOLD_US  (500 * 1000)
#define POLL_US  (16 * 1000)
#define QMAX     8

static char   g_game[16];
static char   g_game_path[64], g_file_name[40], g_earned_path[64];
static EmGame g_em;
static AcSet  g_have;
static char   g_text[32 * 1024];

/* ---- the queue of pop-ups ------------------------------------------------- */
static AcToast g_tbuf[2];
static int     g_tcur;
static AcToast g_queue[QMAX];
static int     g_qn, g_more;                    /* g_more: unlocks that did not fit in the queue */

static void queue_toast(const char *head, const char *title, const char *foot)
{
    if (g_qn == QMAX) return;
    AcToast *t = &g_queue[g_qn++];
    ac_snprintf(t->head, sizeof(t->head), "%s", head);
    ac_snprintf(t->title, sizeof(t->title), "%s", title);
    ac_snprintf(t->foot, sizeof(t->foot), "%s", foot);
    t->h = dr_card_height(t->title);
}

static int earned_count(void)                   /* earned and still in the game's file */
{
    int n = 0;
    for (int i = 1; i <= EM_MAX_ID; i++) n += g_have.has[i] && g_em.ach[i].included;
    return n;
}

/* Shows the next card when the one on screen has gone. */
static void toast_tick(void)
{
    u32 now = sceKernelGetSystemTimeLow();
    if (g_toast && now - g_toast_t0 < (u32)DR_SHOW_MS * 1000) return;
    if (!g_qn && g_more) {
        char title[48], foot[40];
        ac_snprintf(title, sizeof(title), "And %d more", g_more);
        ac_snprintf(foot, sizeof(foot), "%d of %d", earned_count(), g_em.total);
        g_more = 0;
        queue_toast("Achievements unlocked", title, foot);
    }
    if (!g_qn) { g_toast = 0; return; }
    g_tcur ^= 1;                                /* fill the buffer the hook is not reading */
    g_tbuf[g_tcur] = g_queue[0];
    memmove(g_queue, g_queue + 1, (size_t)(--g_qn) * sizeof(AcToast));
    g_toast_t0 = now;
    g_toast = &g_tbuf[g_tcur];
}

/* ---- reading the game: user RAM only ---------------------------------------- */
u32 em_r32(u32 a) { return (a < 0x08800000u || a > 0x09FFFFFCu || (a & 3)) ? 0 : *(volatile u32 *)a; }
u32 em_r16(u32 a) { return (a < 0x08800000u || a > 0x09FFFFFEu || (a & 1)) ? 0 : *(volatile u16 *)a; }
u32 em_r8(u32 a)  { return (a < 0x08800000u || a > 0x09FFFFFFu) ? 0 : *(volatile u8 *)a; }

static void now_stamp(char *out)                /* YYYYMMDD_HHMMSS */
{
    ScePspDateTime t;
    memset(&t, 0, sizeof(t));
    sceRtcGetCurrentClockLocalTime(&t);
    ac_snprintf(out, 16, "%04d%02d%02d_%02d%02d%02d", t.year, t.month, t.day, t.hour, t.minute, t.second);
}

static int g_quiet, g_quiet_n;                 /* catch-up: what a save just loaded has already earned */

static void unlock(int id)
{
    char stamp[16], foot[40];
    if (!em_included(&g_em, id) || g_have.has[id]) return;
    const EmAch *a = &g_em.ach[id];
    g_have.has[id] = 1;
    now_stamp(stamp);
    ac_snprintf(g_have.date[id], sizeof(g_have.date[id]), "%.4s-%.2s-%.2s %.2s:%.2s", stamp, stamp + 4, stamp + 6, stamp + 9, stamp + 11);
    int r = -9;                                 /* -9: Sidequest was at the stick, not written */
    if (ac_card_wait() == 0) {
        r = ac_append(g_earned_path, g_game, g_em.title[0] ? g_em.title : g_game, id, stamp, a->title, a->points);
        ac_card_end();
    }
    ac_log("%s %d '%s' (%d pts) at %s, written %d", g_quiet ? "ALREADY EARNED" : "UNLOCKED", id, a->title, a->points, stamp, r);
    if (g_quiet) { g_quiet_n++; return; }
    ac_snprintf(foot, sizeof(foot), "%d pts   %d of %d", a->points, earned_count(), g_em.total);
    if (g_qn >= QMAX - 1) { g_more++; return; }  /* the last place is kept for the "and N more" card */
    queue_toast("Achievement unlocked", a->title, foot);
}

/* ---- buttons -------------------------------------------------------------- */
static u32 read_buttons(void)
{
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    if (sceCtrlPeekBufferPositive(&pad, 1) < 0) return 0;
    return pad.Buttons & BTN_ALL;
}

/* L + R with exactly one other button, held half a second (Scanline's rule). */
static u32 combo(u32 now)
{
    static u32 held, since, fired;
    u32 other = now & ~KEYS;
    u32 t = sceKernelGetSystemTimeLow();
    if ((now & KEYS) != KEYS || !other || (other & (other - 1))) { held = 0; return 0; }
    if (other != held) { held = other; since = t; fired = 0; return 0; }
    if (fired || t - since < HOLD_US) return 0;
    fired = 1;
    return other;
}

/* ---- which game ------------------------------------------------------------ */
static int sfo_disc_id(const char *path, char *out, int size)
{
    static u8 b[4096];
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return -1;
    int n = sceIoRead(fd, b, sizeof(b));
    sceIoClose(fd);
    if (n < 20 || memcmp(b, "\0PSF", 4)) return -1;
    u32 keys, data, count;
    memcpy(&keys, b + 8, 4); memcpy(&data, b + 12, 4); memcpy(&count, b + 16, 4);
    if (count > 64 || keys >= (u32)n || data >= (u32)n) return -1;
    for (u32 i = 0; i < count; i++) {
        const u8 *e = b + 20 + i * 16;
        if (e + 16 > b + n) return -1;
        u16 koff; u32 doff;
        memcpy(&koff, e, 2); memcpy(&doff, e + 12, 4);
        if (keys + koff + 8 >= (u32)n || data + doff + 10 >= (u32)n) return -1;
        if (strcmp((const char *)b + keys + koff, "DISC_ID")) continue;
        int o = 0;
        for (const u8 *v = b + data + doff; *v && o < size - 1 && o < 12; v++) out[o++] = (char)*v;
        out[o] = 0;
        return 0;
    }
    return -1;
}

static void find_game(void)
{
    u16 type;
    u32 len = sizeof(g_game) - 1;
    memset(g_game, 0, sizeof(g_game));
    if (sctrlGetInitPARAM("DISC_ID", &type, &len, g_game) < 0) g_game[0] = 0;
    g_game[sizeof(g_game) - 1] = 0;
    if (strlen(g_game) != 9) sfo_disc_id("disc0:/PSP_GAME/PARAM.SFO", g_game, sizeof(g_game));
    for (int i = 0; g_game[i]; i++) if (g_game[i] == '-') memmove(g_game + i, g_game + i + 1, strlen(g_game + i));
    for (int i = 0; g_game[i]; i++) {               /* a plain name only: it becomes a file name */
        char c = g_game[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) { g_game[0] = 0; break; }
    }
}

static void file_error(int line, const char *msg, const char *text)
{
    ac_log("%s line %d: %s: %.60s", g_file_name, line, msg, text);
}

/* Reads games/<ID>.txt. Returns 1 if it has achievements to watch. */
static int load_game_file(void)
{
    ac_snprintf(g_file_name, sizeof(g_file_name), "games/%s.txt", g_game);
    ac_snprintf(g_game_path, sizeof(g_game_path), AC_DIR "/%s", g_file_name);
    SceUID fd = sceIoOpen(g_game_path, PSP_O_RDONLY, 0);
    if (fd < 0) { ac_log("no %s: nothing to watch in this game, staying idle", g_file_name); return 0; }
    int n = sceIoRead(fd, g_text, sizeof(g_text));
    sceIoClose(fd);
    if (n >= (int)sizeof(g_text)) { ac_log("%s is too big (over %d KB): staying idle", g_file_name, (int)sizeof(g_text) / 1024); return 0; }
    if (n <= 0) { ac_log("%s could not be read: staying idle", g_file_name); return 0; }
    g_text[n] = 0;
    int lines = em_load(&g_em, g_text, n, file_error);
    ac_log("%s: '%s', %d achievements with a rule (%d listed), %d lines skipped, %d of %d nodes",
           g_file_name, g_em.title, g_em.total, lines, g_em.errors, g_em.nnodes, EM_MAX_NODES);
    if (g_em.game[0] && strcmp(g_em.game, g_game)) ac_log("note: the file says it is for %s", g_em.game);
    return g_em.total > 0;
}

/* Progress kept by Achieve (0.1, 0.2) is moved over once. */
static void move_old_progress(void)
{
    char old[96];
    SceIoStat st;
    if (sceIoGetstat(g_earned_path, &st) >= 0) return;
    ac_snprintf(old, sizeof(old), AC_OLD_DIR "/%s/unlocked.txt", g_game);
    if (sceIoGetstat(old, &st) < 0) return;
    int r = -9;
    if (ac_card_wait() == 0) { r = ac_copy_file(old, g_earned_path); ac_card_end(); }
    ac_log("progress from %s copied to earned/%s.txt: %d", old, g_game, r);
}

/* ---- main loop ------------------------------------------------------------- */
static int main_thread(SceSize args, void *argp)
{
    sceKernelDelayThread(4 * 1000 * 1000);      /* let the game boot first (Checkpoint's rule) */
    find_game();
    ac_log("Emblem " AC_VERSION_STR " (built " AC_BUILD_STAMP ") start: game '%s', hook %s", g_game, g_hook_how);
    if (!g_game[0] || !load_game_file()) {
        hook_remove();
        return 0;
    }
    if (ac_card_wait() == 0) { sceIoMkdir(AC_DIR "/earned", 0777); ac_card_end(); }
    ac_snprintf(g_earned_path, sizeof(g_earned_path), AC_DIR "/earned/%s.txt", g_game);
    move_old_progress();
    int kept = ac_load(g_earned_path, &g_have);
    ac_log("%d earned so far (%d of %d in this file)", kept, earned_count(), g_em.total);
    view_setup();
    g_ready = 1;
    /* Only made once there is something to show, so its being there tells
     * Checkpoint's quit screen that this game has achievements to look at. */
    SceUID req = sceKernelCreateEventFlag("EmblemReq", 0, 0, NULL);

    static int met[EM_MAX_ID + 4];
    u32 start = sceKernelGetSystemTimeLow();
    for (;;) {
        sceKernelDelayThread(POLL_US);
        u32 now = sceKernelGetSystemTimeLow();
        u32 now_ms = (now - start) / 1000 + 1;

        int was = g_em.stable;
        int n = em_step(&g_em, now_ms, g_have.has, met, EM_MAX_ID + 4);
        /* The first look at a save just loaded: what it has already earned is
         * written down quietly and shown as one card, not one card each. */
        g_quiet = was < 60 && g_em.stable >= 60;
        g_quiet_n = 0;
        /* The file is what counts, not this copy of it: a Sidequest state brings back the
         * whole system as it was, this plugin's memory included. Read again before writing. */
        if (n) ac_load(g_earned_path, &g_have);
        for (int i = 0; i < n; i++) unlock(met[i]);
        if (g_quiet && g_quiet_n) {
            char title[48], foot[40];
            ac_snprintf(title, sizeof(title), "%d found in this save", g_quiet_n);
            ac_snprintf(foot, sizeof(foot), "%d of %d", earned_count(), g_em.total);
            queue_toast("Already earned", title, foot);
        }
        g_quiet = 0;

        u32 bits = 0;
        int asked = req >= 0 && sceKernelPollEventFlag(req, 1, PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &bits) >= 0;
        if (asked) sceKernelDelayThread(300 * 1000);    /* Checkpoint's quit screen (its R button): let the game paint over it first */
        if (asked || combo(read_buttons()) == PSP_CTRL_LEFT) {
            if (view_blocked()) ac_log("viewer: another plugin has the game frozen, not opening");
            else { ac_load(g_earned_path, &g_have); view_run(&g_em, &g_have, g_file_name, g_em.errors); }
        }
        toast_tick();
    }
    return 0;
}

int module_start(SceSize args, void *argp)
{
    SceIoStat st;
    if (ac_card_begin() == 0) {
        sceIoMkdir(AC_DIR, 0777);
        sceIoMkdir(AC_DIR "/games", 0777);
        if (sceIoGetstat(AC_LOG_PATH, &st) >= 0 && st.st_size > 16 * 1024) {
            sceIoRemove(AC_DIR "/emblem.old");
            if (sceIoRename(AC_LOG_PATH, AC_DIR "/emblem.old") < 0) sceIoRemove(AC_LOG_PATH);
        }
        ac_card_end();
    }
    /* PSP games only: not the XMB, not PS1 games, not ARK's launcher. */
    if (sceKernelInitKeyConfig() != PSP_INIT_KEYCONFIG_GAME) return 1;
    {
        const char *iso = sctrlSEGetUmdFile();
        const char *exe = sceKernelInitFileName();
        int (*is_launcher)(void) = (void *)sctrlHENFindFunction("SystemControl", "ArkCtrl", 0x65C8280F);
        int n = exe ? (int)strlen(exe) : 0;
        int has_iso = iso && iso[0];
        int is_eboot = n >= 9 && !strcmp(exe + n - 9, "EBOOT.PBP");
        int is_disc = n >= 6 && !strncmp(exe, "disc0:", 6);
        int launcher = (n && is_launcher && is_launcher()) ||
                       (n && (strstr(exe, "VBOOT.PBP") || strstr(exe, "/ARK_01234/")));
        if (launcher || !(has_iso || is_eboot || is_disc)) return 1;
    }
    hook_install();                             /* idle until the game is known */
    SceUID th = sceKernelCreateThread("EmblemMain", main_thread, 0x20, 0x4000, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    return 0;
}

int module_stop(SceSize args, void *argp)
{
    hook_remove();
    return 0;
}
