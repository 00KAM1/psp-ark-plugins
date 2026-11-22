/*
 * Scanline - the display hook.
 *
 * Every frame a game shows goes through sceDisplaySetFrameBuf(). We darken
 * the rows of the frame being handed over just before the original runs.
 *
 * How the hook is placed. MissyHUD (and most plugins) take over the system
 * call table entry with sctrlHENPatchSyscall(). That call only replaces an
 * entry that still holds the original address, and says nothing when it
 * doesn't, so two plugins doing it on the same function means one of them
 * silently loses, depending on load order. Instead we patch the first two
 * instructions of the function itself with a jump to our code, and run those
 * two instructions from a small trampoline. A syscall-table hook loaded before
 * or after us ends up calling the original address, which now leads through
 * us, so both keep working whatever the order. If the function's first two
 * instructions can't be moved (a PC-relative branch), we fall back to the
 * syscall table and say so in the log.
 */
#include <pspkernel.h>
#include <pspintrman.h>
#include <psputils.h>
#include <pspsdk.h>
#include <string.h>
#include <systemctrl.h>
#include "common.h"
#include "lines.h"

extern u8 msx[];                                /* 8x8 font, from libpspdebug */

/* ---- shared with main.c --------------------------------------------------- */
volatile int  g_ready;                          /* config read: frames may be touched */
volatile int  g_enabled = 1;
volatile u32  g_gen;                            /* bumped on every settings change */
volatile int  g_osd_frames;                     /* frames left to show the label */
char          g_osd_text[40];
const char *volatile g_osd_line2;                /* second label line, or 0 */
volatile u32  g_now_us, g_now_busy, g_now_wait, g_now_intr, g_now_valid;   /* the last 64 frames: see process() */
volatile u32  g_now_ge, g_st_geasked, g_st_geerr;
SlSettings    g_set = { 1, 2 };
int           g_fast = 1;                       /* 1: cached passes, 0: uncached (for comparison) */
int           g_ge_wait = 1;                    /* 1: let the GE finish the frame first */
volatile u32  g_st_flips, g_st_gebusy, g_st_gewait, g_st_gemax, g_st_gegiveup;

volatile u32  g_st_frames, g_st_us, g_st_max, g_st_rows, g_st_skipped, g_st_segs, g_st_reject;
volatile u32  g_last_addr, g_last_fmt, g_last_stride;
const char   *g_hook_how = "not installed";

/* ---- hook plumbing -------------------------------------------------------- */
static u32  g_target;                           /* kernel address of sceDisplaySetFrameBuf */
static u32  g_saved[2];
static u32  g_tramp[4] __attribute__((aligned(16)));
static int  g_inline;
static int  (*g_orig)(void *, int, int, int);
static SlState g_state;
static u32  g_seen_gen = 0xFFFFFFFFu;
static volatile int g_busy;

#define J_TO(a)  (0x08000000u | (((u32)(a) >> 2) & 0x03FFFFFFu))
#define NOP      0x00000000u

/* 1 if the instruction changes the flow of control with a PC-relative target
 * (branches), or is a register jump. J and JAL are absolute within the same
 * 256MB region and may be copied. */
static int is_branch(u32 w)
{
    u32 op = w >> 26, rs = (w >> 21) & 31, fn = w & 63;
    if (op == 0) return fn == 8 || fn == 9;                     /* jr, jalr */
    if (op == 1) return 1;                                      /* bltz, bgez, ... */
    if (op >= 4 && op <= 7) return 1;                           /* beq, bne, blez, bgtz */
    if (op >= 20 && op <= 23) return 1;                         /* the "likely" forms */
    if ((op == 17 || op == 18) && rs == 8) return 1;            /* FPU / VFPU branches */
    if (op == 16 && fn == 0x18) return 1;                       /* eret */
    return 0;
}
static int is_jump(u32 w) { u32 op = w >> 26; return op == 2 || op == 3; }

/* A few cheap checks so a strange call never makes us touch memory we
 * shouldn't. The frame must lie whole inside the 2MB of VRAM or inside main
 * RAM. The VRAM mirrors above 0x04200000 are refused: they are not plain
 * copies (the GE uses them for other views of the same memory), and no game
 * is known to display from one. */
static int frame_ok(u32 phys, int stride, int fmt)
{
    if (fmt < 0 || fmt > 3 || stride < SL_W || stride > 1024 || (stride & 1) || (phys & 3)) return 0;
    u32 bytes = (u32)((SL_H - 1) * stride + SL_W) * (fmt == SL_8888 ? 4 : 2);
    if (phys >= 0x04000000u && phys + bytes <= 0x04200000u) return 1;
    if (phys >= 0x08000000u && phys + bytes <= 0x0C000000u) return 1;
    return 0;
}

/* ---- waiting for the graphics chip ------------------------------------------
 * A game builds a frame by handing the GE lists of drawing commands, which
 * the GE works through on its own. Most games wait for it to finish before
 * they show the frame; some hand the frame over while the GE is still at it,
 * counting on it being done by the next screen refresh. Rows we darken there
 * are painted over by whatever the GE draws afterwards, differently every
 * frame: flicker. So ask the GE where it is, and if it is still drawing,
 * sleep in short steps until it is done.
 *
 * Only the "where are you" form of sceGeDrawSync is used (it returns at
 * once). Its waiting form is not: a game that has left a list paused on
 * purpose would never come back from it. For the same reason a paused list
 * is not waited for, and the wait as a whole gives up after 12 ms. */
#define GE_QUEUED   1
#define GE_DRAWING  2
#define GE_STEP_US  250
#define GE_MAX_US   12000
#define GE_SPIN_US  600
static int (*g_ge_sync)(int);

int hook_ge_found(void) { return g_ge_sync != 0; }

static void ge_lookup(void)
{
    g_ge_sync = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_user", 0xB287BD61);
    if (!g_ge_sync) g_ge_sync = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0xB287BD61);
}

/* `quick`: the frame is already on screen, so every fraction of a millisecond
 * counts. Ask again and again for up to 0.6 ms before falling back to sleeping
 * in steps (a sleep is 250 us at the very least). */
static void ge_wait(int quick)
{
    g_st_flips++;
    if (!g_ge_wait || !g_ge_sync) return;
    int s = g_ge_sync(1);
    /* A proper answer is a list state, 0 to 4. Anything else (an error code)
     * is counted apart, so the label does not show a failed call as "0%". */
    if (s < 0 || s > 4) { g_st_geerr++; return; }
    g_st_geasked++;
    if (s != GE_QUEUED && s != GE_DRAWING) return;
    g_st_gebusy++;
    if (quick) g_st_nowbusy++;
    u32 t0 = sceKernelGetSystemTimeLow(), dt = 0;
    for (;;) {
        if (!quick || dt >= GE_SPIN_US) {
            if (sceKernelDelayThread(GE_STEP_US) < 0) { g_st_gegiveup++; break; }   /* can't sleep here: don't spin */
        }
        dt = sceKernelGetSystemTimeLow() - t0;
        s = g_ge_sync(1);
        if (s != GE_QUEUED && s != GE_DRAWING) break;
        if (dt >= GE_MAX_US) { g_st_gegiveup++; break; }
    }
    g_st_gewait += dt;
    if (dt > g_st_gemax) g_st_gemax = dt;
}

/* Write back and drop from the data cache `rows` rows starting at row y:
 * `bytes` of each, rows `pitch` bytes apart. Whole 64-byte cache lines. */
static void flush_rows(u8 *base, int pitch, int y, int rows, int bytes)
{
    u32 a = ((u32)base + (u32)(y * pitch)) & ~63u;
    u32 e = ((u32)base + (u32)((y + rows - 1) * pitch + bytes) + 63u) & ~63u;
    sceKernelDcacheWritebackInvalidateRange((void *)a, e - a);
}

/* Which row the screen is showing (0 to 271; 272 and up is the blank period).
 * Looked up at start like the others; without it every pass starts at the top. */
static int (*g_hcount)(void);
volatile u32 g_st_mid;                          /* passes begun part-way down */
int hook_hcount_found(void) { return g_hcount != 0; }

#define BAND 40
#define LEAD 6                                  /* rows of head start over the screen */

static void osd(u8 *base, int bw, int fmt)
{
    const char *line2 = g_osd_line2;
    sl_osd_corner(base, bw, fmt, 0, g_osd_text, msx);
    if (line2) sl_osd_corner(base, bw, fmt, 1, line2, msx);
}

/* How a frame reaches process(). */
#define F_WAIT_GE   1                           /* let the GE finish it first */
#define F_ON_SCREEN 2                           /* it is being shown already */

static void process(void *topaddr, int bw, int fmt, int how)
{
    u32 phys = (u32)topaddr & 0x1FFFFFFFu;
    if (!frame_ok(phys, bw, fmt)) { g_st_reject++; return; }
    /* A frame handed over from an interrupt was finished when it was shown;
     * whatever the GE is doing by now belongs to the next frame, and waiting
     * for that would only make the lines late: no wait there.
     * An immediate flip from the game's own thread is different. The game has
     * not moved on, so a GE still drawing is still drawing *this* frame, and
     * whatever it paints after us wipes our lines out for as long as the frame
     * is shown (seen on the PSP in a Jeanne d'Arc video, build 10:31:25, which
     * did not wait there: a piece of one line gone for one game frame). */
    if (how & F_WAIT_GE) ge_wait(how & F_ON_SCREEN); else g_st_flips++;
    g_last_addr = phys; g_last_fmt = (u32)fmt; g_last_stride = (u32)bw;

    int fresh = g_gen != g_seen_gen;
    if (fresh) { sl_forget(&g_state); g_seen_gen = g_gen; }
    u32 t0 = sceKernelGetSystemTimeLow();
    SlStats st = { 0, 0, 0 };
    if (g_fast) {
        /* Cached: rows are read in 64-byte lines instead of one slow word at a
         * time. The cache must not hold anything stale about the rows we read
         * (the GE drew them behind the CPU's back), and what we wrote must be
         * in memory before the display reads it.
         *
         * Only those rows are flushed. Up to build 08:40:38 the whole data
         * cache was emptied, twice a frame, and on the PSP that made the
         * game's sound pop: the popping stayed the same when the amount of
         * drawing was halved (1 in 4), which left the per-frame flushes. The
         * rest of the cache is the game's and the system's business. */
        u8 *base = (u8 *)(phys | 0x80000000u);
        int bpp = fmt == SL_8888 ? 4 : 2;
        int spacing = g_set.spacing < 2 ? 2 : g_set.spacing > 4 ? 4 : g_set.spacing;
        /* From the top, in bands of 40 rows, each flushed, darkened and
         * flushed again before the next is touched. When the frame is already
         * on its way to the screen (a flip from an interrupt) the top rows
         * are shown within a millisecond, so they must be finished first, and
         * the label (rows 7 to 36) straight after its band. */
        /* The label goes on first, before any darkening. Its top line sits in
         * the very first rows, which the screen shows within a millisecond of
         * a flip made from an interrupt: drawn only after the first band (as
         * up to build 09:56:48) that line came too late on some frames and
         * flickered, while the line below it was in time. It is drawn again,
         * clean, after the first band has been darkened. */
        if (g_osd_frames > 0) {
            flush_rows(base, bw * bpp, 0, 30, SL_W * bpp);
            osd(base, bw, fmt);
            flush_rows(base, bw * bpp, 0, 30, SL_W * bpp);
        }
        /* Where to begin. A frame darkened before it is shown is simply done
         * from the top. One that is already on screen (F_ON_SCREEN: a flip
         * from an interrupt, or an immediate one) is being shown row by row
         * right now, so the rows the screen is about to reach come first: from
         * a little below the row it is on, down to the bottom, and then the
         * rows above, which it will not show again until the next refresh.
         * The pass moves about five times faster than the screen does. */
        int band = BAND;
        int start = 0;
        if ((how & F_ON_SCREEN) && g_hcount) {
            int h = g_hcount();
            if (h >= 16 && h < SL_H) { start = h + LEAD < SL_H ? h + LEAD : SL_H; g_st_mid++; }
        }
        for (int part = 0; part < 2; part++) {
            int from = part == 0 ? start : 0, to = part == 0 ? SL_H : start;
            for (int y0 = from; y0 < to; y0 += band) {
                int y1 = y0 + band < to ? y0 + band : to;
                int first = y0 + ((spacing - 1 - y0) % spacing + spacing) % spacing;     /* first dark row at or below y0 */
                for (int y = first; y < y1; y += spacing) flush_rows(base, bw * bpp, y, 1, SL_W * bpp);
                sl_frame_rows(base, phys, bw, fmt, &g_set, g_gen, &g_state, &st, y0, y1);
                for (int y = first; y < y1; y += spacing) flush_rows(base, bw * bpp, y, 1, SL_W * bpp);
                if (y0 < 30 && g_osd_frames > 0) {              /* a band that touched label rows: the clean label again */
                    osd(base, bw, fmt);
                    flush_rows(base, bw * bpp, 0, 30, SL_W * bpp);
                }
            }
        }
    } else {
        /* Uncached, by the same address form Sidequest's drawing has used on
         * the real PSP-3000 (0x44000000 for VRAM). */
        sl_frame((u8 *)(phys | 0x40000000u), phys, bw, fmt, &g_set, g_gen, &g_state, &st);
        if (g_osd_frames > 0) osd((u8 *)(phys | 0x40000000u), bw, fmt);
    }
    u32 dt = sceKernelGetSystemTimeLow() - t0;
    g_st_frames++; g_st_us += dt; if (dt > g_st_max) g_st_max = dt;

    /* Figures for the label: averages over the last 64 frames. */
    static u32 w_n, w_us, w_busy0, w_wait0, w_intr0, w_asked0, w_err0;
    if (fresh) { w_n = 0; w_us = 0; w_busy0 = g_st_gebusy; w_wait0 = g_st_gewait; w_intr0 = g_st_intr; w_asked0 = g_st_geasked; w_err0 = g_st_geerr; g_now_valid = 0; }
    w_us += dt;
    if (++w_n == 64) {
        g_now_us = w_us / 64;
        u32 asked = g_st_geasked - w_asked0, err = g_st_geerr - w_err0;
        g_now_busy = asked ? (g_st_gebusy - w_busy0) * 100 / asked : 0;
        g_now_ge = !g_ge_sync ? GE_MISSING : asked ? GE_OK : err ? GE_ERROR : GE_UNASKED;
        g_now_wait = (g_st_gewait - w_wait0) / 64;
        g_now_intr = (g_st_intr - w_intr0) * 100 / 64; if (g_now_intr > 100) g_now_intr = 100;
        g_now_valid = 1;
        w_n = 0; w_us = 0; w_busy0 = g_st_gebusy; w_wait0 = g_st_gewait; w_intr0 = g_st_intr; w_asked0 = g_st_geasked; w_err0 = g_st_geerr;
    }
    g_st_rows += st.rows_done; g_st_skipped += st.rows_skipped; g_st_segs += st.segs_done;
}

/* One frame: scanlines, or just the "off" label. Thread context only. */
static void do_frame(void *topaddr, int bufferwidth, int pixelformat, int how)
{
    int intr = sceKernelCpuSuspendIntr();
    int mine = !g_busy;
    g_busy = 1;
    sceKernelCpuResumeIntr(intr);
    if (!mine) return;
    if (g_enabled) process(topaddr, bufferwidth, pixelformat, how);
    else if (g_osd_frames > 0 && frame_ok((u32)topaddr & 0x1FFFFFFFu, bufferwidth, pixelformat)) {
        u32 phys = (u32)topaddr & 0x1FFFFFFFu;        /* "off" label only */
        sl_osd_corner((u8 *)(phys | 0x40000000u), bufferwidth, pixelformat, 0, g_osd_text, msx);
    }
    if (g_osd_frames > 0) g_osd_frames--;
    g_busy = 0;
}

/* ---- flips made from an interrupt ----------------------------------------------
 * Some games change the displayed buffer from their vertical-blank interrupt
 * handler (Ys: The Oath in Felghana is one, by every sign). Nothing else
 * runs while an interrupt handler does: not other interrupts, not sound. A
 * pass of 2.7 ms there made that game's sound pop once a frame, the same at
 * half the work, and for longer when the pass was made slower.
 *
 * So no work is done there. The flip is noted, a thread of ours is woken, and
 * the handler returns at once. The thread runs as soon as the interrupt is
 * over, ahead of anything of the game's, with interrupts on. The interrupt
 * comes at the start of the blank period before the screen is drawn from the
 * top, and the pass goes down the picture several times faster than the
 * screen does, so the lines are in place before each row is shown. */
static SceUID g_evf = -1;

/* "Am I in an interrupt handler?" is looked up when the plugin starts, never
 * linked in. Build 09:08:58 linked it in, and that build did not show up in
 * the game at all: a module that asks at load time for a function the
 * system does not have under that number is not loaded, and this was the
 * only function in it that no plugin of ours had asked for before. If it is
 * not found, the other sign is used alone: interrupts being off on entry,
 * which they are inside a handler. */
static int (*g_in_intr)(void);

static void intr_lookup(void)
{
    static const u32 nids[] = { 0xFE28C6D9, 0xFC4374B8 };      /* pspsdk's 6.60 number, then the old one */
    for (int i = 0; i < 2 && !g_in_intr; i++)
        g_in_intr = (void *)sctrlHENFindFunction("sceInterruptManager", "InterruptManagerForKernel", nids[i]);
    g_hcount = (void *)sctrlHENFindFunction("sceDisplay_Service", "sceDisplay", 0x773DD3A3);     /* sceDisplayGetCurrentHcount */
    if (!g_hcount) g_hcount = (void *)sctrlHENFindFunction("sceDisplay_Service", "sceDisplay_driver", 0x773DD3A3);
}

int hook_intr_found(void) { return g_in_intr != 0; }
static volatile struct { void *addr; int bw, fmt; u32 seq; } g_req;
static u32 g_req_done;
volatile u32 g_st_intr, g_st_now, g_st_nowbusy;
int g_defer = 1;                                /* 0: work inside the interrupt after all (sound pops) */

/* Mode 2, chosen per game with L+R+SQUARE: the lines always go on before the
 * game's call is passed on, wherever the call comes from. The frame is then
 * complete when it is first shown, top rows and label included, which mode 1
 * cannot promise in a game that swaps late and is heavy to draw (seen on the
 * PSP: a 59 fps game at 7.1 ms, and still at 4.5 ms, with the label's top
 * line cut in every frame). The price is the one mode 1 exists to avoid: the
 * swap is held back for the length of the pass (tearing in a game that swaps
 * immediately), or the pass runs inside the game's interrupt (sound pops). */
int g_mode2;

void draw_pending(void)                         /* not static: the emulator test calls it */
{
    int intr = sceKernelCpuSuspendIntr();
    void *addr = g_req.addr; int bw = g_req.bw, fmt = g_req.fmt; u32 seq = g_req.seq;
    sceKernelCpuResumeIntr(intr);
    if (seq == g_req_done) return;
    g_req_done = seq;
    do_frame(addr, bw, fmt, F_ON_SCREEN);
}

static int draw_thread(SceSize args, void *argp)
{
    for (;;) {
        u32 bits = 0;
        /* Woken by the flip; and, in case that wake-up does not arrive from
         * where the game flips, it also looks by itself every millisecond.
         * A look that finds nothing new costs a few instructions. */
        SceUInt timeout = 1000;
        int r = sceKernelWaitEventFlag(g_evf, 1, PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &bits, &timeout);
        if (r < 0 && (u32)r != 0x800201A8u) sceKernelDelayThread(1000);     /* not a plain timeout: never spin */
        draw_pending();
    }
    return 0;
}

static int hook_setframebuf(void *topaddr, int bufferwidth, int pixelformat, int sync)
{
    if (g_ready && topaddr && (g_enabled || g_osd_frames > 0)) {
        int in_intr = g_in_intr ? g_in_intr() : 0;
        int intr = sceKernelCpuSuspendIntr();   /* 0: interrupts were already off */
        int pre = g_mode2;
        if ((in_intr || !intr) && g_defer && !pre && g_evf >= 0) {
            g_req.addr = topaddr; g_req.bw = bufferwidth; g_req.fmt = pixelformat; g_req.seq++;
            g_st_intr++;
            sceKernelCpuResumeIntr(intr);
            /* The flag is the kernel's. This call arrives marked as coming from
             * the game (it is the game's system call we are inside), and the
             * system refused it: 0x800200D1, no permission, on every flip (seen
             * on the PSP in build 09:29:01). Marked as the kernel's own for the length of the call. */
            u32 k1 = pspSdkSetK1(0);
            sceKernelSetEventFlag(g_evf, 1);
            pspSdkSetK1(k1);
        } else {
            int off = in_intr || !intr;
            if (off) g_st_intr++;
            sceKernelCpuResumeIntr(intr);
            if (!off && sync == 0 && !pre) {
                /* "Change the picture now", not "at the next refresh" (sync 0 is
                 * PSP_DISPLAY_SETBUF_IMMEDIATE). A game that asks for this has
                 * just waited for the refresh itself, so the change must go
                 * through at once: held back for the 3 ms of our pass it lands
                 * with the screen already some forty rows down, and the picture
                 * tears there (seen on the PSP in Jeanne d'Arc's videos, builds
                 * up to 10:03:09). So: the game's call first, the lines straight
                 * after, from the top, ahead of the screen as for a flip made
                 * from an interrupt. The GE is asked first, quickly: see process(). */
                int r = g_orig(topaddr, bufferwidth, pixelformat, sync);
                g_st_now++;
                do_frame(topaddr, bufferwidth, pixelformat, F_WAIT_GE | F_ON_SCREEN);
                return r;
            }
            do_frame(topaddr, bufferwidth, pixelformat, off ? 0 : F_WAIT_GE);
        }
    }
    return g_orig(topaddr, bufferwidth, pixelformat, sync);
}

int hook_install(void)
{
    ge_lookup();
    intr_lookup();
    g_evf = sceKernelCreateEventFlag("ScanlineFlip", 0, 0, NULL);
    if (g_evf >= 0) {
        /* 15: ahead of every game thread (those start at 16), behind the system's own. */
        SceUID th = sceKernelCreateThread("ScanlineDraw", draw_thread, 15, 0x4000, 0, NULL);
        if (th >= 0) sceKernelStartThread(th, 0, NULL);
    }
    g_target = sctrlHENFindFunction("sceDisplay_Service", "sceDisplay", 0x289D82FE);
    if (!g_target) { g_hook_how = "sceDisplaySetFrameBuf not found"; return -1; }

    u32 *t = (u32 *)g_target;
    u32 w0 = t[0], w1 = t[1];
    g_saved[0] = w0; g_saved[1] = w1;           /* kept for the log whichever way we hook */
    int movable = !is_branch(w0) && !is_branch(w1) && !is_jump(w1) &&
                  ((g_target + 8) & 0xF0000000u) == (((u32)&g_tramp[2] + 4) & 0xF0000000u) &&
                  ((g_target + 4) & 0xF0000000u) == ((u32)hook_setframebuf & 0xF0000000u) &&
                  (!is_jump(w0) || ((g_target + 4) & 0xF0000000u) == (((u32)&g_tramp[0] + 4) & 0xF0000000u));
    if (movable) {
        g_tramp[0] = w0;
        g_tramp[1] = w1;
        g_tramp[2] = J_TO(g_target + 8);        /* not reached if w0 is a J: it never comes back */
        g_tramp[3] = NOP;
        g_orig = (void *)g_tramp;

        int intr = sceKernelCpuSuspendIntr();
        t[0] = J_TO(hook_setframebuf);
        t[1] = NOP;
        sctrlFlushCache();                      /* both caches, trampoline included */
        sceKernelCpuResumeIntr(intr);
        g_inline = 1;
        g_hook_how = is_jump(w0) ? "inline (after another plugin's jump)" : "inline";
        return 0;
    }

    /* Can't move those instructions: take the syscall table entry instead. */
    g_orig = (void *)g_target;
    sctrlHENPatchSyscall((void *)g_target, hook_setframebuf);
    sctrlFlushCache();
    g_hook_how = "syscall table (MissyHUD loaded later would replace it)";
    return 1;
}

void hook_remove(void)
{
    if (!g_inline) return;
    u32 *t = (u32 *)g_target;
    int intr = sceKernelCpuSuspendIntr();
    if (t[0] == J_TO(hook_setframebuf) && t[1] == NOP) {
        t[0] = g_saved[0];
        t[1] = g_saved[1];
        sctrlFlushCache();
    }
    sceKernelCpuResumeIntr(intr);
    g_inline = 0;
}

u32 hook_target(void) { return g_target; }
u32 hook_word(int i) { return g_saved[i & 1]; }
