/*
 * Emblem - the display hook. Draws the pop-up card onto each frame the game shows.
 *
 * The hook itself is Scanline 2.0's, proven on the PSP-3000 in this very
 * game: the first two instructions of sceDisplaySetFrameBuf are replaced
 * with a jump here and run from a trampoline, so it chains with Scanline's
 * own inline hook and MissyHUD's syscall-table hook in any load order.
 *
 * When the call comes:
 *  - from a thread, for the next refresh (sync = 1): draw, then pass it on;
 *  - from a thread, immediately (sync = 0): pass it on first, then draw
 *    (holding such a call back tore the picture in Scanline's tests);
 *  - from an interrupt handler or with interrupts off (Ys: The Oath in
 *    Felghana flips this way on every frame): draw nothing there, wake our
 *    own thread, which draws as soon as the interrupt is over. The card sits
 *    at the bottom of the screen, which the display reaches about 13 ms later.
 */
#include <pspkernel.h>
#include <pspintrman.h>
#include <psputils.h>
#include <pspsdk.h>
#include <string.h>
#include <systemctrl.h>
#include "common.h"
#include "draw.h"

extern u8 msx[];                                /* 8x8 font, from libpspdebug */

volatile int  g_ready;
AcToast *volatile g_toast;
volatile u32  g_toast_t0;
volatile int  g_view_open;
volatile u32  g_st_frames, g_st_intr, g_st_now, g_st_reject;
const char   *g_hook_how = "not installed";

static u32  g_target;
static u32  g_saved[2];
static u32  g_tramp[4] __attribute__((aligned(16)));
static int  g_inline;
static int  (*g_orig)(void *, int, int, int);
static volatile int g_busy;

#define J_TO(a)  (0x08000000u | (((u32)(a) >> 2) & 0x03FFFFFFu))
#define NOP      0x00000000u

static int is_branch(u32 w)
{
    u32 op = w >> 26, rs = (w >> 21) & 31, fn = w & 63;
    if (op == 0) return fn == 8 || fn == 9;
    if (op == 1) return 1;
    if (op >= 4 && op <= 7) return 1;
    if (op >= 20 && op <= 23) return 1;
    if ((op == 17 || op == 18) && rs == 8) return 1;
    if (op == 16 && fn == 0x18) return 1;
    return 0;
}
static int is_jump(u32 w) { u32 op = w >> 26; return op == 2 || op == 3; }

static int frame_ok(u32 phys, int stride, int fmt)
{
    if (fmt < 0 || fmt > 3 || stride < DR_W || stride > 1024 || (stride & 1) || (phys & 3)) return 0;
    u32 bytes = (u32)((DR_H - 1) * stride + DR_W) * (fmt == DR_8888 ? 4 : 2);
    if (phys >= 0x04000000u && phys + bytes <= 0x04200000u) return 1;
    if (phys >= 0x08000000u && phys + bytes <= 0x0C000000u) return 1;
    return 0;
}

/* Draws what there is to draw on one frame. Thread context only. */
static void do_frame(void *topaddr, int bw, int fmt)
{
    int intr = sceKernelCpuSuspendIntr();
    int mine = !g_busy;
    g_busy = 1;
    sceKernelCpuResumeIntr(intr);
    if (!mine) return;
    u32 phys = (u32)topaddr & 0x1FFFFFFFu;
    if (!frame_ok(phys, bw, fmt)) { g_st_reject++; g_busy = 0; return; }
    /* Uncached, the form Sidequest's and Checkpoint's drawing use on this PSP. */
    DrSurface s = { (u8 *)(phys | 0x40000000u), bw, fmt };
    AcToast *t = g_toast;
    if (t) {
        u32 ms = (sceKernelGetSystemTimeLow() - g_toast_t0) / 1000;
        int y = dr_card_y(t->h, ms);
        if (y < DR_H) dr_card(&s, DR_W - DR_CARD_W - 8, y, t->head, t->title, t->foot, msx);
    }
    g_st_frames++;
    g_busy = 0;
}

static int wanted(void) { return g_ready && !g_view_open && g_toast; }

/* ---- flips made from an interrupt: drawn by our thread ------------------- */
static SceUID g_evf = -1;
static int (*g_in_intr)(void);
static volatile int g_stop;                     /* the hook is gone: the drawing thread ends */
static volatile struct { void *addr; int bw, fmt; u32 seq; } g_req;
static u32 g_req_done;

void draw_pending(void)
{
    int intr = sceKernelCpuSuspendIntr();
    void *addr = g_req.addr; int bw = g_req.bw, fmt = g_req.fmt; u32 seq = g_req.seq;
    sceKernelCpuResumeIntr(intr);
    if (seq == g_req_done) return;
    g_req_done = seq;
    if (wanted()) do_frame(addr, bw, fmt);
}

static int draw_thread(SceSize args, void *argp)
{
    while (!g_stop) {
        u32 bits = 0;
        /* Woken by the hook. While a card is showing it also looks by itself every
         * millisecond; otherwise ten times a second, only to notice g_stop. */
        SceUInt timeout = wanted() ? 1000 : 100 * 1000;
        int r = sceKernelWaitEventFlag(g_evf, 1, PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &bits, &timeout);
        if (r < 0 && (u32)r != 0x800201A8u) sceKernelDelayThread(1000);
        draw_pending();
    }
    return 0;
}

static int hook_setframebuf(void *topaddr, int bufferwidth, int pixelformat, int sync)
{
    if (topaddr && wanted()) {
        int in_intr = g_in_intr ? g_in_intr() : 0;
        int intr = sceKernelCpuSuspendIntr();   /* 0: interrupts were already off */
        if ((in_intr || !intr) && g_evf >= 0) {
            g_req.addr = topaddr; g_req.bw = bufferwidth; g_req.fmt = pixelformat; g_req.seq++;
            g_st_intr++;
            sceKernelCpuResumeIntr(intr);
            u32 k1 = pspSdkSetK1(0);            /* the flag is the kernel's: refused (0x800200D1) with the game's k1 */
            sceKernelSetEventFlag(g_evf, 1);
            pspSdkSetK1(k1);
        } else {
            sceKernelCpuResumeIntr(intr);
            if (in_intr || !intr) return g_orig(topaddr, bufferwidth, pixelformat, sync);   /* no thread: never draw in an interrupt */
            if (sync == 0) {
                int r = g_orig(topaddr, bufferwidth, pixelformat, sync);
                g_st_now++;
                do_frame(topaddr, bufferwidth, pixelformat);
                return r;
            }
            do_frame(topaddr, bufferwidth, pixelformat);
        }
    }
    return g_orig(topaddr, bufferwidth, pixelformat, sync);
}

int hook_install(void)
{
    static const u32 nids[] = { 0xFE28C6D9, 0xFC4374B8 };      /* sceKernelIsIntrContext: 6.60, then old */
    for (int i = 0; i < 2 && !g_in_intr; i++)
        g_in_intr = (void *)sctrlHENFindFunction("sceInterruptManager", "InterruptManagerForKernel", nids[i]);
    g_evf = sceKernelCreateEventFlag("EmblemFlip", 0, 0, NULL);
    if (g_evf >= 0) {
        SceUID th = sceKernelCreateThread("EmblemDraw", draw_thread, 15, 0x2000, 0, NULL);
        if (th >= 0) sceKernelStartThread(th, 0, NULL);
        else g_evf = -1;
    }
    g_target = sctrlHENFindFunction("sceDisplay_Service", "sceDisplay", 0x289D82FE);
    if (!g_target) { g_hook_how = "sceDisplaySetFrameBuf not found"; return -1; }

    u32 *t = (u32 *)g_target;
    u32 w0 = t[0], w1 = t[1];
    g_saved[0] = w0; g_saved[1] = w1;
    int movable = !is_branch(w0) && !is_branch(w1) && !is_jump(w1) &&
                  ((g_target + 8) & 0xF0000000u) == (((u32)&g_tramp[2] + 4) & 0xF0000000u) &&
                  ((g_target + 4) & 0xF0000000u) == ((u32)hook_setframebuf & 0xF0000000u) &&
                  (!is_jump(w0) || ((g_target + 4) & 0xF0000000u) == (((u32)&g_tramp[0] + 4) & 0xF0000000u));
    if (movable) {
        g_tramp[0] = w0;
        g_tramp[1] = w1;
        g_tramp[2] = J_TO(g_target + 8);
        g_tramp[3] = NOP;
        g_orig = (void *)g_tramp;
        int intr = sceKernelCpuSuspendIntr();
        t[0] = J_TO(hook_setframebuf);
        t[1] = NOP;
        sctrlFlushCache();
        sceKernelCpuResumeIntr(intr);
        g_inline = 1;
        g_hook_how = is_jump(w0) ? "inline (after another plugin's jump)" : "inline";
        return 0;
    }
    g_orig = (void *)g_target;
    sctrlHENPatchSyscall((void *)g_target, hook_setframebuf);
    sctrlFlushCache();
    g_hook_how = "syscall table";
    return 1;
}

void hook_remove(void)
{
    g_stop = 1;
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
