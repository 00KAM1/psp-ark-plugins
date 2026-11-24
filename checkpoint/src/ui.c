/*
 * Checkpoint - the menu: pick a save, pick a version, put it back.
 */
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspthreadman_kernel.h>
#include <string.h>
#include <systemctrl.h>
#include "common.h"
#include "blit.h"
#include "jtext.h"

int hook_busy(void);
void hook_mem_report(void);
void hook_flush(void);
const char *hook_game(void);

#define BTN_ALL 0x0000F3F9u
#define BTN_HOME 0x00010000u                     /* the PS button, visible to kernel code only */
#define BX 24
#define BY 28
#define BW 432
#define BH 216
#define ROWS 10
#define MAX_SAVES 16

/* ---- freeze the game while the menu is open ----------------------------- */
#define MAX_FROZEN 128
static SceUID g_frozen[MAX_FROZEN];
static int    g_nfrozen;

static void freeze_game(void)
{
    SceUID ids[MAX_FROZEN];
    int count = 0;
    if (g_nfrozen) return;
    if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids, MAX_FROZEN, &count) < 0) return;
    if (count > MAX_FROZEN) count = MAX_FROZEN;
    SceUID self = sceKernelGetThreadId();
    for (int i = 0; i < count; i++) {
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (ids[i] == self || sceKernelReferThreadStatus(ids[i], &info) < 0) continue;
        if (!(info.attr & PSP_THREAD_ATTR_USER)) continue;
        if (sceKernelSuspendThread(ids[i]) >= 0) g_frozen[g_nfrozen++] = ids[i];
    }
}

static void thaw_game(void)
{
    for (int i = 0; i < g_nfrozen; i++) sceKernelResumeThread(g_frozen[i]);
    g_nfrozen = 0;
}

/* ---- the PS button ------------------------------------------------------- */
/* The system's own "quit the game?" screen can be switched off; games that
 * handle the PS button themselves use the same switch. With the quit screen
 * feature on, Checkpoint keeps it off and answers the button itself. */
static int (*g_pop_get)(void), (*g_pop_set)(int);
static int (*g_invoke_exit)(void);
static int g_quit_on;                       /* the quit screen feature is active */

static void home_setup(void)
{
    g_pop_get = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose", 0x0F341BE4);
    g_pop_set = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose", 0x5595A71A);
    if (!g_pop_get) g_pop_get = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose_driver", 0xE9A42056);
    if (!g_pop_set) g_pop_set = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose_driver", 0xC08C41EF);
    /* what the system's quit screen calls when "Yes" is chosen */
    g_invoke_exit = (void *)sctrlHENFindFunction("sceLoadExec", "LoadExecForKernel", 0x1F08547A);
    if (!g_invoke_exit) g_invoke_exit = (void *)sctrlHENFindFunction("sceLoadExec", "LoadExecForKernel", 0x62A27008);
    /* Only take the button over if we can also do the quitting. */
    g_quit_on = g_cfg.quit_screen && g_pop_get && g_pop_set && g_invoke_exit;
    sq_log("PS button: switch %s, exit call %s, quit screen %s", g_pop_get && g_pop_set ? "found" : "NOT found",
           g_invoke_exit ? "found" : "NOT found", g_quit_on ? "on" : "off");
}

/* While one of our screens is open the system's quit screen stays away. */
static void home_block(int on)
{
    static int saved = 1;
    if (!g_pop_get || !g_pop_set) return;
    if (g_quit_on) { g_pop_set(0); return; }            /* it is always ours */
    if (on) { saved = g_pop_get(); g_pop_set(0); }
    else    g_pop_set(saved);
}

/* ---- input -------------------------------------------------------------- */
static u32 read_buttons(void)
{
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    if (sceCtrlPeekBufferPositive(&pad, 1) < 0) return 0;
    return pad.Buttons & BTN_ALL;
}

static u32 read_raw(void)
{
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    if (sceCtrlPeekBufferPositive(&pad, 1) < 0) return 0;
    return pad.Buttons;
}

static void wait_release(void) { while (read_raw() & (BTN_ALL | BTN_HOME)) sceKernelDelayThread(16 * 1000); }

/* 1 if every user thread is already suspended: another plugin (Sidequest's
 * menu, for one) has the game frozen and owns the screen. */
static int game_frozen_by_other(void)
{
    SceUID ids[MAX_FROZEN];
    int count = 0, user = 0, running = 0;
    if (g_nfrozen) return 0;
    if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids, MAX_FROZEN, &count) < 0) return 0;
    if (count > MAX_FROZEN) count = MAX_FROZEN;
    for (int i = 0; i < count; i++) {
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelReferThreadStatus(ids[i], &info) < 0 || !(info.attr & PSP_THREAD_ATTR_USER)) continue;
        user++;
        if (!(info.status & PSP_THREAD_SUSPEND)) running++;
    }
    return user > 0 && running == 0;
}

/* ---- the POWER switch while the game is frozen ---------------------------- */
/* A PSP cannot go to sleep around a frozen game: on the PSP-3000 the power
 * light blinks and it never wakes. So while one of our screens has the game
 * frozen we hold a power lock, which makes the system wait with a slide of
 * the switch instead of acting on it. We watch for that waiting request; when
 * it appears the screen closes itself, the game is let go, the lock is
 * released, and the PSP then sleeps (or turns off) the normal way. */
static int (*g_pw_lock)(int), (*g_pw_unlock)(int), (*g_pw_request)(void);
static int g_pw_held, g_slide;

static void power_lookup(void)
{
    static const char *M = "scePower_Service";
    g_pw_lock   = (void *)sctrlHENFindFunction(M, "scePower_driver", 0x6CF50928);   /* kernel form: never waits */
    g_pw_unlock = (void *)sctrlHENFindFunction(M, "scePower_driver", 0xC3024FE6);
    if (!g_pw_lock || !g_pw_unlock) {                                               /* a matching pair, or none */
        g_pw_lock   = (void *)sctrlHENFindFunction(M, "scePower", 0xD6D016EF);
        g_pw_unlock = (void *)sctrlHENFindFunction(M, "scePower", 0xCA3D34C1);
    }
    g_pw_request = (void *)sctrlHENFindFunction(M, "scePower", 0x7FA406DD);
    if (!g_pw_request) g_pw_request = (void *)sctrlHENFindFunction(M, "scePower_driver", 0x566B8353);
    if (!g_pw_lock || !g_pw_unlock || !g_pw_request) g_pw_lock = g_pw_unlock = NULL;   /* all or nothing */
    sq_log("POWER switch guard %s", g_pw_lock ? "on" : "NOT available");
}

static void power_hold(void)
{
    if (!g_pw_lock || g_pw_held) return;
    g_pw_lock(0);
    g_pw_held = 1;
}

static void power_release(void)
{
    if (!g_pw_held) return;
    g_pw_held = 0;
    g_pw_unlock(0);
}

/* 1 once the switch has been slid while we hold the lock. */
static int power_slid(void)
{
    if (!g_slide && g_pw_held && g_pw_request()) {
        g_slide = 1;
        sq_log("POWER slid while a screen is open: closing it so the PSP can sleep");
    }
    return g_slide;
}

/* The screen has closed and the game runs again: give the lock back. After a
 * slide, let the system act on it and come back when it is over. */
static void power_done(void)
{
    power_release();
    if (!g_slide) return;
    for (int t = 0; t < 600 && g_pw_request(); t++) sceKernelDelayThread(50 * 1000);
    sceKernelDelayThread(500 * 1000);
    g_slide = 0;
    sq_log("back after the POWER slide");
}

/*
 * The pad, hidden from the game. While one of our screens is open the game is
 * frozen, but the system goes on recording the pad for it, and the game reads
 * that record when it runs again: the X or TRIANGLE pressed on our screen then
 * opens a menu in the game, and that menu ends up in the picture of a state.
 * The controller driver can report every button as "not pressed" to user
 * programs while kernel code (these plugins) still reads the real pad. It is
 * switched on when a screen opens and off when the game is handed back, after
 * every button has been let go. It is one switch for the whole system and it is
 * kept in the driver's memory, so a loaded state wakes up with it on: every way
 * out of a screen must switch it off.
 * on: 1 hide, 0 show, -1 show and do not hide again (see wait_press).
 */
#define PAD_GAME_BUTTONS 0x0000FFFFu
/* 0 leaves the switch unused. It was off for three test builds on Nov 24 2026 while the
 * system's save screen was failing; that turned out to be kernel memory, not this. */
#define PAD_HIDE_ENABLED 1
static int g_pad_hidden, g_pad_off;
static void pad_hide(int on)
{
    static u32 (*intercept)(u32 buttons, u32 mode);
    static int looked;
    if (!PAD_HIDE_ENABLED) return;
    if (!looked) {
        looked = 1;
        intercept = (void *)sctrlHENFindFunction("sceController_Service", "sceCtrl_driver", 0x7CA723DC);
        sq_log("ui: hiding the pad from the game: %s", intercept ? "found" : "NOT found");
    }
    if (!intercept) return;
    if (on < 0) g_pad_off = 1;
    if (on > 0 && g_pad_off) return;
    if (on == 0) for (int t = 0; t < 100 && read_buttons(); t++) sceKernelDelayThread(20 * 1000);   /* up to 2 s for the buttons to be let go */
    /* The sixteen ordinary buttons. The PS button and the other system ones are above them
     * and are left alone. */
    intercept(PAD_GAME_BUTTONS, on > 0 ? 1 : 0);
    g_pad_hidden = on > 0;
}

static u32 wait_press(void)
{
    u32 prev = read_buttons();
    int idle = 0;
    for (;;) {
        if (power_slid()) return PSP_CTRL_CIRCLE;       /* "close": every screen backs out */
        sceKernelDelayThread(16 * 1000);
        u32 now = read_buttons();
        u32 edge = now & ~prev;
        prev = now;
        if (edge) return edge;
        /* Half a minute on a screen without one button seen, with the pad hidden: in case
         * the hiding reaches us too on some system, it is switched off for good, so a
         * screen can never be left without a way out. */
        if (g_pad_hidden && ++idle == 1800) { pad_hide(-1); sq_log("ui: no button for 30 s with the pad hidden: hiding switched off"); }
    }
}

/* ---- state -------------------------------------------------------------- */
static char      g_disc[16];
static char      g_saves[MAX_SAVES][BT_NAME_MAX];
static char      g_save_title[MAX_SAVES][24 * 3 + 4];    /* 24 characters of UTF-8 */
static int       g_save_count[MAX_SAVES];
static int       g_save_pins[MAX_SAVES];
static int       g_nsaves, g_sel_save;
static BtVersion g_ver[BT_MAX_VERSIONS];
static char      g_ver_detail[BT_MAX_VERSIONS][33 * 3 + 5];
static int       g_nver, g_sel_ver;
static char      g_status[64];
static u32       g_status_color = C_GREY;

static void set_status(u32 color, const char *msg)
{
    sq_strcpy(g_status, sizeof(g_status), msg);
    g_status_color = color;
}

/* A text value from a save's PARAM.SFO as the game wrote it (UTF-8), tidied
 * and cut between whole characters to fit. */
static int sfo_text(const char *path, const char *key, char *out, int size)
{
    static char raw[200];
    int r = vault_sfo_text(path, key, raw, sizeof(raw));
    journey_clean(out, size, raw);
    return r;
}

static void load_versions(void)
{
    char p[160];
    g_nver = vault_list_versions(g_saves[g_sel_save], g_ver, BT_MAX_VERSIONS);
    if (g_sel_ver >= g_nver) g_sel_ver = g_nver ? g_nver - 1 : 0;
    for (int i = 0; i < g_nver; i++) {
        sq_snprintf(p, sizeof(p), BT_DIR "/%s/%s/PARAM.SFO", g_saves[g_sel_save], g_ver[i].stamp);
        if (sfo_text(p, "SAVEDATA_DETAIL", g_ver_detail[i], sizeof(g_ver_detail[i])) < 0 || !g_ver_detail[i][0])
            sfo_text(p, "SAVEDATA_TITLE", g_ver_detail[i], sizeof(g_ver_detail[i]));
    }
}

/* Saves of this game: vault folders that start with the game's ID, plus any
 * save folder the game has written to since it started. */
static void load_saves(void)
{
    static char seen[8][BT_NAME_MAX];
    static BtVersion tmp[BT_MAX_VERSIONS];
    char p[160];
    g_nsaves = vault_list_saves(g_disc, g_saves, MAX_SAVES);
    int ns = hook_seen(seen, 8);
    for (int i = 0; i < ns && g_nsaves < MAX_SAVES; i++) {
        int have = 0;
        for (int j = 0; j < g_nsaves; j++) if (!strcmp(g_saves[j], seen[i])) have = 1;
        if (!have) strcpy(g_saves[g_nsaves++], seen[i]);
    }
    for (int i = 0; i < g_nsaves; i++) {
        g_save_count[i] = vault_list_versions(g_saves[i], tmp, BT_MAX_VERSIONS);
        g_save_pins[i] = 0;
        for (int v = 0; v < g_save_count[i]; v++) g_save_pins[i] += tmp[v].pinned;
        sq_snprintf(p, sizeof(p), BT_SAVEROOT "/%s/PARAM.SFO", g_saves[i]);
        if (sfo_text(p, "SAVEDATA_TITLE", g_save_title[i], sizeof(g_save_title[i])) < 0 && g_save_count[i] > 0) {
            sq_snprintf(p, sizeof(p), BT_DIR "/%s/%s/PARAM.SFO", g_saves[i], tmp[0].stamp);
            sfo_text(p, "SAVEDATA_TITLE", g_save_title[i], sizeof(g_save_title[i]));
        }
    }
    if (g_sel_save >= g_nsaves) g_sel_save = g_nsaves ? g_nsaves - 1 : 0;
}

/* ---- drawing ------------------------------------------------------------ */
static void frame(const char *right)
{
    blit_rect(BX, BY, BW, BH, C_BG);
    blit_rect(BX, BY, BW, 12, C_HI);
    blit_text_uc(BX + 4, BY + 2, "Checkpoint " BT_VERSION_STR, C_WHITE, C_HI);
    if (right) blit_text_uc(BX + BW - 4 - 8 * (int)strlen(right), BY + 2, right, C_WHITE, C_HI);
}

static void footer(const char *keys)
{
    char st[56];
    int fy = BY + BH - 34;
    blit_text_uc(BX + 4, fy, keys, C_WHITE, C_BG);
    if (g_cfg.journey) blit_text_uc(BX + 4, fy + 11, "SELECT journey log", C_GREY, C_BG);
    sq_snprintf(st, sizeof(st), "%.52s", g_status);
    blit_text_uc(BX + 4, fy + 22, st, g_status_color, C_BG);
}

static void row(int k, int selected, const char *text, u32 fg)
{
    int y = BY + 34 + k * 14;
    u32 bg = selected ? C_HI : C_BG;
    blit_rect(BX + 2, y - 2, BW - 4, 12, bg);
    blit_text_uc(BX + 6, y, text, fg, bg);
}

static void draw_saves(void)
{
    char line[200];
    if (blit_setup() < 0) return;
    frame(g_disc);
    blit_text_uc(BX + 4, BY + 18, "Saves of this game", C_GREY, C_BG);
    if (!g_nsaves) {
        blit_text_uc(BX + 8, BY + 60, "No backups yet for this game.", C_WHITE, C_BG);
        blit_text_uc(BX + 8, BY + 76, "A copy is kept each time the game saves.", C_GREY, C_BG);
        footer("O close");
        return;
    }
    int first = g_sel_save >= ROWS ? g_sel_save - ROWS + 1 : 0;
    for (int k = 0; k < ROWS && first + k < g_nsaves; k++) {
        int i = first + k;
        /* 24 + 1 + 2 + 1 + 24 = 52 characters */
        int at = sq_snprintf(line, sizeof(line), "%-24.24s %2d ", g_saves[i], g_save_count[i]);
        jt_fit(line + at, sizeof(line) - at, g_save_title[i], 24);
        row(k, i == g_sel_save, line, C_WHITE);
    }
    footer("X versions   [] back up now   O close");
}

static void draw_versions(void)
{
    char line[200], right[48];
    if (blit_setup() < 0) return;
    frame(g_disc);
    sq_snprintf(right, sizeof(right), "%.40s", g_saves[g_sel_save]);
    blit_text_uc(BX + 4, BY + 18, right, C_GREY, C_BG);
    if (!g_nver) {
        blit_text_uc(BX + 8, BY + 60, "No versions kept for this save yet.", C_WHITE, C_BG);
        footer("[] back up now   O back");
        return;
    }
    int first = g_sel_ver >= ROWS ? g_sel_ver - ROWS + 1 : 0;     /* the list scrolls past ten */
    for (int k = 0; k < ROWS && first + k < g_nver; k++) {
        int i = first + k;
        const char *s = g_ver[i].stamp;                 /* YYYYMMDD_HHMMSS */
        /* 2 + 1 + 3 + 1 + 11 + 1 + 33 = 52 characters */
        int at = sq_snprintf(line, sizeof(line), "%2d %s %.2s-%.2s %.2s:%.2s ", i + 1, g_ver[i].pinned ? "PIN" : "   ",
                             s + 4, s + 6, s + 9, s + 11);
        jt_fit(line + at, sizeof(line) - at, g_ver_detail[i], 33);
        row(k, i == g_sel_ver, line, g_ver[i].pinned ? C_OK : C_WHITE);
    }
    footer("X restore  /\\ delete  [] back up  START pin  O back");
}

static void message(const char *l1, const char *l2, u32 color)
{
    if (blit_setup() < 0) return;
    blit_rect(BX, BY + 70, BW, 60, C_BG);
    blit_text_uc(BX + 8, BY + 84, l1, color, C_BG);
    if (l2) blit_text_uc(BX + 8, BY + 100, l2, C_GREY, C_BG);
}

static int confirm(const char *question)
{
    message(question, "X: yes     any other button: no", C_WARN);
    wait_release();
    return wait_press() == PSP_CTRL_CROSS;
}

/* ---- journey log: the diary ------------------------------------------------ */
/* An open book over the frozen game: a title page, then the entries, newest
 * spread first. Drawn with rectangles and the 8x8 font only. */
#define D_PAPER 0x00C4E0EC
#define D_RULE  0x00A6C2D0
#define D_INK   0x001E2634
#define D_RED   0x00223096
#define D_FADE  0x00647C8C
#define D_COVER 0x0018284A
#define D_COVR2 0x00223C68
#define D_EDGE  0x00ACC8D6
#define D_SH1   0x00B5D1DE
#define D_SH2   0x009EBAC8
#define D_SPINE 0x00101A30
#define DX 12
#define DY 8
#define DW 456
#define DH 244
#define DPW 216                                 /* one page */
#define DPH 228
#define DLX (DX + 8)
#define DRX (DX + 8 + DPW + 8)
#define DPY (DY + 8)
#define DCOLS 24                                /* characters across a page */
#define DPITCH 11
#define DTOP 14
#define DROWS 18                                /* ruled lines down a page */
#define JR_MAX 120
#define JR_PAGES 64

static JrEntry g_jr[JR_MAX];
static u16     g_jr_first[JR_PAGES];
static int     g_jr_n, g_jr_total, g_jr_pages;
static char    g_jr_begun[BT_STAMP_LEN + 1];
static u32     g_jr_secs;
static char    g_title[100];                     /* the game's name, for the title page */

static const char *const MONTHS[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *const DAYS[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static int two(const char *s) { return (s[0] - '0') * 10 + (s[1] - '0'); }
static const char *month_of(const char *stamp) { int m = two(stamp + 4); return MONTHS[m >= 1 && m <= 12 ? m - 1 : 0]; }

static void d_line(int right, int k, const char *s, u32 fg, int centre)
{
    char buf[JR_LINE_MAX];
    int cols = jt_fit(buf, sizeof(buf), s, DCOLS);
    int x = (right ? DRX : DLX) + 12;
    if (centre) x += (DCOLS * 8 - cols * 8) / 2;
    blit_text_uc(x, DPY + DTOP + k * DPITCH, buf, fg, D_PAPER);
}

static void d_book(void)
{
    blit_rect(DX, DY, DW, DH, D_COVER);
    blit_rect(DX + 2, DY + 2, DW - 4, DH - 4, D_COVR2);
    blit_rect(DX + 4, DY + 4, DW - 8, DH - 8, D_COVER);
    for (int s = 0; s < 2; s++) {
        int x = s ? DRX : DLX;
        blit_rect(x - 2, DPY + 2, DPW + 4, DPH, D_EDGE);        /* the pages underneath */
        blit_rect(x, DPY, DPW, DPH, D_PAPER);
        for (int k = 0; k < DROWS; k++) blit_rect(x + 8, DPY + DTOP + k * DPITCH + 9, DPW - 16, 1, D_RULE);
    }
    blit_rect(DLX + DPW - 6, DPY, 3, DPH, D_SH1); blit_rect(DLX + DPW - 3, DPY, 3, DPH, D_SH2);   /* shadow into the spine */
    blit_rect(DRX, DPY, 3, DPH, D_SH2);           blit_rect(DRX + 3, DPY, 3, DPH, D_SH1);
    blit_rect(DLX + DPW, DPY - 2, 8, DPH + 4, D_SPINE);
    blit_rect(DRX + DPW - 30, DPY - 4, 6, 40, D_RED);                                             /* ribbon */
}

static void d_pageno(int right, int n)
{
    char s[12];
    sq_snprintf(s, sizeof(s), "- %d -", n);
    blit_text_uc((right ? DRX : DLX) + (DPW - (int)strlen(s) * 8) / 2, DPY + DPH - 12, s, D_FADE, D_PAPER);
}

static void d_title_page(void)
{
    char l[2][JR_LINE_MAX], s[40];
    d_line(0, 3, "Journey log", D_RED, 1);
    int k = 6;
    if (g_title[0]) {
        int n = journey_wrap(g_title, DCOLS, l);
        for (int i = 0; i < n; i++) d_line(0, k++, l[i], D_INK, 1);
        k++;
    }
    d_line(0, k, g_disc, D_FADE, 1);
    if (g_jr_begun[0]) {
        sq_snprintf(s, sizeof(s), "Begun %s %d %.4s", month_of(g_jr_begun), two(g_jr_begun + 6), g_jr_begun);
        d_line(0, 12, s, D_INK, 1);
    }
    sq_snprintf(s, sizeof(s), "%d entr%s", g_jr_total, g_jr_total == 1 ? "y" : "ies");
    d_line(0, 13, s, D_INK, 1);
    sq_snprintf(s, sizeof(s), "%dh %02dm played", (int)(g_jr_secs / 3600), (int)(g_jr_secs / 60 % 60));
    d_line(0, 14, s, D_INK, 1);
}

/* Entry page p (0 = the first page of entries) on the left or right. */
static void d_entries(int right, int p)
{
    char l[2][JR_LINE_MAX], s[40];
    if (p < 0 || p >= g_jr_pages) return;
    int end = p + 1 < g_jr_pages ? g_jr_first[p + 1] : g_jr_n, k = 0;
    for (int i = g_jr_first[p]; i < end; i++) {
        const char *st = g_jr[i].stamp;
        int y = two(st) * 100 + two(st + 2);
        sq_snprintf(s, sizeof(s), "%s %s %d   %.2s:%.2s", DAYS[journey_weekday(y, two(st + 4), two(st + 6))],
                    month_of(st), two(st + 6), st + 9, st + 11);
        d_line(right, k++, s, D_RED, 0);
        int n = journey_wrap(g_jr[i].text, DCOLS, l);
        for (int j = 0; j < n; j++) d_line(right, k++, l[j], D_INK, 0);
        sq_snprintf(s, sizeof(s), "%dh %02dm", (int)(g_jr[i].secs / 3600), (int)(g_jr[i].secs / 60 % 60));
        d_line(right, k++, s, D_FADE, 0);
        k++;
    }
    d_pageno(right, p + 1);
}

/* Spread 0 is the title page and the first page of entries. */
static void d_draw(int spread, int nspreads)
{
    if (blit_setup() < 0) return;
    d_book();
    if (spread == 0) {
        d_title_page();
        if (g_jr_pages) d_entries(1, 0);
        else {
            d_line(1, 3, "No entries yet.", D_INK, 0);
            d_line(1, 5, "A line is written when", D_FADE, 0);
            d_line(1, 6, "the game saves somewhere", D_FADE, 0);
            d_line(1, 7, "new.", D_FADE, 0);
        }
    } else {
        d_entries(0, spread * 2 - 1);
        d_entries(1, spread * 2);
    }
    blit_rect(0, 256, 480, 16, C_BG);
    const char *keys = nspreads > 1 ? "L/R turn page    O close" : "O close";
    blit_text_uc((480 - (int)strlen(keys) * 8) / 2, 260, keys, C_WHITE, C_BG);
}

/* The book is larger than the menu it opens from. The game is frozen and will
 * not repaint, so the parts of the screen the book covers outside the menu
 * box are kept while it is open and put back when it closes. */
static const short D_KEEP[5][4] = {
    { DX, DY, DW, BY - DY },                    /* above the menu box */
    { DX, BY + BH, DW, DY + DH - (BY + BH) },   /* below it */
    { DX, BY, BX - DX, BH },                    /* left of it */
    { BX + BW, BY, DX + DW - (BX + BW), BH },   /* right of it */
    { 0, 256, 480, 16 },                        /* the key line at the bottom */
};
#define D_KEEP_BYTES ((DW * (BY - DY) + DW * (DY + DH - BY - BH) + (BX - DX) * BH + (DX + DW - BX - BW) * BH + 480 * 16) * 4)

static void d_keep(u8 *buf, int out)
{
    if (!buf || blit_setup() < 0) return;
    for (int i = 0; i < 5; i++) {
        int n = out ? blit_copy_out(D_KEEP[i][0], D_KEEP[i][1], D_KEEP[i][2], D_KEEP[i][3], buf)
                    : blit_copy_in(D_KEEP[i][0], D_KEEP[i][1], D_KEEP[i][2], D_KEEP[i][3], buf);
        buf += n;
    }
}

static void journey_screen(void)
{
    const char *game = vault_name_ok(hook_game()) ? hook_game() : g_disc;
    SceUID keep_id = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_KERNEL, "CpBook", PSP_SMEM_Low, D_KEEP_BYTES, NULL);
    u8 *keep = keep_id >= 0 ? sceKernelGetBlockHeadAddr(keep_id) : NULL;
    if (!keep) sq_log("journey: no memory to keep the screen (%08X)", keep_id);
    d_keep(keep, 1);
    if (blit_setup() >= 0) message("Opening the journey log...", NULL, C_GREY);
    hook_play_flush();
    g_jr_n = journey_load(game, g_jr, JR_MAX, &g_jr_total, g_jr_begun);
    g_jr_secs = journey_time_add(game, 0);
    g_jr_pages = journey_paginate(g_jr, g_jr_n, DCOLS, DROWS, g_jr_first, JR_PAGES);
    int nspreads = (g_jr_pages + 2) / 2;                /* the title page shares the first spread */
    int spread = nspreads - 1;                          /* open where the ribbon is: the newest */
    wait_release();
    for (;;) {
        d_draw(spread, nspreads);
        u32 b = wait_press();
        if (b & PSP_CTRL_CIRCLE) break;
        if ((b & PSP_CTRL_LTRIGGER) && spread > 0) spread--;
        else if ((b & PSP_CTRL_RTRIGGER) && spread < nspreads - 1) spread++;
    }
    d_keep(keep, 0);                                    /* the menu box is redrawn by the caller */
    if (keep_id >= 0) sceKernelFreePartitionMemory(keep_id);
    if (!g_slide) wait_release();
}

/* ---- actions ------------------------------------------------------------ */
static const char *why(int r)
{
    switch (r) {
    case BT_E_SPACE:   return "Not enough free space on the Memory Stick";
    case BT_E_MISSING: return "That save or version is missing";
    case BT_E_TOOMANY: return "Too many files in that save";
    default:           return "Copy failed. Is the Memory Stick full?";
    }
}

static void do_backup_now(void)
{
    if (hook_busy()) { set_status(C_WARN, "The game is saving. Close the menu and try again."); return; }
    message("Copying the current save...", NULL, C_WHITE);
    int r = hook_backup_now(g_saves[g_sel_save]);
    sq_log("manual backup %s: %d", g_saves[g_sel_save], r);
    if (r == BT_OK)           set_status(C_OK, "Backed up");
    else if (r == BT_SAME)    set_status(C_GREY, "Already backed up: nothing has changed");
    else if (r == BT_NOTHING) set_status(C_WARN, "The game has no save there right now");
    else                      set_status(C_ERR, why(r));
}

static void do_restore(void)
{
    char stamp[BT_STAMP_LEN + 1];
    if (!g_nver) return;
    if (hook_busy()) { set_status(C_WARN, "The game is saving. Close the menu and try again."); return; }
    if (!confirm("Replace the game's current save with this version?")) { set_status(C_GREY, "Cancelled"); return; }
    strcpy(stamp, g_ver[g_sel_ver].stamp);

    /* Keep what is there now, so the restore itself can be undone. */
    message("Keeping a copy of the current save...", NULL, C_WHITE);
    int r = hook_backup_now(g_saves[g_sel_save]);
    if (r < 0) {
        sq_log("restore %s %s: safety copy failed %d", g_saves[g_sel_save], stamp, r);
        set_status(C_ERR, "Could not keep the current save first. Nothing changed.");
        return;
    }
    message("Restoring...", NULL, C_WHITE);
    if (bt_card_begin() < 0) r = BT_E_COPY;
    else { r = vault_restore(g_saves[g_sel_save], stamp); bt_card_end(); }
    sq_log("restore %s %s: %d", g_saves[g_sel_save], stamp, r);
    if (r == BT_OK) set_status(C_OK, "Restored. Now load it from the game's load screen.");
    else            set_status(C_ERR, r == BT_E_COPY ? "Restore failed. The current save was not changed." : why(r));
}

static void do_pin(void)
{
    if (!g_nver) return;
    int on = !g_ver[g_sel_ver].pinned;
    int r = BT_E_COPY;
    if (bt_card_begin() == 0) { r = vault_set_pin(g_saves[g_sel_save], g_ver[g_sel_ver].stamp, on); bt_card_end(); }
    sq_log("pin %s %s -> %d: %d", g_saves[g_sel_save], g_ver[g_sel_ver].stamp, on, r);
    if (r == BT_OK)          set_status(C_OK, on ? "Pinned: this version is kept until you delete it" : "Unpinned");
    else if (r == BT_E_PINS) set_status(C_WARN, "10 versions of this save are pinned already");
    else                     set_status(C_ERR, "Could not change the pin");
}

static void do_delete(void)
{
    if (!g_nver) return;
    if (!confirm(g_ver[g_sel_ver].pinned ? "Delete this PINNED version?" : "Delete this version?")) { set_status(C_GREY, "Cancelled"); return; }
    if (bt_card_begin() < 0) { set_status(C_GREY, "Cancelled"); return; }
    vault_delete_version(g_saves[g_sel_save], g_ver[g_sel_ver].stamp);
    bt_card_end();
    set_status(C_OK, "Deleted");
}

static void versions_screen(void)
{
    g_sel_ver = 0;
    load_versions();
    for (;;) {
        draw_versions();
        u32 b = wait_press();
        if (b & PSP_CTRL_UP)            { if (g_nver) g_sel_ver = (g_sel_ver + g_nver - 1) % g_nver; }
        else if (b & PSP_CTRL_DOWN)     { if (g_nver) g_sel_ver = (g_sel_ver + 1) % g_nver; }
        else if (b & PSP_CTRL_CROSS)    { do_restore(); load_versions(); }
        else if (b & PSP_CTRL_TRIANGLE) { do_delete(); load_versions(); }
        else if (b & PSP_CTRL_SQUARE)   { do_backup_now(); load_versions(); }
        else if (b & PSP_CTRL_START)    { do_pin(); load_versions(); }
        else if ((b & PSP_CTRL_SELECT) && g_cfg.journey) journey_screen();
        else if (b & PSP_CTRL_CIRCLE)   break;
    }
}

/* The save list and version screens. The game is already frozen. */
static void menu_body(void)
{
    set_status(C_GREY, "");
    if (!g_disc[0] && vault_name_ok(hook_game())) sq_strcpy(g_disc, sizeof(g_disc), hook_game());
    if (blit_setup() >= 0) { frame(g_disc); blit_text_uc(BX + 8, BY + 60, "One moment...", C_GREY, C_BG); }
    hook_flush();
    load_saves();
    wait_release();

    if (g_nsaves == 1) {                                /* one save: straight to its versions */
        g_sel_save = 0;
        versions_screen();
    } else {
        for (;;) {
            draw_saves();
            u32 b = wait_press();
            if (b & PSP_CTRL_CIRCLE) break;
            if ((b & PSP_CTRL_SELECT) && g_cfg.journey) { journey_screen(); continue; }
            if (!g_nsaves) continue;
            if (b & PSP_CTRL_UP)          g_sel_save = (g_sel_save + g_nsaves - 1) % g_nsaves;
            else if (b & PSP_CTRL_DOWN)   g_sel_save = (g_sel_save + 1) % g_nsaves;
            else if (b & PSP_CTRL_CROSS)  { versions_screen(); load_saves(); }
            else if (b & PSP_CTRL_SQUARE) { do_backup_now(); load_saves(); }
        }
    }
    wait_release();
}

/* 1 when Sidequest is running and is one that shows the pad again itself. */
static int sidequest_takes_pad(void)
{
    int (*takes)(void) = (void *)sctrlHENFindFunction("Sidequest", "SidequestLib", 0x5C4B5006);
    return takes && takes();
}

static void run_menu(void)
{
    home_block(1);
    freeze_game();
    pad_hide(1);
    power_hold();
    menu_body();
    pad_hide(0);
    thaw_game();
    home_block(0);
    power_done();
}

/* ---- quit screen --------------------------------------------------------- */
/* Shown instead of the system's "quit the game?" screen whenever the PS
 * button is pressed: when the game last saved, and what could be restored. */
#define QX 56
#define QY 64
#define QW 368
#define QH 144

/* Minutes since 2000-01-01 on the real calendar, from a YYYYMMDD_HHMMSS stamp. */
static u32 real_minutes(const char *s)
{
    int v[5], pos[5] = { 0, 4, 6, 9, 11 }, len[5] = { 4, 2, 2, 2, 2 };
    for (int i = 0; i < 5; i++) {
        v[i] = 0;
        for (int k = 0; k < len[i]; k++) v[i] = v[i] * 10 + (s[pos[i] + k] - '0');
    }
    int y = v[0], m = v[1], d = v[2];
    if (m <= 2) { y--; m += 12; }
    /* days since 2000-03-01, the usual shifted-year count */
    int days = 365 * (y - 2000) + (y - 2000) / 4 - (y - 2000) / 100 + (y - 2000) / 400 + (153 * (m - 3) + 2) / 5 + d - 1;
    return (u32)((days * 24 + v[3]) * 60 + v[4]);
}

/* The other plugins listen on event flags of their own:
 *   "SidequestReq"  bit 0: save the game into its suspend slot and quit it;
 *                   bit 1: open Sidequest's menu
 *   "EmblemReq"     bit 0: open Emblem's viewer (the flag only exists in a
 *                   game Emblem has an achievement file for)
 * Returns the flag, or -1 when that plugin is not running in this game. */
static SceUID plugin_flag(const char *name)
{
    static SceUID ids[64];
    int count = 0;
    if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_EventFlag, ids, 64, &count) < 0) return -1;
    if (count > 64) count = 64;
    for (int i = 0; i < count; i++) {
        SceKernelEventFlagInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelReferEventFlagStatus(ids[i], &info) >= 0 && !strcmp(info.name, name)) return ids[i];
    }
    return -1;
}

static void quit_screen(void)
{
    char name[BT_NAME_MAX], stamp[BT_STAMP_LEN + 1], now[BT_STAMP_LEN + 1];
    char head[48], l2[120], l3[48], fit[84];
    u32 color;

    freeze_game();
    pad_hide(1);
    power_hold();
    if (!g_disc[0] && vault_name_ok(hook_game())) sq_strcpy(g_disc, sizeof(g_disc), hook_game());
    if (blit_setup() >= 0) {
        blit_rect(QX, QY, QW, QH, C_BG);
        blit_text_uc(QX + 12, QY + 26, "One moment...", C_GREY, C_BG);
    }
    int saving = hook_busy();
    SceUID sidequest = plugin_flag("SidequestReq"), emblem = plugin_flag("EmblemReq");
    if (!saving) hook_flush();                          /* copy anything still waiting before a possible quit */
    load_saves();

    int total = 0, pins = 0, newest = -1;
    char newest_stamp[BT_STAMP_LEN + 1] = "";
    static BtVersion one[1];
    for (int i = 0; i < g_nsaves; i++) {
        total += g_save_count[i];
        pins += g_save_pins[i];
        if (g_save_count[i] > 0 && vault_list_versions(g_saves[i], one, 1) > 0 && strcmp(one[0].stamp, newest_stamp) > 0) {
            strcpy(newest_stamp, one[0].stamp);
            newest = i;
        }
    }

    now_stamp(now);
    if (hook_last_save(name, stamp)) {
        u32 a = real_minutes(now), b = real_minutes(stamp);
        u32 ago = a > b ? a - b : 0;
        if (ago == 0)       sq_snprintf(head, sizeof(head), "Saved just now");
        else if (ago < 120) sq_snprintf(head, sizeof(head), "%s %d minute%s ago", ago < (u32)g_cfg.warn_minutes ? "Saved" : "Last saved", (int)ago, ago == 1 ? "" : "s");
        else                sq_snprintf(head, sizeof(head), "Last saved %dh %02dmin ago", (int)(ago / 60), (int)(ago % 60));
        color = ago < (u32)g_cfg.warn_minutes ? C_OK : C_WARN;
        const char *title = name;
        for (int i = 0; i < g_nsaves; i++) if (!strcmp(g_saves[i], name) && g_save_title[i][0]) title = g_save_title[i];
        jt_fit(fit, sizeof(fit), title, 26);
        sq_snprintf(l2, sizeof(l2), "%s  %.2s-%.2s %.2s:%.2s", fit, stamp + 4, stamp + 6, stamp + 9, stamp + 11);
    } else {
        sq_snprintf(head, sizeof(head), "Not saved this session");
        color = C_ERR;
        if (newest >= 0)
            jt_fit(fit, sizeof(fit), g_save_title[newest][0] ? g_save_title[newest] : g_saves[newest], 20),
            sq_snprintf(l2, sizeof(l2), "Last: %s  %.2s-%.2s %.2s:%.2s", fit,
                        newest_stamp + 4, newest_stamp + 6, newest_stamp + 9, newest_stamp + 11);
        else
            sq_snprintf(l2, sizeof(l2), "No checkpoint of this game yet");
    }
    if (pins) sq_snprintf(l3, sizeof(l3), "%d checkpoint%s kept, %d pinned", total, total == 1 ? "" : "s", pins);
    else      sq_snprintf(l3, sizeof(l3), "%d checkpoint%s kept", total, total == 1 ? "" : "s");

    /* The buttons, in two columns (keys at QK1 and QK2). The other plugins' rows are only
     * there when that plugin is running in this game, and not while the game is saving:
     *     L  Sidequest     R  Emblem
     *     [] checkpoints   /\\ suspend and quit
     *     X  quit          O  back to game
     * Without Sidequest, "R  Emblem" takes the place beside "[] checkpoints". */
#define QK1 (QX + 12)
#define QK2 (QX + 12 + 17 * 8)
    int sq = sidequest >= 0 && !saving, em = emblem >= 0 && !saving;
    int qh = QH + (sq ? 14 : 0), qy = QY - (sq ? 7 : 0);       /* a third row makes the box taller, still centred */
    wait_release();
    for (;;) {
        if (blit_setup() >= 0) {
            blit_rect(QX, qy, QW, qh, C_BG);
            blit_rect(QX, qy, QW, 12, C_HI);
            blit_text_uc(QX + 4, qy + 2, "Checkpoint", C_WHITE, C_HI);
            blit_text_uc(QX + QW - 4 - 8 * (int)strlen(g_disc), qy + 2, g_disc, C_WHITE, C_HI);
            blit_text_uc(QX + 12, qy + 26, "Quit the game?", C_WHITE, C_BG);
            blit_rect(QX + 12, qy + 46, 4, 44, color);
            blit_text_uc(QX + 24, qy + 48, head, color, C_BG);
            blit_text_uc(QX + 24, qy + 64, l2, C_GREY, C_BG);
            blit_text_uc(QX + 24, qy + 80, l3, C_GREY, C_BG);
            int y = qy + 112;
            if (sq) {
                blit_text_uc(QK1, y, "L  Sidequest", C_GREY, C_BG);
                if (em) blit_text_uc(QK2, y, "R  Emblem", C_GREY, C_BG);
                y += 14;
            }
            blit_text_uc(QK1, y, "[] checkpoints", C_GREY, C_BG);
            if (sq)      blit_text_uc(QK2, y, "/\\ suspend and quit", C_GREY, C_BG);
            else if (em) blit_text_uc(QK2, y, "R  Emblem", C_GREY, C_BG);
            y += 14;
            if (saving) blit_text_uc(QK1, y, "The game is saving: O back to game", C_WARN, C_BG);
            else {
                blit_text_uc(QK1, y, "X  quit", C_WHITE, C_BG);
                blit_text_uc(QK2, y, "O  back to game", C_WHITE, C_BG);
            }
        }
        u32 b = wait_press();
        if (g_slide || (b & PSP_CTRL_CIRCLE)) break;
        if (b & PSP_CTRL_SQUARE) { menu_body(); break; }
        if (((b & PSP_CTRL_LTRIGGER) && sq) || ((b & PSP_CTRL_RTRIGGER) && em)) {
            /* Sidequest's menu or Emblem's viewer: let the game go and ask that
             * plugin to open its own screen. It freezes the game again itself. */
            int side = (b & PSP_CTRL_LTRIGGER) && sq;
            sq_log("quit: %s opened from the quit screen", side ? "Sidequest's menu" : "Emblem's viewer");
            wait_release();
            if (!(side && sidequest_takes_pad())) pad_hide(0);    /* Sidequest keeps it hidden until its own screen closes */
            thaw_game();
            power_release();                            /* that plugin holds its own power lock */
            sceKernelSetEventFlag(side ? sidequest : emblem, side ? 2 : 1);
            return;
        }
        if ((b & PSP_CTRL_TRIANGLE) && sq) {
            /* Hand over to Sidequest: it saves the game into a suspend slot and
             * does the quitting itself. We just get out of its way. */
            sq_log("quit: suspend and quit, handed to Sidequest");
            wait_release();
            hook_play_flush();
            if (!sidequest_takes_pad()) pad_hide(0);    /* hidden until Sidequest has taken the state: the game runs for a moment first */
            thaw_game();
            power_release();                            /* Sidequest's own sleep must not wait for our lock */
            sceKernelSetEventFlag(sidequest, 1);
            return;
        }
        if ((b & PSP_CTRL_CROSS) && !saving) {
            sq_log("quit: confirmed (%s)", head);
            hook_play_flush();
            wait_release();
            pad_hide(0);
            thaw_game();                                /* the game must run to act on the request */
            power_release();
            g_invoke_exit();                            /* exactly what the system's screen does on "Yes" */
            for (int t = 0; t < 250; t++) sceKernelDelayThread(20 * 1000);
            /* Still here after five seconds: the game ignored it. Leave directly. */
            sq_log("quit: game did not exit, leaving directly");
            sctrlKernelExitVSH(NULL);
            return;
        }
    }
    if (!g_slide) wait_release();
    pad_hide(0);
    thaw_game();
    power_done();
}

int ui_thread(SceSize args, void *argp)
{
    u16 type;
    u32 len = sizeof(g_disc) - 1;
    sceKernelDelayThread(4 * 1000 * 1000);              /* let the game boot first */
    memset(g_disc, 0, sizeof(g_disc));
    if (sctrlGetInitPARAM("DISC_ID", &type, &len, g_disc) < 0) g_disc[0] = 0;
    g_disc[sizeof(g_disc) - 1] = 0;
    if (!vault_name_ok(g_disc))                         /* not given by the system: read it off the disc image */
        vault_sfo_string("disc0:/PSP_GAME/PARAM.SFO", "DISC_ID", g_disc, sizeof(g_disc));
    if (!vault_name_ok(g_disc)) g_disc[0] = 0;          /* still unknown: filled in from the game's first save */
    len = sizeof(g_title) - 1;
    memset(g_title, 0, sizeof(g_title));
    if (sctrlGetInitPARAM("TITLE", &type, &len, g_title) < 0 || !g_title[0])
        vault_sfo_text("disc0:/PSP_GAME/PARAM.SFO", "TITLE", g_title, sizeof(g_title));
    g_title[sizeof(g_title) - 1] = 0;
    journey_clean(g_title, sizeof(g_title), g_title);
    sq_log("game id '%s' title '%s'", g_disc, g_title);
    sq_log("memory free: kernel %d KB (largest piece %d KB), user %d KB (largest %d KB)", (int)(sceKernelPartitionTotalFreeMemSize(1) >> 10), (int)(sceKernelPartitionMaxFreeMemSize(1) >> 10), (int)(sceKernelPartitionTotalFreeMemSize(2) >> 10), (int)(sceKernelPartitionMaxFreeMemSize(2) >> 10));

    home_setup();
    power_lookup();
    u32 held_since = 0, tick = 0, prev_home = BTN_HOME;  /* a press held across start-up does not count */
    for (;;) {
        sceKernelDelayThread(20 * 1000);
        hook_play_tick();
        hook_poll();                                    /* make a copy when the game has just saved */
        hook_mem_report();
        if (g_quit_on) {
            /* keep the system's screen away; a game or another plugin may switch it back */
            if ((tick++ & 15) == 0 && g_pop_get() != 0) g_pop_set(0);
            u32 home = read_raw() & BTN_HOME;
            if (home && !prev_home && !game_frozen_by_other()) {
                quit_screen();
                held_since = 0;
                prev_home = BTN_HOME;
                continue;
            }
            prev_home = home;
        }
        u32 b = read_buttons();
        if (b == g_cfg.hotkey) {
            u32 now = sceKernelGetSystemTimeLow();
            if (!held_since) held_since = now | 1;
            else if (now - held_since >= (u32)g_cfg.hold_ms * 1000) {
                held_since = 0;
                if (!game_frozen_by_other()) run_menu();
            }
        } else {
            held_since = 0;
        }
    }
    return 0;
}
