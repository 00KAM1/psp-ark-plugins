/*
 * Sidequest - in-game menu.
 *
 * Hold L + R + UP + SELECT (configurable) to open. While the menu is open
 * every user thread of the game is suspended, so the game is frozen and does
 * not see our button presses.
 */
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspinit.h>
#include <pspthreadman_kernel.h>
#include <pspsysmem_kernel.h>
#include <psprtc.h>
#include <string.h>
#include <systemctrl.h>
#include <systemctrl_se.h>
#include "common.h"
#include "blit.h"
#include "engine.h"
#include "staging.h"
#include "slots.h"


#define BTN_ALL 0x0000F3F9u

/* ---- freezing the game -------------------------------------------------- */
#define MAX_FROZEN 128
static SceUID g_frozen[MAX_FROZEN];
static int    g_nfrozen;

static void freeze_game(void)
{
    SceUID ids[MAX_FROZEN];
    int count = 0;
    if (g_nfrozen) return;                      /* already frozen */
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

/* 1 if every user thread is already suspended: another plugin (Checkpoint's
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
static int (*g_pw_lock)(int), (*g_pw_unlock)(int), (*g_pw_request)(void), (*g_pw_cancel)(void);
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
    g_pw_cancel = (void *)sctrlHENFindFunction(M, "scePower", 0xDB62C9CF);
    if (!g_pw_cancel) g_pw_cancel = (void *)sctrlHENFindFunction(M, "scePower_driver", 0xDB62C9CF);
    if (!g_pw_lock || !g_pw_unlock || !g_pw_request) g_pw_lock = g_pw_unlock = NULL;   /* all or nothing */
    sq_log("ui: POWER switch guard %s (cancel %s)", g_pw_lock ? "on" : "NOT available", g_pw_cancel ? "found" : "not found");
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
        sq_log("ui: POWER slid while a screen is open: closing it so the PSP can sleep");
    }
    return g_slide;
}

/* Our own sleep for a save or load is about to start. The lock must be gone
 * or that sleep would wait for it; a slide still waiting is dropped, so one
 * sleep happens and not two. */
static void power_before_own_sleep(void)
{
    if (g_pw_held && g_pw_cancel && g_pw_request()) { g_pw_cancel(); sq_log("ui: POWER slide during a save or load dropped"); }
    g_slide = 0;
    power_release();
}

/* The screen has closed because of a slide and the game runs again: let the
 * system act on it and come back when it is over. */
static void power_after_slide(void)
{
    power_release();
    if (!g_slide) return;
    for (int t = 0; t < 600 && g_pw_request(); t++) sceKernelDelayThread(50 * 1000);
    sceKernelDelayThread(500 * 1000);
    g_slide = 0;
    sq_log("ui: back after the POWER slide");
}

/* ---- input -------------------------------------------------------------- */
static u32 read_buttons(void)
{
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    if (sceCtrlPeekBufferPositive(&pad, 1) < 0) return 0;
    return pad.Buttons & BTN_ALL;
}

static void wait_release(void)
{
    while (read_buttons()) sceKernelDelayThread(16 * 1000);
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

/* ---- drawing ------------------------------------------------------------ */
#define BX 24
#define BY 28
#define BW 432
#define BH 216
#define LIST_W 168                              /* slot list on the left, picture on the right */
#define TH_X (BX + LIST_W + 12)
#define TH_Y (BY + 34)
#define MAX_GAMES 48

int slot_thumb_write(int k, const u16 *img, int w, int h);
SceUID slot_thumb_open(int k, int *w, int *h);
static int g_suspend;                       /* the save in progress is a suspend-and-quit */
static int g_save_ok;                       /* the last do_save wrote its slot */
/* The game's screen when the menu opened, for the slot's picture. 64 KB: taken
 * when a menu opens and given back when it closes, so it is not held through
 * the whole game. With none to be had the slot is saved without a picture. */
static u16 *g_shot;
static SceUID g_shot_id = -1;
static int g_shot_ok;

int blit_grab_shot(u16 *out);

static void shot_take(void)
{
    g_shot_ok = 0;
    if (g_shot_id < 0) {
        g_shot_id = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_KERNEL, "SqShot", PSP_SMEM_High, SHOT_W * SHOT_H * 2, NULL);
        g_shot = g_shot_id >= 0 ? sceKernelGetBlockHeadAddr(g_shot_id) : NULL;
        if (!g_shot) sq_log("ui: no memory for the slot picture (%08X)", g_shot_id);
    }
    if (g_shot) g_shot_ok = blit_grab_shot(g_shot) == 0;
}

static void shot_give(void)
{
    g_shot_ok = 0;
    g_shot = NULL;
    if (g_shot_id >= 0) sceKernelFreePartitionMemory(g_shot_id);
    g_shot_id = -1;
}
/* The picture on screen is only redrawn when it has to be: it is read from
 * the Memory Stick each time, which is too slow to do on every key press. */
static int g_pic_slot = -1, g_pic_page = -1, g_pic_dirty = 1;

/* Draw slot k's picture `want` pixels wide at x,y, straight from its file.
 * Pictures are stored 240 wide (older ones 120); they are shown as stored,
 * doubled, or halved to fit. Returns 1 if something was drawn. */
static int draw_picture(int k, int x, int y, int want)
{
    static u16 rows[SHOT_W * 4];
    int w = 0, h = 0, mode;
    SceUID fd = slot_thumb_open(k, &w, &h);
    if (fd < 0) return 0;
    if (w == want) mode = 1; else if (w * 2 == want) mode = 2; else if (w == want * 2) mode = 0; else { sceIoClose(fd); return 0; }
    if (w > SHOT_W) { sceIoClose(fd); return 0; }
    int ok = 1, out = y;
    for (int r = 0; r < h && ok; ) {
        int n = h - r < 4 ? h - r : 4;
        if (sceIoRead(fd, rows, n * w * 2) != n * w * 2) { ok = 0; break; }
        for (int i = 0; i < n; i++, r++) {
            const u16 *px = rows + i * w;
            if (mode == 0) { if (!(r & 1)) blit_span565(x, out++, px, w, 0); }
            else if (mode == 2) { blit_span565(x, out++, px, w, 2); blit_span565(x, out++, px, w, 2); }
            else blit_span565(x, out++, px, w, 1);
        }
    }
    sceIoClose(fd);
    return ok;
}

/* The menu shows one game at a time. Page 0 is the running game; L and R
 * step through the other games that have a folder. */
static char g_key[SQ_KEY_MAX];                  /* folder of the running game */
static char g_games[MAX_GAMES][SQ_KEY_MAX];     /* g_games[0] = g_key */
static int  g_ngames, g_page;
static char g_page_title[48];

typedef struct {
    int          valid;
    SqSlotHeader h;
} SlotView;

static SlotView g_view[SQ_MAX_SLOTS];
static int      g_sel;
static char     g_status[56];
static u32      g_status_color = C_GREY;
static char     g_title[48], g_disc[16], g_boot[128];
static int      g_real_umd;
static int      g_can_save;             /* 0 in the launcher and anything that is not an ISO or EBOOT game */
static u32      g_need, g_cap;          /* estimated packed size of this game / room for it, bytes */

static void set_status(u32 color, const char *msg)
{
    sq_strcpy(g_status, sizeof(g_status), msg);
    g_status_color = color;
}

static void load_views(void)
{
    for (int k = 0; k < SQ_MAX_SLOTS; k++)
        g_view[k].valid = slot_read_header(k, &g_view[k].h) == 0;
}

/* A folder name for a game, made from the file it runs from: the ISO's name,
 * or for an EBOOT.PBP the folder it sits in. Only letters, digits and '_',
 * plus four hex digits of a checksum of the whole path so that two games
 * with similar names never share a folder. */
static void make_key(const char *path, char *out)
{
    const char *end = path + strlen(path), *name = end, *stop = end;
    while (name > path && name[-1] != '/' && name[-1] != ':') name--;
    if (!strcmp(name, "EBOOT.PBP") && name > path) {    /* use the folder's name */
        stop = name - 1;
        name = stop;
        while (name > path && name[-1] != '/' && name[-1] != ':') name--;
    } else {
        const char *dot = stop;
        while (dot > name && *dot != '.') dot--;
        if (dot > name) stop = dot;
    }
    int n = 0;
    for (const char *c = name; c < stop && n < 20; c++) {
        char ch = *c;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        out[n++] = ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) ? ch : '_';
    }
    if (n == 0) { memcpy(out, "GAME", 4); n = 4; }
    sq_snprintf(out + n, SQ_KEY_MAX - n, "_%04X", crc32_update(0, path, (u32)strlen(path)) & 0xFFFF);
}

/* The running game first, then every other folder under ms0:/SIDEQUEST. */
static void load_games(void)
{
    g_ngames = 0;
    if (g_key[0]) strcpy(g_games[g_ngames++], g_key);
    SceUID d = sceIoDopen(SQ_DIR);
    if (d < 0) return;
    SceIoDirent e;
    for (;;) {
        memset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0 || g_ngames >= MAX_GAMES) break;
        if (!(e.d_stat.st_mode & FIO_S_IFDIR) || e.d_name[0] == '.') continue;
        if (strlen(e.d_name) >= SQ_KEY_MAX || !strcmp(e.d_name, g_key)) continue;
        strcpy(g_games[g_ngames++], e.d_name);
    }
    sceIoDclose(d);
}

static int own_page(void) { return g_page == 0 && g_key[0]; }

/* Point the slot code at a page's game and read its six headers. */
static void show_page(int page)
{
    if (g_ngames == 0) { g_page = 0; for (int k = 0; k < SQ_MAX_SLOTS; k++) g_view[k].valid = 0; g_page_title[0] = 0; return; }
    g_page = (page % g_ngames + g_ngames) % g_ngames;
    slots_set_game(g_games[g_page]);
    load_views();
    g_pic_dirty = 1;
    if (own_page()) sq_strcpy(g_page_title, sizeof(g_page_title), g_title);
    else {
        sq_strcpy(g_page_title, sizeof(g_page_title), g_games[g_page]);
        for (int k = 0; k < SQ_MAX_SLOTS; k++)
            if (g_view[k].valid) { sq_strcpy(g_page_title, sizeof(g_page_title), g_view[k].h.title); break; }
    }
}

/* The most recently saved of the six, or -1. */
static int newest_slot(void)
{
    int k = -1;
    for (int i = 0; i < SQ_MAX_SLOTS; i++)
        if (g_view[i].valid && (k < 0 || strcmp(g_view[i].h.saved_at, g_view[k].h.saved_at) > 0)) k = i;
    return k;
}

static int row_y(int k)
{
    /* "Suspend" heading, its row, a rule, "States" heading, five rows */
    return k == 0 ? BY + 48 : BY + 82 + (k - 1) * 14;
}

static void draw_thumb(void)
{
    blit_rect(TH_X - 1, TH_Y - 1, SHOT_W + 2, SHOT_H + 2, C_HI);
    if (!(g_view[g_sel].valid && draw_picture(g_sel, TH_X, TH_Y, SHOT_W))) {
        const char *msg = g_view[g_sel].valid ? "No picture" : "Empty";
        blit_rect(TH_X, TH_Y, SHOT_W, SHOT_H, 0x00181008);
        blit_text_uc(TH_X + SHOT_W / 2 - 4 * (int)strlen(msg), TH_Y + SHOT_H / 2 - 4, msg, C_GREY, 0x00181008);
    }
    g_pic_slot = g_sel; g_pic_page = g_page; g_pic_dirty = 0;
}

static void draw_row(int k)
{
    char line[32];
    int y = row_y(k);
    u32 bg = (k == g_sel) ? C_HI : C_BG;
    const char *when = g_view[k].h.saved_at;            /* "2026-11-19 09:14": show from the month */
    blit_rect(BX + 2, y - 2, LIST_W, 12, bg);
    if (k == 0) {
        sq_snprintf(line, sizeof(line), "   %.11s", g_view[k].valid ? (strlen(when) > 5 ? when + 5 : when) : "(none)");
        blit_text_uc(BX + 6, y, line, g_view[k].valid ? C_OK : C_GREY, bg);
    } else {
        sq_snprintf(line, sizeof(line), "%d  %.11s", k, g_view[k].valid ? (strlen(when) > 5 ? when + 5 : when) : "(empty)");
        blit_text_uc(BX + 6, y, line, g_view[k].valid ? C_WHITE : C_GREY, bg);
    }
}

static void draw_menu(void)
{
    if (blit_setup() < 0) return;
    char line[64];
    int keep_pic = !g_pic_dirty && g_pic_slot == g_sel && g_pic_page == g_page;
    if (!keep_pic) blit_rect(BX, BY, BW, BH, C_BG);
    else {                                              /* everything except the picture */
        int px0 = TH_X - 1, py0 = TH_Y - 1, px1 = TH_X + SHOT_W + 1, py1 = TH_Y + SHOT_H + 1;
        blit_rect(BX, BY, BW, py0 - BY, C_BG);
        blit_rect(BX, py0, px0 - BX, py1 - py0, C_BG);
        blit_rect(px1, py0, BX + BW - px1, py1 - py0, C_BG);
        blit_rect(BX, py1, BW, BY + BH - py1, C_BG);
    }
    blit_rect(BX, BY, BW, 12, C_HI);
    blit_text_uc(BX + 4, BY + 2, g_cfg.test ? "Sidequest " SQ_VERSION_STR " TEST" : "Sidequest " SQ_VERSION_STR, C_WHITE, C_HI);
    if (g_need && own_page()) {
        sq_snprintf(line, sizeof(line), "needs ~%d of %d MB", (g_need + 0xFFFFF) >> 20, g_cap >> 20);
        blit_text_uc(BX + BW - 4 - 8 * (int)strlen(line), BY + 2, line, g_need <= g_cap ? C_WHITE : C_WARN, C_HI);
    }

    /* whose slots these are; with more than one game, where L and R lead */
    sq_snprintf(line, sizeof(line), "%.40s", g_page_title);
    blit_text_uc(BX + 4, BY + 18, line, own_page() ? C_WHITE : C_WARN, C_BG);
    if (g_ngames > 1) {
        sq_snprintf(line, sizeof(line), "L/R %d/%d", g_page + 1, g_ngames);
        blit_text_uc(BX + BW - 4 - 8 * (int)strlen(line), BY + 18, line, C_GREY, C_BG);
    }

    blit_text_uc(BX + 6, BY + 36, "Suspend", C_GREY, C_BG);
    blit_rect(BX + 4, BY + 62, LIST_W - 4, 1, C_HI);
    blit_text_uc(BX + 6, BY + 70, "States", C_GREY, C_BG);
    for (int k = 0; k < SQ_MAX_SLOTS; k++) draw_row(k);
    if (!keep_pic) draw_thumb();

    int fy = BY + BH - 34;
    const char *keys;
    /* a slot that holds something can be loaded and deleted, the suspend included; X saves
     * into the states of the game that is running (the suspend is written by Suspend and Quit) */
    int full = g_view[g_sel].valid, can_save = own_page() && g_sel != 0;
    if (can_save)  keys = full ? "X save   [] load   /\\ delete   O close" : "X save   O close";
    else           keys = full ? "[] load   /\\ delete   O close" : "O close";
    blit_text_uc(BX + 4, fy, keys, C_WHITE, C_BG);
    if (!g_cfg.enable_load)
        blit_text_uc(BX + 4, fy + 10, "Load is off (enable_load=0)", C_GREY, C_BG);
    char st[56];
    if (g_status[0])         { sq_snprintf(st, sizeof(st), "%.52s", g_status); blit_text_uc(BX + 4, fy + 22, st, g_status_color, C_BG); }
    else if (!own_page())    blit_text_uc(BX + 4, fy + 22, "Another game: loading it switches games", C_GREY, C_BG);
    else if (g_sel == 0)     blit_text_uc(BX + 4, fy + 22, "Suspend is written by Suspend and Quit only", C_GREY, C_BG);
}

static void draw_message(const char *l1, const char *l2, u32 color)
{
    g_pic_dirty = 1;                                    /* painted over the picture */
    if (blit_setup() < 0) return;
    blit_rect(BX, BY + 70, BW, 60, C_BG);
    blit_text_uc(BX + 8, BY + 84, l1, color, C_BG);
    if (l2) blit_text_uc(BX + 8, BY + 100, l2, C_GREY, C_BG);
}

static const char *g_prog_label;
static void progress(u32 done, u32 total)
{
    g_pic_dirty = 1;
    if (blit_setup() < 0) return;
    blit_rect(BX, BY + 70, BW, 60, C_BG);
    blit_text_uc(BX + 8, BY + 80, g_prog_label, C_WHITE, C_BG);
    blit_bar(BX + 8, BY + 100, BW - 16, 10, done, total, C_OK, 0x00202020);
}

static int confirm(const char *question)
{
    draw_message(question, "X: yes     any other button: no", C_WARN);
    wait_release();
    return wait_press() == PSP_CTRL_CROSS;
}

/* Free space on the Memory Stick in KB, or 0xFFFFFFFF if it cannot be read. */
static u32 free_kb(void)
{
    u32 info[5] = { 0, 0, 0, 0, 0 };            /* max clusters, free clusters, max sectors, sector size, sectors per cluster */
    u32 *pinfo = info;
    if (sceIoDevctl("ms0:", 0x02425818, &pinfo, sizeof(pinfo), NULL, 0) < 0 || !info[3] || !info[4])
        return 0xFFFFFFFFu;
    return (u32)(((u64)info[1] * info[3] * info[4]) >> 10);
}

/* ---- after a load -------------------------------------------------------- */
/* A slot brings back the volume, mute and brightness it was saved with. So
 * that a load never surprises (sound blaring from a slot saved at home), the
 * PSP is put into a known state instead: muted, and a set brightness. The
 * settings are read fresh from config.txt, not taken from the slot. */
static void after_load_settings(void)
{
    int (*set_param)(u32, int);
    void (*set_bright)(int, int);
    config_load();
    set_param = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose_driver", 0x3C318569);
    if (!set_param) set_param = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose_driver", 0x810FB7FB);
    set_bright = (void *)sctrlHENFindFunction("sceDisplay_Service", "sceDisplay_driver", 0x60112E07);
    if (!set_bright) set_bright = (void *)sctrlHENFindFunction("sceDisplay_Service", "sceDisplay_driver", 0x9E3C6DC6);
    if (g_cfg.load_mute && set_param) set_param(0x8, 1);                    /* 0x8 = mute */
    if (g_cfg.load_brightness > 0 && set_bright) set_bright(g_cfg.load_brightness, 0);
    sq_log("after load: mute %s, brightness %d%% %s", !g_cfg.load_mute ? "left" : set_param ? "on" : "NOT available",
           g_cfg.load_brightness, g_cfg.load_brightness <= 0 ? "(left)" : set_bright ? "set" : "NOT available");
}

/* ---- Memory Stick safety ------------------------------------------------- */
static SceUID g_sentinel = -1;

/*
 * Other plugins ask this before they write (found with sctrlHENFindFunction,
 * module "Sidequest", library "SidequestLib").
 *   0  write as usual
 *   1  a state is being taken or loaded: wait
 *   2  a state was loaded and the stick could not be refreshed: do not write
 *      again until the next program start
 * It is set before the snapshot is taken, so every state holds it set: a
 * loaded state wakes up saying 1 and nothing has to raise it afterwards. It is
 * cleared only once the stick is known to be fresh.
 */
static volatile int g_card_busy;
int sidequestCardBusy(void) { return g_card_busy; }

/*
 * The other half of the handshake: each of the other plugins says when it is
 * in the middle of a write. A state must not be taken then (it would hold the
 * half-done write, to be finished after a load on a changed stick), and a
 * load must not cut one off. Called with g_card_busy already set, so no new
 * write starts while we wait. 0: nobody is writing. Otherwise: the plugin (1..)
 * that was still writing after ten seconds.
 */
static const struct { const char *mod, *lib; u32 nid; } g_writers[] = {
    { "Runtime",    "RuntimeLib",    0x5C4B5002 },
    { "Checkpoint", "CheckpointLib", 0x5C4B5003 },
    { "Emblem",     "EmblemLib",     0x5C4B5004 },
    { "Scanline",   "ScanlineLib",   0x5C4B5005 },
};
#define STILL_WRITING "A plugin is still writing: try again shortly"

static int writers_wait(const char *what)
{
    int who = 0;
    for (int t = 0; t <= 100; t++) {
        who = 0;
        for (int i = 0; i < (int)(sizeof(g_writers) / sizeof(g_writers[0])); i++) {
            int (*writing)(void) = (void *)sctrlHENFindFunction(g_writers[i].mod, g_writers[i].lib, g_writers[i].nid);
            if (writing && writing()) { who = i + 1; break; }
        }
        if (!who) {
            if (t) sq_log("%s: waited %d ms for the other plugins to finish writing", what, t * 100);
            return 0;
        }
        sceKernelDelayThread(100 * 1000);
    }
    sq_log("%s: refused, %s was still writing after 10 s", what, g_writers[who - 1].mod);
    return who;
}

static void sentinel_open(void)
{
    /* not the log: while this handle is open, appending to the same file may fail */
    g_sentinel = sceIoOpen(SQ_CONFIG_PATH, PSP_O_RDONLY, 0);
}

/* 1 if the handle opened before the suspend still works. */
static int sentinel_alive(void)
{
    char c;
    if (g_sentinel < 0) return 0;
    return sceIoLseek32(g_sentinel, 0, PSP_SEEK_SET) >= 0 && sceIoRead(g_sentinel, &c, 1) >= 0;
}

static void sentinel_close(void)
{
    if (g_sentinel >= 0) sceIoClose(g_sentinel);
    g_sentinel = -1;
}

/*
 * We have just become a restored snapshot. Our memory of the Memory Stick is
 * as old as the slot, and files have changed since. Before the game or this
 * plugin writes anything, make the file system driver forget what it knows:
 * devctl 0x0240D81E is the "stick was changed behind your back" call that is
 * also used after USB mode. A handle opened before the snapshot was taken
 * tells us whether a re-mount really happened.
 * Returns 1 when the stick is known to be fresh.
 */
static int stick_refresh(void)
{
    SceIoStat st;
    int have = g_sentinel >= 0;
    int before = sentinel_alive(), after = 0, dv = 0;
    int fresh = have && !before;                /* resume already re-mounted it */
    if (!fresh) {
        dv = sceIoDevctl("fatms0:", 0x0240D81E, NULL, 0, NULL, 0);
        for (int i = 0; i < 50; i++) {          /* up to 5s for the stick to come back */
            sceKernelDelayThread(100 * 1000);
            if (sceIoGetstat(SQ_DIR, &st) >= 0) break;
        }
        after = sentinel_alive();
        fresh = have ? !after : dv >= 0;
    }
    sentinel_close();
    sq_log("restored: stick handle=%d before=%d devctl=%08X after=%d -> %s",
           have, before, dv, after, fresh ? "fresh" : "NOT CONFIRMED");
    return fresh;
}

/* ---- actions ------------------------------------------------------------ */
static void now_string(char *out, int len)
{
    ScePspDateTime t;
    memset(&t, 0, sizeof(t));
    sceRtcGetCurrentClockLocalTime(&t);
    sq_snprintf(out, len, "%04d-%02d-%02d %02d:%02d", t.year, t.month, t.day, t.hour, t.minute);
}

static void measure(void)
{
    char err[80];
    SqRegion reg[SQ_MAX_REGIONS];
    static int described;
    g_need = 0;
    if (staging_alloc(err, sizeof(err)) < 0) { set_status(C_WARN, err); return; }
    g_cap = staging_capacity();
    if (!described) { staging_describe(); described = 1; }
    staging_free();
    int n = engine_regions(reg);
    /* Estimate only. From a normal thread we read nothing but the game's own
     * memory (partition 2). Kernel-side memory is not safe to read here: on
     * real hardware, reading all of lower RAM from this thread killed the
     * system (the 4MB "volatile" area is access-protected unless a game has
     * locked it). Everything else is counted at full size, so the figure is
     * on the high side. The real packing happens while suspended. */
    u32 g_lo = 0x88800000u, g_sz = 0x01800000u;
    PspSysMemPartition *p2 = sctrlGetMemoryPartition(2);
    if (p2 && p2->size && PHYS(p2->address) >= SQ_LOWER_PHYS &&
        PHYS(p2->address) + p2->size <= SQ_LOWER_PHYS + SQ_LOWER_SIZE) {
        g_lo = KSEG0(p2->address);
        g_sz = p2->size;
    }
    sq_log("menu: measuring game memory %08X + %08X", g_lo, g_sz);
    u32 words = sq_pack_measure((const u32 *)g_lo, g_sz / 4) + (SQ_LOWER_SIZE - g_sz) / 4 + 2 * n + 1;
    for (int i = 1; i < n; i++) words += reg[i].words;
    g_need = words * 4;
    sq_log("menu: needs about %d of %d bytes", g_need, g_cap);
}

/* Returns 1 if the menu should close (we are a freshly restored snapshot). */
/* Saving and loading put the PSP to sleep and wake it, and a load re-scans
 * the flash chip. None of that should start on a battery that may give out.
 * Returns 1 when it is fine to go on (charger connected, or above the limit,
 * or the level cannot be read). */
static int battery_ok(const char *what)
{
    static int (*percent)(void), (*online)(void);
    static int looked;
    if (!looked) {
        looked = 1;
        percent = (void *)sctrlHENFindFunction("scePower_Service", "scePower", 0x2085D15D);
        online  = (void *)sctrlHENFindFunction("scePower_Service", "scePower", 0x87440F5E);
    }
    if (!g_cfg.min_battery || !percent) return 1;
    int pct = percent(), ac = online ? online() : 0;
    if (pct < 0 || pct > g_cfg.min_battery || ac == 1) return 1;
    sq_log("%s: refused, battery at %d%% (limit %d%%) and no charger", what, pct, g_cfg.min_battery);
    set_status(C_ERR, "Battery too low: connect the charger first");
    return 0;
}

/* The internal flash around a save and after a load: see flashfix.c. */
static int g_flash_ok;                          /* the flash driver is the one flashfix.c knows (checked at game start) */
static int g_flash_detached, g_flash_det_rc, g_flash_att_rc;
static int g_flash_bad;                         /* a load could not re-scan the flash: no more saves until a power-off */

static int do_save(int k)
{
    char err[80];
    g_save_ok = 0;
    if (engine_ready(err, sizeof(err)) < 0) { set_status(C_ERR, err); return 0; }
    if (g_real_umd) { set_status(C_ERR, "UMD discs are not supported, only ISO/PSN"); return 0; }
    if (!g_can_save) { set_status(C_WARN, "Nothing to save here: start a game first"); return 0; }
    if (!own_page()) { set_status(C_WARN, "Start that game to save it"); return 0; }
    if (!g_suspend && k == 0) { set_status(C_WARN, "Suspend is written by Suspend and Quit only"); return 0; }
    {
        /* Refuse before sleeping if the stick has no room for the file. */
        u32 want = (g_need ? g_need : staging_capacity()) / 1024 + 2048, have = free_kb();
        if (have != 0xFFFFFFFFu && have < want) { set_status(C_ERR, "Not enough free space on the Memory Stick"); return 0; }
    }
    if (!g_flash_ok) { set_status(C_ERR, "This firmware is not supported: nothing saved"); return 0; }
    if (g_flash_bad) { set_status(C_ERR, "Power the PSP off fully before saving again"); return 0; }
    if (!battery_ok("save")) return 0;
    if (!g_suspend && g_view[k].valid && !confirm("Overwrite this slot?")) { set_status(C_GREY, "Cancelled"); return 0; }
    sq_log("save: X pressed, slot %d", k + 1);
    if (staging_alloc(err, sizeof(err)) < 0) { set_status(C_ERR, err); return 0; }
    sq_log("save: staging reserved");
    g_card_busy = 1;                            /* from here the other plugins start no write */
    if (writers_wait("save")) {                 /* and one that is in the middle of one is waited for */
        g_card_busy = 0;
        staging_free();
        set_status(C_ERR, STILL_WRITING);
        return 0;
    }

    if (engine_instant())
        draw_message("Taking snapshot...", "The screen may go dark for a few seconds.", C_WARN);
    else if (engine_self_wake())
        draw_message(g_suspend ? "Suspending. The screen goes dark for a few seconds."
                               : "Saving. The screen goes dark for a few seconds.",
                     "If it stays dark, slide POWER up.", C_WARN);
    else if (g_suspend)
        draw_message("Suspending: the PSP will now sleep.", "Slide POWER up to finish and quit the game.", C_WARN);
    else
        draw_message("The PSP will now sleep to take the snapshot.", "Slide POWER up to wake it.", C_WARN);
    sceKernelDelayThread((engine_instant() ? 300 : engine_self_wake() ? 1200 : 2500) * 1000);

    SqSlotHeader h;
    u32 len = 0;
    sq_log("save: slot %d, asking for %s suspend (test=%d)", k + 1, engine_instant() ? "instant" : "real", g_cfg.test);
    sceKernelDelayThread(200 * 1000);           /* let that line reach the stick */
    sentinel_open();
    /* flash1 is detached while the snapshot is taken, so the state holds no
     * file-system view of it. Nothing may be logged from here until we know
     * which system we woke up as. */
    g_ff.ran = 0;
    g_flash_att_rc = 1;
    g_flash_det_rc = ff_detach();
    g_flash_detached = g_flash_det_rc >= 0;
    thaw_game();                                /* its power callbacks must be able to run */
    power_before_own_sleep();
    int r = engine_capture(&h, &len, freeze_game);
    power_hold();                               /* awake again, game frozen again */
    /* FIRST thing after the wake. In a loaded state (r == 2) the flash driver's
     * block tables are as old as the state: have the driver scan the chip
     * again before anything can use flash1, and only then attach flash1. If
     * the scan could not be done, flash1 stays detached for this session: text
     * will be missing, but nothing can be written to the settings storage
     * through wrong tables. */
    int loaded = r == 2 && g_cfg.test != 3 && g_cfg.test != 4;
    int scan_rc = loaded ? ff_rescan() : 0;
    if (scan_rc == 0) {
        if (!g_flash_detached) g_flash_detached = ff_detach() >= 0;    /* the detach before the sleep had failed: replace the old view now */
        if (g_flash_detached) { g_flash_att_rc = ff_attach(); g_flash_detached = 0; }
    }

    if (r == 2 && (g_cfg.test == 3 || g_cfg.test == 4)) {
        /* Round-trip test: we are the same system, unpacked over itself. */
        int fresh = -1;
        if (g_cfg.test == 4) fresh = stick_refresh(); else sentinel_close();
        g_card_busy = 0;
        sq_log("test %d: snapshot unpacked in place, stick fresh=%d", g_cfg.test, fresh);
        set_status(C_OK, g_cfg.test == 3 ? "Test 3 passed: snapshot unpacked in place" :
                         fresh ? "Test 4 passed: unpacked, stick refreshed" : "Test 4: unpacked, stick NOT refreshed");
        return 0;
    }
    if (r == 2) {
        /* Not a save at all: this is the moment a slot was loaded, seen from
         * inside the snapshot. Nothing may touch the stick before this. */
        int fresh = stick_refresh();
        g_card_busy = fresh ? 0 : 2;
        staging_free();
        ff_report();
        {
            SceIoStat rs;
            memset(&rs, 0, sizeof(rs));
            int gs = sceIoGetstat("flash1:/registry/system.dreg", &rs);
            sq_log("flash: after load: detach %08X, attach %08X, registry file %s (%08X)", g_flash_det_rc, g_flash_att_rc,
                   gs >= 0 ? "found" : "NOT FOUND", gs);
        }
        after_load_settings();
        if (!g_ff.ok) {
            g_flash_bad = 1;
            draw_message("The internal flash could not be re-scanned.", "Text will be missing. Power off fully. X: go on", C_ERR);
            wait_release();
            while (wait_press() != PSP_CTRL_CROSS) {}
        }
        if (!fresh) {
            draw_message("Memory Stick was not refreshed!", "Saving now may damage files. X: go on", C_ERR);
            wait_release();
            while (wait_press() != PSP_CTRL_CROSS) {}
        }
        g_suspend = 0;                                  /* we are the game coming back; the slot stays */
        draw_message("Resumed from slot", g_title, C_OK);
        sceKernelDelayThread(1200 * 1000);
        return 1;
    }

    sentinel_close();
    g_card_busy = 0;                            /* the same system as before the sleep: its view of the stick is its own */
    sq_log("save: back from suspend, result %d, hook ran %d times, %d bytes packed", r, engine_hook_hits(), len);
    if (g_flash_det_rc < 0 || g_flash_att_rc < 0)
        sq_log("flash: around the save: detach %08X, attach %08X", g_flash_det_rc, g_flash_att_rc);
    if (r == 10 || r == 11) {
        staging_free();
        set_status(C_OK, r == 10 ? "Test 1 passed: suspend and resume work"
                                 : "Test 2 passed: Sidequest's hook ran");
        return 0;
    }
    if (r != 1) {
        staging_free();
        sq_log("capture failed: %d", r);
        set_status(C_ERR, r == -5 ? "Game uses too much memory to fit" :
                          r == -4 ? "The PSP did not suspend (game busy?)" :
                          r == -3 ? "Suspend did not reach Sidequest (see log)" : "Snapshot failed");
        return 0;
    }

    sq_strcpy(h.title, sizeof(h.title), g_title);
    sq_strcpy(h.disc_id, sizeof(h.disc_id), g_disc);
    sq_strcpy(h.boot_path, sizeof(h.boot_path), g_boot);
    now_string(h.saved_at, sizeof(h.saved_at));

    sq_log("save: writing slot file");
    g_prog_label = "Writing slot to Memory Stick...";
    for (int tries = 0; tries < 10; tries++) {  /* the stick may still be waking up */
        r = slot_write(k, &h, staging_ptr(), len, progress);
        if (r != -1) break;
        sceKernelDelayThread(500 * 1000);
    }
    staging_free();
    if (r == 0) {
        slot_thumb_write(k, g_shot_ok ? g_shot : NULL, SHOT_W, SHOT_H);
        g_save_ok = 1;
    }
    sq_log("slot %d: %d bytes, write=%d", k, len, r);
    load_views();
    g_pic_dirty = 1;
    set_status(r == 0 ? C_OK : C_ERR, r == 0 ? "Saved" : "Write failed. Is the Memory Stick full?");
    return 0;
}

static void do_load(int k, int ask)
{
    char err[80];
    SceIoStat st;
    const SqSlotHeader *h = &g_view[k].h;
    if (!g_view[k].valid) { set_status(C_GREY, "Slot is empty"); return; }
    if (!g_cfg.enable_load) { set_status(C_WARN, "Set enable_load=1 in config.txt first"); return; }
    if (!battery_ok("load")) return;
    if (engine_ready(err, sizeof(err)) < 0) { set_status(C_ERR, err); return; }
    if (!strncmp(h->boot_path, "ms0:", 4) && sceIoGetstat(h->boot_path, &st) < 0) {
        set_status(C_ERR, "That game's file was moved or deleted");
        return;
    }
    if (staging_alloc(err, sizeof(err)) < 0) { set_status(C_ERR, err); return; }
    if (engine_validate(h, err, sizeof(err)) < 0) { staging_free(); set_status(C_ERR, err); return; }
    if (h->stream_len > staging_capacity()) { staging_free(); set_status(C_ERR, "Slot is too big for this PSP"); return; }
    if (ask && !confirm(own_page() ? "Load this state? Unsaved progress is lost." : "Switch games? Unsaved progress here is lost.")) { staging_free(); set_status(C_GREY, "Cancelled"); return; }

    g_card_busy = 1;                            /* the other plugins start no write; this system is about to be replaced */
    if (writers_wait("load")) { g_card_busy = 0; staging_free(); set_status(C_ERR, STILL_WRITING); return; }
    g_prog_label = "Reading slot...";
    int r = slot_read_stream(k, h, staging_ptr(), staging_capacity(), progress);
    if (r == 0) {
        SqRegion reg[SQ_MAX_REGIONS];
        int n = engine_regions(reg);
        if (sq_stream_validate((const u32 *)staging_ptr(), h->stream_len, reg, n) < 0) r = -3;
    }
    if (r < 0) {
        g_card_busy = 0;
        staging_free();
        set_status(C_ERR, r == -3 ? "Slot data is damaged. Not loaded." : "Could not read the slot");
        return;
    }

    if (engine_instant())
        draw_message("Switching...", "The screen may go dark for a few seconds.", C_WARN);
    else if (engine_self_wake())
        draw_message("Loading. The screen goes dark for a few seconds.", "If it stays dark, slide POWER up.", C_WARN);
    else
        draw_message("Switching. The PSP will sleep now.", "Slide POWER up to continue.", C_WARN);
    sceKernelDelayThread((engine_instant() ? 300 : engine_self_wake() ? 1200 : 2500) * 1000);

    sq_log("load: slot %d verified (%d bytes), %s suspend", k + 1, h->stream_len, engine_instant() ? "instant" : "real");
    sceKernelDelayThread(200 * 1000);
    thaw_game();
    power_before_own_sleep();
    r = engine_restore(h, freeze_game);
    /* Only reached when the switch did not happen; this game is untouched. */
    power_hold();
    g_card_busy = 0;
    staging_free();
    sq_log("restore failed: %d", r);
    set_status(C_ERR, r == -6 ? "Stopped safely: slot data changed in RAM" :
                      r == -4 ? "The PSP did not suspend (game busy?)" : "Switch did not happen (see log)");
}

static void do_delete(int k)
{
    if (!g_view[k].valid) return;
    if (!confirm(k == 0 ? "Delete this suspend?" : "Delete this state?")) { set_status(C_GREY, "Cancelled"); return; }
    slot_delete(k);
    load_views();
    g_pic_dirty = 1;
    set_status(C_OK, "Deleted");
}

/* Other plugins ask this before they leave the pad hidden for us (Checkpoint's quit
 * screen does, when it hands over to a suspend or to this menu): an older Sidequest
 * would never show it again. */
int sidequestTakesPad(void) { return 1; }

/* While the menu is open the PS button must not bring up the system's
 * "quit game?" screen: the game is frozen and quitting from there hangs.
 * This is the switch games use to handle the PS button themselves. */
static int home_block(int on)
{
    static int (*get)(void), (*set)(int);
    static int looked, saved = 1;
    if (!looked) {
        looked = 1;
        get = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose", 0x0F341BE4);
        set = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose", 0x5595A71A);
        if (!get) get = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose_driver", 0x0F341BE4);
        if (!set) set = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose_driver", 0x5595A71A);
        sq_log("ui: PS button switch %s", get && set ? "found" : "NOT found");
    }
    pad_hide(on);                               /* every screen opens and closes through here */
    if (!get || !set) return -1;
    if (on) { saved = get(); set(0); }
    else    set(saved);
    return 0;
}

static void run_menu(void)
{
    home_block(1);
    freeze_game();
    power_hold();
    shot_take();                                /* before the menu paints over the game */
    load_games();
    show_page(0);
    set_status(C_GREY, "");
    g_need = 0;
    if (g_sel >= SQ_MAX_SLOTS) g_sel = 1;
    if (g_sel == 0 && !g_view[0].valid) g_sel = 1;      /* open on the states unless there is a suspend to load */
    draw_menu();
    if (g_can_save && own_page()) measure();
    wait_release();

    for (;;) {
        draw_menu();
        u32 b = wait_press();
        if (b & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) {
            if (g_ngames > 1) { show_page(g_page + ((b & PSP_CTRL_RTRIGGER) ? 1 : -1)); set_status(C_GREY, ""); }
            continue;
        }
        set_status(C_GREY, "");
        if (b & PSP_CTRL_UP)            g_sel = (g_sel + SQ_MAX_SLOTS - 1) % SQ_MAX_SLOTS;
        else if (b & PSP_CTRL_DOWN)     g_sel = (g_sel + 1) % SQ_MAX_SLOTS;
        else if (b & PSP_CTRL_CROSS)    { if (do_save(g_sel)) break; }
        else if (b & PSP_CTRL_SQUARE)   do_load(g_sel, 1);
        else if (b & PSP_CTRL_TRIANGLE) do_delete(g_sel);
        else if (b & PSP_CTRL_CIRCLE)   break;
    }
    shot_give();
    show_page(0);                                       /* leave the slot code on the running game */
    if (!g_slide) wait_release();
    thaw_game();
    home_block(0);
    power_after_slide();
}

/* ---- resume prompt ------------------------------------------------------ */
/* When a game starts and one of the slots was saved from this same game,
 * ask whether to go straight back to it. X accepts, O starts the game. */
#define RP_X 8
#define RP_Y 184
#define RP_W 464
#define RP_H 80

/* A launcher can ask for the newest slot to be loaded without the question:
 * it writes the game's path (the ISO, or the EBOOT.PBP) as the one line of
 * ms0:/SIDEQUEST/RESUME.REQ just before it starts the game. Shelf does this
 * when a game is started from its Playing tab ("Resume suspended game").
 * The file is used once: it is removed when it names this game. One that names
 * another program is left alone (the launcher itself may be the program running
 * here, a moment before it starts the game); the launcher clears leftovers.
 * Returns 1 when it asks for this game. */
#define SQ_RESUME_REQ SQ_DIR "/RESUME.REQ"

/*
 * A launcher can also name the slot, or ask for none: ms0:/SIDEQUEST/START.REQ
 * holds the game's path and, on a second line, the slot number (0 = suspend,
 * 1..5 = states) or the word "none" (start the game, and do not ask either).
 * Shelf's "Start from..." writes it. It is a file of its own so that a
 * Sidequest from before it, which knows only RESUME.REQ, does not take the
 * path for "load the newest".
 */
#define SQ_START_REQ SQ_DIR "/START.REQ"

/* 1 when the request file names this game: it is removed, and its second line
 * (if any) is copied to `second`. */
static int request_for_me(const char *file, char *second, int size)
{
    char b[sizeof(g_boot) + 24];
    if (second && size > 0) second[0] = 0;
    SceUID fd = sceIoOpen(file, PSP_O_RDONLY, 0);
    if (fd < 0) return 0;
    int n = sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    char *rest = NULL;
    for (int i = 0; i < n; i++) if (b[i] == '\r' || b[i] == '\n') {
        b[i] = 0;
        rest = b + i + 1;
        while (*rest == '\r' || *rest == '\n') rest++;
        break;
    }
    const char *x = b, *y = g_boot;                     /* same path, whatever the letter case */
    for (; *x && *y; x++, y++) {
        char cx = *x, cy = *y;
        if (cx >= 'a' && cx <= 'z') cx -= 32;
        if (cy >= 'a' && cy <= 'z') cy -= 32;
        if (cx != cy) return 0;
    }
    if (*x || *y) return 0;
    if (second && rest) {
        int i = 0;
        for (; rest[i] && rest[i] != '\r' && rest[i] != '\n' && i < size - 1; i++) second[i] = rest[i];
        second[i] = 0;
    }
    sceIoRemove(file);
    return 1;
}

static void resume_prompt(void)
{
    char line[64], pick[8];
    int want = -1;                                      /* a slot named by the launcher */
    int asked = g_boot[0] ? request_for_me(SQ_RESUME_REQ, NULL, 0) : 0;     /* honoured even when the prompt is off */
    if (g_boot[0] && request_for_me(SQ_START_REQ, pick, sizeof(pick))) {
        if (pick[0] >= '0' && pick[0] <= '5' && !pick[1]) { want = pick[0] - '0'; asked = 1; }
        else {                                          /* "none", or anything else: a plain start, and no question */
            sq_log("resume: the launcher asked for a start without loading");
            return;
        }
    }
    if ((!g_cfg.resume_prompt && !asked) || !g_cfg.enable_load || !g_can_save || !g_boot[0]) return;
    load_games();
    show_page(0);
    int k = newest_slot();                      /* the most recent of this game's suspend and states */
    if (want >= 0) {
        if (g_view[want].valid) k = want;
        else { sq_log("resume: the launcher asked for slot %d, which is empty: asking instead", want); asked = 0; }
    }
    if (k < 0) return;
    if (!asked && !g_cfg.resume_prompt) return;
    const char *when = g_view[k].h.saved_at;
    if (want >= 0 && asked) sq_log("resume: slot %d named by the launcher, loading without the question", k);
    else sq_log(asked ? "resume: slot %d asked for by the launcher, loading without the question" : "resume: offering slot %d", k);

    /* The game is frozen while the question is on screen: nothing paints
     * over it, and it stays until one of the two buttons is pressed. */
again:
    home_block(1);
    freeze_game();
    power_hold();
    wait_release();
    g_pic_dirty = 1;
    if (asked) goto load;                       /* the launcher already said yes */
    if (blit_setup() >= 0) {
        blit_rect(RP_X, RP_Y, RP_W, RP_H, C_BG);
        blit_rect(RP_X, RP_Y, RP_W, 2, C_HI);
        int pic = draw_picture(k, RP_X + 6, RP_Y + 7, SHOT_W / 2);     /* half size here */
        int tx = RP_X + 8 + (pic ? SHOT_W / 2 + 8 : 0);
        blit_text_uc(tx, RP_Y + 10, "Continue where you left off?", C_WHITE, C_BG);
        sq_snprintf(line, sizeof(line), "%.38s", g_view[k].h.title);
        blit_text_uc(tx, RP_Y + 26, line, C_GREY, C_BG);
        if (k == 0) sq_snprintf(line, sizeof(line), "Suspended %s", strlen(when) > 5 ? when + 5 : when);
        else        sq_snprintf(line, sizeof(line), "State %d, saved %s", k, strlen(when) > 5 ? when + 5 : when);
        blit_text_uc(tx, RP_Y + 40, line, C_GREY, C_BG);
        blit_text_uc(tx, RP_Y + 58, "X continue   O start the game", C_OK, C_BG);
    }
    for (;;) {
        u32 b = wait_press();
        if (g_slide) {                          /* POWER was slid: sleep first, then ask again */
            thaw_game();
            home_block(0);
            power_after_slide();
            sceKernelDelayThread(1000 * 1000);
            goto again;
        }
        if (b & PSP_CTRL_CROSS) break;
        if (b & PSP_CTRL_CIRCLE) {
            sq_log("resume: declined");
            wait_release();
            thaw_game();
            home_block(0);
            power_release();
            return;
        }
    }
load:
    wait_release();
    set_status(C_GREY, "");
    do_load(k, 0);
    /* Only reached when the load did not happen: say why, then carry on with the game. */
    sq_log("resume: not loaded: %s", g_status);
    draw_message("Could not continue from the slot", g_status, C_ERR);
    sceKernelDelayThread(3500 * 1000);
    thaw_game();
    home_block(0);
    power_after_slide();
}

/* ---- suspend and quit ---------------------------------------------------- */
/* Another plugin (Checkpoint's quit screen) can ask for this by setting bit 0
 * of the event flag named "SidequestReq" (bit 1 asks for the menu instead:
 * see the main loop). Sidequest saves the game into a
 * suspend slot and then quits it; the continue prompt offers that slot the
 * next time the game starts. Sidequest does the quitting itself because only
 * it can tell "just saved" from "just resumed from that save". */
static SceUID g_req = -1;

static void quit_game(void)
{
    int (*invoke_exit)(void) = (void *)sctrlHENFindFunction("sceLoadExec", "LoadExecForKernel", 0x1F08547A);
    if (!invoke_exit) invoke_exit = (void *)sctrlHENFindFunction("sceLoadExec", "LoadExecForKernel", 0x62A27008);
    if (invoke_exit) {                                  /* what the system's quit screen does on "Yes" */
        invoke_exit();
        for (int t = 0; t < 250; t++) sceKernelDelayThread(20 * 1000);
    }
    sq_log("suspend: game did not exit by itself, leaving directly");
    sctrlKernelExitVSH(NULL);
}

static void suspend_and_quit(void)
{
    sceKernelDelayThread(400 * 1000);                   /* let the game paint over the screen that asked */
    home_block(1);
    freeze_game();
    power_hold();
    shot_take();
    load_games();
    show_page(0);
    set_status(C_GREY, "");
    wait_release();

    int k = 0;                                          /* the game's suspend slot, replaced every time */
    sq_log("suspend: %s", g_key);
    g_suspend = 1;
    int resumed = do_save(k);
    g_suspend = 0;
    shot_give();
    if (resumed) {                                      /* we are the game coming back, not the one leaving */
        thaw_game();
        home_block(0);
        power_after_slide();
        return;
    }
    if (!g_save_ok) {
        sq_log("suspend: not saved: %s", g_status);
        draw_message("Could not suspend the game", g_status, C_ERR);
        sceKernelDelayThread(3500 * 1000);
        thaw_game();
        home_block(0);
        power_after_slide();
        return;
    }
    draw_message("Suspended", "Quitting the game...", C_OK);
    sceKernelDelayThread(800 * 1000);
    pad_hide(0);                                        /* the state is taken; nothing may leave the pad hidden for what runs next */
    thaw_game();                                        /* the game must run to act on the request */
    power_release();
    g_slide = 0;
    quit_game();
}

/* ---- main loop ---------------------------------------------------------- */
static void read_game_identity(void)
{
    u32 len = sizeof(g_title) - 1;
    u16 type;
    memset(g_title, 0, sizeof(g_title));
    memset(g_disc, 0, sizeof(g_disc));
    memset(g_boot, 0, sizeof(g_boot));
    int no_title = sctrlGetInitPARAM("TITLE", &type, &len, g_title) < 0 || !g_title[0];
    len = sizeof(g_disc) - 1;
    if (sctrlGetInitPARAM("DISC_ID", &type, &len, g_disc) < 0) g_disc[0] = 0;
    g_title[sizeof(g_title) - 1] = 0;
    g_disc[sizeof(g_disc) - 1] = 0;

    const char *iso = sctrlSEGetUmdFile();
    const char *exe = sceKernelInitFileName();
    sq_strcpy(g_boot, sizeof(g_boot), (iso && iso[0]) ? iso : exe);
    /* started from disc0: with no ISO mounted = a real UMD in the drive */
    g_real_umd = !(iso && iso[0]) && exe && !strncmp(exe, "disc0:", 6);

    /* Saving is for games only: an ISO, or an EBOOT.PBP that is not the
     * launcher. In the launcher the menu is a slot picker (load/delete). */
    /* ARK-5 exports this to tell its custom launcher from other homebrew */
    int (*is_launcher)(void) = (void *)sctrlHENFindFunction("SystemControl", "ArkCtrl", 0x65C8280F);
    int n = (int)strlen(g_boot);
    int is_eboot = n >= 9 && !strcmp(g_boot + n - 9, "EBOOT.PBP");
    int launcher = (exe && exe[0] && is_launcher && is_launcher()) || strstr(g_boot, "VBOOT.PBP") || strstr(g_boot, "/ARK_01234/");
    g_can_save = !launcher && ((iso && iso[0]) || is_eboot);
    if (launcher) { sq_strcpy(g_title, sizeof(g_title), "Launcher"); g_disc[0] = 0; no_title = 0; }

    if (no_title) {                             /* fall back to the file name, without folder or extension */
        const char *name = strrchr(g_boot, '/');
        sq_strcpy(g_title, sizeof(g_title), name ? name + 1 : (g_boot[0] ? g_boot : "Unknown"));
        char *dot = strrchr(g_title, '.');
        if (dot && dot != g_title) *dot = 0;
    }
}

int ui_thread(SceSize args, void *argp)
{
    sceKernelDelayThread(4 * 1000 * 1000);      /* let the game boot first */
    read_game_identity();
    sq_log("ui: '%s' %s from %s%s%s", g_title, g_disc, g_boot, g_real_umd ? " (UMD)" : "", g_can_save ? "" : " (load only)");
    if (g_can_save && g_boot[0]) make_key(g_boot, g_key);
    sq_log("ui: folder '%s'", g_key);
    {
        u32 at = 0;
        int v = ff_check(&at);
        g_flash_ok = v == 0;
        sq_log("flash: driver at %08X, check %d (%s)", at, v, v ? "NOT the driver Sidequest knows: saving is off" : "ok");
    }
    load_games();
    show_page(0);
    power_lookup();
    resume_prompt();
    g_req = sceKernelCreateEventFlag("SidequestReq", 0, 0, NULL);

    u32 held_since = 0;
    for (;;) {
        u32 bits = 0;
        sceKernelDelayThread(20 * 1000);
        if (g_req >= 0 && sceKernelPollEventFlag(g_req, 3, PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &bits) >= 0) {
            if (bits & 1) suspend_and_quit();
            else {
                /* Bit 1: Checkpoint's quit screen asks for the menu (its L button).
                 * The game runs for a moment first, so it paints over that screen
                 * and the slot picture taken when the menu opens shows the game. */
                sq_log("ui: menu asked for by Checkpoint's quit screen");
                sceKernelDelayThread(400 * 1000);
                if (!game_frozen_by_other()) run_menu();
            }
            held_since = 0;
            continue;
        }
        u32 b = read_buttons();
        if (b == g_cfg.hotkey) {                 /* exactly the hotkey, nothing else held */
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
