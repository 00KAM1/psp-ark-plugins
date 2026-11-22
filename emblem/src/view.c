/*
 * Emblem - the viewer: every achievement of the game, earned or not, over the
 * frozen game. Opened with L + R + LEFT held for half a second.
 *
 * Freezing the game, the PS button and the POWER switch are handled the way
 * Checkpoint's menu handles them on this PSP (proven there):
 *  - the game's user threads are suspended while the viewer is open;
 *  - the system's "quit the game?" screen is kept away while it is open;
 *  - a power lock is held, so a slide of POWER waits; the viewer then closes
 *    itself, lets the game go, and the PSP sleeps the normal way.
 */
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspthreadman_kernel.h>
#include <string.h>
#include <systemctrl.h>
#include "common.h"
#include "draw.h"
#include "view.h"

extern u8 msx[];                                /* 8x8 font, from libpspdebug */

#define BTN_ALL 0x0000F3F9u

/* ---- freeze the game while the viewer is open (Checkpoint's code) --------- */
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

/* 1 if every user thread is already suspended: another plugin (Sidequest's or
 * Checkpoint's menu) has the game frozen and owns the screen. */
int view_blocked(void)
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

/* ---- the PS button ------------------------------------------------------- */
static int (*g_pop_get)(void), (*g_pop_set)(int);

static void home_block(int on)
{
    static int saved = 1;
    if (!g_pop_get || !g_pop_set) return;
    if (on) { saved = g_pop_get(); g_pop_set(0); }
    else    g_pop_set(saved);
}

/* ---- the POWER switch while the game is frozen (Checkpoint's code) --------- */
static int (*g_pw_lock)(int), (*g_pw_unlock)(int), (*g_pw_request)(void);
static int g_pw_held, g_slide;

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

static int power_slid(void)
{
    if (!g_slide && g_pw_held && g_pw_request()) {
        g_slide = 1;
        ac_log("POWER slid while the viewer is open: closing it so the PSP can sleep");
    }
    return g_slide;
}

static void power_done(void)
{
    power_release();
    if (!g_slide) return;
    for (int t = 0; t < 600 && g_pw_request(); t++) sceKernelDelayThread(50 * 1000);
    sceKernelDelayThread(500 * 1000);
    g_slide = 0;
    ac_log("back after the POWER slide");
}

void view_setup(void)
{
    static const char *M = "scePower_Service";
    g_pop_get = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose", 0x0F341BE4);
    g_pop_set = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose", 0x5595A71A);
    if (!g_pop_get) g_pop_get = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose_driver", 0xE9A42056);
    if (!g_pop_set) g_pop_set = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose_driver", 0xC08C41EF);
    g_pw_lock   = (void *)sctrlHENFindFunction(M, "scePower_driver", 0x6CF50928);   /* kernel form: never waits */
    g_pw_unlock = (void *)sctrlHENFindFunction(M, "scePower_driver", 0xC3024FE6);
    if (!g_pw_lock || !g_pw_unlock) {
        g_pw_lock   = (void *)sctrlHENFindFunction(M, "scePower", 0xD6D016EF);
        g_pw_unlock = (void *)sctrlHENFindFunction(M, "scePower", 0xCA3D34C1);
    }
    g_pw_request = (void *)sctrlHENFindFunction(M, "scePower", 0x7FA406DD);
    if (!g_pw_request) g_pw_request = (void *)sctrlHENFindFunction(M, "scePower_driver", 0x566B8353);
    if (!g_pw_lock || !g_pw_unlock || !g_pw_request) g_pw_lock = g_pw_unlock = NULL;
    ac_log("viewer: PS button switch %s, POWER switch guard %s", g_pop_get && g_pop_set ? "found" : "NOT found",
           g_pw_lock ? "on" : "NOT available");
}

/* ---- input --------------------------------------------------------------- */
static u32 read_buttons(void)
{
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    if (sceCtrlPeekBufferPositive(&pad, 1) < 0) return 0;
    return pad.Buttons & BTN_ALL;
}

static void wait_release(void) { while (read_buttons()) sceKernelDelayThread(16 * 1000); }

/* The next press. UP, DOWN, L and R repeat when held. */
static u32 g_prev;
static u32 wait_input(void)
{
    static u32 since, last;
    for (;;) {
        if (power_slid()) return PSP_CTRL_CIRCLE;       /* "close" */
        sceKernelDelayThread(16 * 1000);
        u32 now = read_buttons(), t = sceKernelGetSystemTimeLow();
        u32 edge = now & ~g_prev;
        g_prev = now;
        if (edge) { since = last = t; return edge; }
        u32 rep = now & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER);
        if (rep && t - since >= 400 * 1000 && t - last >= 80 * 1000) { last = t; return rep; }
    }
}

/* ---- the screen ------------------------------------------------------------ */
static int surface(DrSurface *s)
{
    void *top = NULL;
    int bw = 0, fmt = 0;
    sceDisplayGetFrameBuf(&top, &bw, &fmt, PSP_DISPLAY_SETBUF_IMMEDIATE);
    u32 phys = (u32)top & 0x1FFFFFFFu;
    if (!top || fmt < 0 || fmt > 3 || bw < DR_W || bw > 1024) return -1;
    u32 bytes = (u32)((DR_H - 1) * bw + DR_W) * (fmt == DR_8888 ? 4 : 2);
    if (!((phys >= 0x04000000u && phys + bytes <= 0x04200000u) || (phys >= 0x08000000u && phys + bytes <= 0x0C000000u))) return -1;
    s->base = (u8 *)(phys | 0x40000000u);       /* uncached, as Checkpoint draws */
    s->stride = bw;
    s->fmt = fmt;
    return 0;
}

/* ---- the viewer ------------------------------------------------------------ */
void view_run(const EmGame *g, const AcSet *have, const char *file_name, int file_errors)
{
    static u8 ids[EM_MAX_ID];
    static char title_buf[DR_VROWS][EM_TITLE_MAX], pts_buf[DR_VROWS][12], sub_buf[DR_VROWS][28];
    static char vval_buf[EM_MAX_SHOW][16];
    static const char *vname[EM_MAX_SHOW], *vval[EM_MAX_SHOW];
    char count[24], points[32], file2[48], build[48];
    DrRow rows[DR_VROWS];
    DrSurface s;

    if (surface(&s) < 0) { ac_log("viewer: no screen to draw on"); return; }
    u32 opened = sceKernelGetSystemTimeLow();
    g_view_open = 1;
    home_block(1);
    freeze_game();
    power_hold();

    int n = 0, got = 0, pts = 0, all_pts = 0;
    for (int id = 1; id <= EM_MAX_ID; id++) {
        if (!g->ach[id].included) continue;
        ids[n++] = (u8)id;
        all_pts += g->ach[id].points;
        if (have->has[id]) { got++; pts += g->ach[id].points; }
    }
    ac_snprintf(count, sizeof(count), "%d of %d", got, n);
    ac_snprintf(points, sizeof(points), "%d / %d pts", pts, all_pts);
    ac_snprintf(file2, sizeof(file2), file_errors ? "%d achievements, %d lines skipped" : "%d achievements", n, file_errors);
    ac_snprintf(build, sizeof(build), "%s  %s", AC_VERSION_STR, AC_BUILD_STAMP);
    for (int i = 0; i < g->nshow; i++) {
        vname[i] = g->show_name[i];
        ac_snprintf(vval_buf[i], sizeof(vval_buf[i]), "%d", (int)em_show_value(g, i));
        vval[i] = vval_buf[i];
    }
    ac_log("viewer: open, %d of %d earned", got, n);

    DrView v;
    memset(&v, 0, sizeof(v));
    v.game = g->title[0] ? g->title : g->game;
    v.count = count; v.points = points; v.got = got; v.total = n;
    v.rows = rows; v.nall = n;
    v.vname = vname; v.vval = vval; v.nval = g->nshow;
    v.ready = g->stable >= 60 ? "Yes" : "Not now (title screen or loading)";
    v.ready_ok = g->stable >= 60;
    v.file = file_name; v.file2 = file2; v.build = build;

    dr_view_dim_around(&s);
    wait_release();
    g_prev = 0;
    int cursor = 0, first = 0, page = 0;
    for (;;) {
        if (cursor >= n) cursor = n - 1;
        if (cursor < 0) cursor = 0;
        if (cursor < first) first = cursor;
        if (cursor >= first + DR_VROWS) first = cursor - DR_VROWS + 1;
        v.page = page; v.first = first; v.cursor = cursor - first;
        v.nvis = 0;
        for (int r = 0; r < DR_VROWS && first + r < n; r++) {
            int id = ids[first + r];
            const EmAch *a = &g->ach[id];
            memcpy(title_buf[r], a->title, EM_TITLE_MAX);
            ac_snprintf(pts_buf[r], sizeof(pts_buf[r]), "%d pts", a->points);
            if (have->has[id]) ac_snprintf(sub_buf[r], sizeof(sub_buf[r]), have->date[id][0] ? "Earned %s" : "Earned", have->date[id]);
            else ac_snprintf(sub_buf[r], sizeof(sub_buf[r]), "Locked");
            rows[r].title = title_buf[r]; rows[r].pts = pts_buf[r]; rows[r].sub = sub_buf[r]; rows[r].earned = have->has[id];
            v.nvis++;
        }
        if (surface(&s) >= 0) dr_view(&s, &v, msx);
        u32 b = wait_input();
        if (b & PSP_CTRL_CIRCLE) break;
        if (b & PSP_CTRL_TRIANGLE) { page ^= 1; continue; }
        if (page) continue;
        if (b & PSP_CTRL_UP) cursor--;
        else if (b & PSP_CTRL_DOWN) cursor++;
        else if (b & PSP_CTRL_LTRIGGER) { cursor -= DR_VROWS; first -= DR_VROWS; if (first < 0) first = 0; }
        else if (b & PSP_CTRL_RTRIGGER) {
            cursor += DR_VROWS;
            if (cursor < n) { first += DR_VROWS; if (first > n - DR_VROWS) first = n - DR_VROWS > 0 ? n - DR_VROWS : 0; }
        }
    }
    if (!g_slide) wait_release();
    thaw_game();
    home_block(0);
    g_toast_t0 += sceKernelGetSystemTimeLow() - opened;   /* a card that was showing carries on where it was */
    g_view_open = 0;
    ac_log("viewer: closed");
    power_done();
}
