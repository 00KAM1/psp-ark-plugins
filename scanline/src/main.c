/*
 * Scanline - CRT-style scanlines over every game, for PSP-2000/3000 on ARK.
 * Kernel plugin; load it at `game` level from SEPLUGINS/PLUGINS.TXT.
 * MIT licensed, see LICENSE.
 */
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <pspsysmem_kernel.h>
#include <pspinit.h>
#include <systemctrl_se.h>
#include <string.h>
#include "common.h"

PSP_MODULE_INFO("Scanline", 0x1007, 1, 0);

#define KEYS     (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)
#define BTN_ALL  0x0000F3F9u
#define OSD_FRAMES 180                          /* 3 s at 60 fps: time to read the second line */
#define SAVE_DELAY_US (3 * 1000 * 1000)
#define HOLD_US   (500 * 1000)                  /* a combination must be held this long */
#define REPEAT_US (400 * 1000)                  /* UP / DOWN step again while still held */

static SlConfig g_cfg;
static char g_key[SL_KEY_MAX];                  /* this game's name in games.txt */

static void publish(void)
{
    g_set.level = sl_level_from_percent(g_cfg.strength);
    g_set.spacing = g_cfg.spacing;
    g_fast = g_cfg.fast;
    g_ge_wait = g_cfg.ge_wait;
    g_defer = g_cfg.defer;
    g_mode2 = g_cfg.mode == 2;
    g_enabled = g_cfg.enabled;
    g_gen++;
}

/* Second label line: the time the scanlines take per frame in this game,
 * right now (average of the last 64 frames). The
 * text is built in the buffer not on screen, then the two are swapped. */
static void cost_line(void)
{
    static char buf[2][56];
    static int cur;
    if (!g_cfg.enabled) { g_osd_line2 = NULL; return; }
    char *b = buf[cur ^= 1];
    if (!g_now_valid) sl_snprintf(b, sizeof(buf[0]), "COST ...");
    else {
        u32 us = g_now_us;
        /* "GE n%": of the flips in the last 64 frames at which the graphics chip
         * was asked, how many found it still drawing (0 in most games; above 0
         * the plugin had to wait for it). A percentage is shown only when the
         * chip was asked and answered; otherwise the label says why not. */
        u32 ge = g_now_ge;
        /* "WAIT x.xms" (units are drawn small: see sl_osd), only when the chip was found busy: the time per frame
         * spent waiting for it, on top of COST. */
        u32 busy = g_now_busy, wait = g_now_wait;
        if (ge == GE_OK && busy) sl_snprintf(b, sizeof(buf[0]), "COST %d.%dms  GE %d%%  WAIT %d.%dms", us / 1000, us % 1000 / 100, busy,
                                             wait / 1000, wait % 1000 / 100);
        else if (ge == GE_OK) sl_snprintf(b, sizeof(buf[0]), "COST %d.%dms  GE %d%%", us / 1000, us % 1000 / 100, busy);
        else sl_snprintf(b, sizeof(buf[0]), "COST %d.%dms  GE %s", us / 1000, us % 1000 / 100,
                         ge == GE_UNASKED ? "--" : ge == GE_ERROR ? "ERR" : "N/A");
    }
    g_osd_line2 = b;
}

static void show_label(void)
{
    cost_line();
    if (!g_cfg.enabled) sl_snprintf(g_osd_text, sizeof(g_osd_text), "SCANLINES OFF FOR THIS GAME");
    else sl_snprintf(g_osd_text, sizeof(g_osd_text), "SCANLINES %s%%  1 IN %d  MODE %d",
                     sl_level_text[g_set.level], g_set.spacing, g_cfg.mode == 2 ? 2 : 1);
    g_osd_frames = OSD_FRAMES;
}

static u32 read_buttons(void)
{
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    if (sceCtrlPeekBufferPositive(&pad, 1) < 0) return 0;
    return pad.Buttons & BTN_ALL;
}

/* L + R with exactly one other button, held for half a second. Plenty of
 * games have you hold both shoulder buttons and tap a face button, so a tap
 * must not count: only a deliberate hold does. Returns the button once when
 * the hold completes; UP and DOWN then repeat while still held. */
static u32 combo(u32 now)
{
    static u32 held, since, fired;
    u32 other = now & ~KEYS;
    u32 t = sceKernelGetSystemTimeLow();
    if ((now & KEYS) != KEYS || !other || (other & (other - 1))) { held = 0; return 0; }
    if (other != held) { held = other; since = t; fired = 0; return 0; }
    if (!fired) {
        if (t - since < HOLD_US) return 0;
        fired = 1; since = t;
        return other;
    }
    if ((other & (PSP_CTRL_UP | PSP_CTRL_DOWN)) && t - since >= REPEAT_US) { since = t; return other; }
    return 0;
}

static void log_stats(const char *when)
{
    u32 n = g_st_frames;
    if (!n) { sl_log("%s: no frames yet (rejected calls %d)", when, g_st_reject); return; }
    sl_log("%s: %d frames, %d us average, %d us worst, rows done %d (%d segments), rows skipped %d, rejected %d (%s)",
           when, n, g_st_us / n, g_st_max, g_st_rows, g_st_segs, g_st_skipped, g_st_reject, g_fast ? "fast" : "slow");
    /* Was the graphics chip still drawing a frame when the game handed it over? */
    sl_log("%s: %d flips came from an interrupt (drawn by our thread instead), %d asked for the change at once (drawn straight after it; GE still busy at %d of those)",
           when, g_st_intr, g_st_now, g_st_nowbusy);
    sl_log("%s: %d passes were begun part-way down the screen", when, g_st_mid);
    sl_log("%s: GE asked %d times, answered with an error %d times", when, g_st_geasked, g_st_geerr);
    sl_log("%s: GE still drawing at %d of %d flips; waited %d us in all, %d us longest, gave up %d times (ge_wait=%d, %s)",
           when, g_st_gebusy, g_st_flips, g_st_gewait, g_st_gemax, g_st_gegiveup, g_ge_wait, hook_ge_found() ? "found" : "NOT found");
}

static int main_thread(SceSize args, void *argp)
{
    /* The Memory Stick is not always mounted this early. */
    int from_file = 0;
    for (int i = 0; i < 20; i++) {
        SceUID d = sceIoDopen("ms0:/");
        if (d >= 0) { sceIoDclose(d); break; }
        sceKernelDelayThread(250 * 1000);
    }
    from_file = config_load(&g_cfg);
    if (!from_file) config_save(&g_cfg);        /* so there is a file to edit */

    /* Which game is this, and has it settings of its own? */
    const char *iso = sctrlSEGetUmdFile();
    const char *exe = sceKernelInitFileName();
    game_key(g_key, (iso && iso[0]) ? iso : exe);
    int own = games_get(g_key, &g_cfg);
    publish();
    g_ready = 1;

    sl_log("Scanline " SL_VERSION_STR " (built " SL_BUILD_STAMP ") start: model=%d", sceKernelGetModel());
    sl_log("hook: %s at %08X (first words %08X %08X)", g_hook_how, hook_target(), hook_word(0), hook_word(1));
    sl_log("config %s: enabled=%d strength=%d spacing=%d mode=%d hotkeys=%d fast=%d",
           from_file ? "read" : "not found, defaults", g_cfg.enabled, g_cfg.strength, g_cfg.spacing,
           g_cfg.mode, g_cfg.hotkeys, g_cfg.fast);
    sl_log("game '%s': %s: enabled=%d strength=%d spacing=%d mode=%d", g_key, own ? "its own settings" : "no entry yet, config.txt's",
           g_cfg.enabled, g_cfg.strength, g_cfg.spacing, g_cfg.mode);
    sl_log("ge_wait=%d, GE status call %s", g_cfg.ge_wait, hook_ge_found() ? "found" : "NOT found");
    sl_log("interrupt check %s", hook_intr_found() ? "found" : "NOT found (going by interrupts being off)");
    sl_log("screen row call %s", hook_hcount_found() ? "found" : "NOT found (every pass starts at the top)");

    u32 dirty_since = 0;
    int first_logged = 0, logged_a = 0, logged_b = 0;
    for (;;) {
        sceKernelDelayThread(30 * 1000);
        u32 b = g_cfg.hotkeys ? combo(read_buttons()) : 0;
        if (g_osd_frames > 0) cost_line();          /* keep the figures fresh while they show */

        if (b) {
            int changed = 1;
            int lv = sl_level_from_percent(g_cfg.strength);
            if (b == PSP_CTRL_TRIANGLE)      g_cfg.enabled = !g_cfg.enabled;
            else if (b == PSP_CTRL_UP)       { if (lv < SL_LEVELS - 1) lv++; g_cfg.strength = sl_level_percent[lv]; g_cfg.enabled = 1; }
            else if (b == PSP_CTRL_DOWN)     { if (lv > 0) lv--; g_cfg.strength = sl_level_percent[lv]; g_cfg.enabled = 1; }
            else if (b == PSP_CTRL_SQUARE)   { g_cfg.mode = g_cfg.mode == 2 ? 1 : 2; g_cfg.enabled = 1; }
            else if (b == PSP_CTRL_CIRCLE)   { g_cfg.spacing = g_cfg.spacing >= 4 ? 2 : g_cfg.spacing + 1; g_cfg.enabled = 1; }
            else changed = 0;
            if (changed) {
                publish();
                show_label();
                dirty_since = sceKernelGetSystemTimeLow() | 1;
            }
        }

        /* Written once the buttons have been left alone for a few seconds. */
        if (dirty_since && sceKernelGetSystemTimeLow() - dirty_since >= SAVE_DELAY_US) {
            dirty_since = 0;
            int r = games_set(g_key, &g_cfg);
            sl_log("saved for '%s' (%d): enabled=%d strength=%d spacing=%d mode=%d", g_key, r,
                   g_cfg.enabled, g_cfg.strength, g_cfg.spacing, g_cfg.mode);
        }

        if (!first_logged && g_st_frames) {
            first_logged = 1;
            sl_log("first frame: address %08X, format %d, stride %d", g_last_addr, g_last_fmt, g_last_stride);
        }
        if (!logged_a && g_st_frames >= 600) { logged_a = 1; log_stats("after 600 frames"); }
        if (!logged_b && g_st_frames >= 6000) { logged_b = 1; log_stats("after 6000 frames"); }
    }
    return 0;
}

int module_start(SceSize args, void *argp)
{
    SceIoStat st;
    sceIoMkdir(SL_DIR, 0777);
    /* Keep the log short: past 3KB the old one becomes scanline.old. */
    if (sceIoGetstat(SL_LOG_PATH, &st) >= 0 && st.st_size > 3 * 1024) {
        sceIoRemove(SL_DIR "/scanline.old");
        if (sceIoRename(SL_LOG_PATH, SL_DIR "/scanline.old") < 0) sceIoRemove(SL_LOG_PATH);
    }
    config_defaults(&g_cfg);
    hook_install();                             /* idle until the config has been read */

    SceUID th = sceKernelCreateThread("ScanlineCtl", main_thread, 0x20, 0x4000, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    return 0;
}

int module_stop(SceSize args, void *argp)
{
    hook_remove();
    return 0;
}
