/*
 * Sidequest - capture / restore engine.
 *
 * When a PSP suspends, every driver saves its state into RAM and the kernel
 * saves the full CPU context. To resume, scePower calls the kernel function
 * sceSuspendForKernel_B2C9640B(1), which reloads that context. That call is
 * made in two situations: when waking from real sleep, and at the end of an
 * instant "touch and go" suspend, which never powers off.
 *
 * We replace scePower's import of that function (see sq_hook_entry). At that
 * point lower RAM plus the Media Engine's save area is a complete, frozen
 * system that knows how to resume itself:
 *
 *   Capture: pack lower RAM + ME save area into free extra RAM, then let the
 *            system resume. A thread writes the packed data to a slot file.
 *
 *   Restore: with a slot's packed data in extra RAM, unpack it over lower
 *            RAM and the ME save area, then jump into the snapshot's copy of
 *            this hook. It calls the snapshot's kernel resume function, so
 *            the saved system resumes exactly as it would after a suspend.
 *
 * Nothing of the replaced system is used after the swap: the stub flushes
 * both CPU caches, so the two systems need not share a memory layout.
 */
#include <pspkernel.h>
#include <pspsysmem_kernel.h>
#include <pspsysevent.h>
#include <psppower.h>
#include <pspintrman.h>
#include <psploadcore.h>
#include <string.h>
#include <systemctrl.h>
#include "engine.h"

extern int  sq_hook_entry(int a0);
extern void sq_jumpctx(SqCtx *ctx);
extern void sq_launch(const u32 *args, void *stub);
extern char sq_stub_start[], sq_stub_end[];

enum { PH_IDLE = 0, PH_ARM_CAPTURE, PH_CAPTURED, PH_ARM_RESTORE };

/* ---- used by sq_hook_entry ---------------------------------------------- */
volatile int  g_phase = PH_IDLE;
int         (*g_orig_resume)(int);
u32           g_hk_a0, g_hk_ra, g_hk_sp;
SqCtx         g_ctx;
static u8     g_hook_stack[0x2000] __attribute__((aligned(16)));
void         *g_hook_stack_top = g_hook_stack + sizeof(g_hook_stack) - 32;

/* ---- state that travels inside every snapshot --------------------------- */
volatile u32  g_restored;
static u8     g_rv_copy[SQ_RESETVEC_SIZE] __attribute__((aligned(16)));

/* ---- capture / restore parameters and results --------------------------- */
static u32          g_stage_base, g_stage_cap;      /* KSEG0, bytes */
static u32          g_me_addr, g_me_size;           /* KSEG0, bytes */
static SqRegion     g_regions[SQ_MAX_REGIONS];      /* what a snapshot consists of */
static int          g_nregions;
static volatile int g_cap_status;                   /* 1 ok, -1 does not fit */
static volatile u32 g_cap_len;
static u32          g_rp_len, g_rp_crc, g_rp_ctx, g_rp_jumpctx, g_rp_flag;
static u32          g_launch_args[4];
static volatile int g_rp_result;

/* ---- plumbing ----------------------------------------------------------- */
static int        (*g_touch_and_go)(void);
static int          g_hook_count;
static volatile u32 g_hook_hits, g_resume_count;
static SceUID       g_evf = -1;
static volatile int g_waiting;
#define EV_RING 64
static volatile u32 g_ev_ring[EV_RING];
static volatile int g_ev_n;

#define EVF_WAKE 1

/* ------------------------------------------------------------------------ */
int engine_me_in_lower(void)
{
    u32 p = PHYS(g_me_addr);
    return g_me_size && p >= SQ_LOWER_PHYS && p + g_me_size <= SQ_LOWER_PHYS + SQ_LOWER_SIZE;
}

/* The memory areas that make up a snapshot. Set by staging_alloc(). */
void engine_set_regions(const SqRegion *r, int n)
{
    if (n > SQ_MAX_REGIONS) n = SQ_MAX_REGIONS;
    for (int i = 0; i < n; i++) g_regions[i] = r[i];
    g_nregions = n;
}

int engine_regions(SqRegion *out)
{
    for (int i = 0; i < g_nregions; i++) out[i] = g_regions[i];
    return g_nregions;
}

/* ---- hook bodies (called from sq_hook_entry, private stack, intr off) --- */
int hook_prepare(void)
{
    g_hook_hits++;
    if (g_phase == PH_ARM_CAPTURE) {
        /* Everything the restored system will need must be written before
         * sq_setctx() runs, so that it is inside the snapshot. */
        memcpy(g_rv_copy, (void *)SQ_RESETVEC, SQ_RESETVEC_SIZE);
        g_restored = 0;
        g_phase = PH_CAPTURED;
        return 1;
    }
    if (g_phase == PH_ARM_RESTORE) return 2;
    return 0;
}

void hook_capture(void)
{
    const SqRegion *reg = g_regions;
    int n = g_nregions;
    u32 *d = (u32 *)g_stage_base;
    u32 *e = d + g_stage_cap / 4;

    g_cap_status = -1;
    if (n < 1) return;
    if (g_cfg.test == 2) { g_cap_len = 0; g_cap_status = 1; return; }   /* hook test: touch nothing */
    sceKernelDcacheWritebackInvalidateAll();    /* cached reads now match RAM */
    for (int i = 0; i < n; i++) {
        if (e - d < 2) return;
        *d++ = SQ_T_SEEK;
        *d++ = reg[i].addr;
        d = sq_pack((const u32 *)reg[i].addr, reg[i].words, d, e);
        if (!d) return;
    }
    if (e - d < 1) return;
    *d++ = SQ_T_END;
    sceKernelDcacheWritebackInvalidateAll();
    g_cap_len = (u32)((u8 *)d - (u8 *)g_stage_base);
    g_cap_status = 1;

    if (g_cfg.test == 3 || g_cfg.test == 4) {
        /* Round-trip test: unpack the snapshot we just took over the very
         * system it was taken from. This exercises the unpack stub, the
         * cache flushes and the jump back in, on the real CPU, with nothing
         * else different. Continues at "second" in sq_hook_entry. */
        u32 stub = g_stage_base + g_stage_cap;
        memcpy((void *)stub, sq_stub_start, sq_stub_end - sq_stub_start);
        g_launch_args[0] = g_stage_base;
        g_launch_args[1] = (u32)&g_ctx;
        g_launch_args[2] = (u32)sq_jumpctx;
        g_launch_args[3] = (u32)&g_restored;
        sq_launch(g_launch_args, (void *)stub);
    }
}

void hook_restore(void)
{
    /* Last check with the whole system stopped: is the staged data still
     * exactly what was verified when it was loaded? If not, do nothing. */
    if (crc32_update(0, (void *)g_stage_base, g_rp_len) != g_rp_crc) {
        g_rp_result = -6;
        g_phase = PH_IDLE;
        return;
    }
    u32 stub = g_stage_base + g_stage_cap;      /* reserved page above the data */
    memcpy((void *)stub, sq_stub_start, sq_stub_end - sq_stub_start);

    g_launch_args[0] = g_stage_base;
    g_launch_args[1] = g_rp_ctx;
    g_launch_args[2] = g_rp_jumpctx;
    g_launch_args[3] = g_rp_flag;
    sq_launch(g_launch_args, (void *)stub);
}

void hook_restored(void)
{
    /* We are the snapshot now. The small SRAM the main CPU shares with the
     * Media Engine still holds the other system's values; put ours back.
     * Words 0x200..0x21F are written by the wake-up code itself: leave them. */
    u8 *rv = (u8 *)SQ_RESETVEC;
    memcpy(rv, g_rv_copy, 0x200);
    memcpy(rv + 0x220, g_rv_copy + 0x220, SQ_RESETVEC_SIZE - 0x220);
}

/* ------------------------------------------------------------------------ */
static int sysevent_handler(int ev_id, char *ev_name, void *param, int *result)
{
    if (g_ev_n < EV_RING) g_ev_ring[g_ev_n++] = (u32)ev_id;
    if (ev_id == 0x100000 && g_waiting)          /* resume phase 1: threads may run again */
        sceKernelSetEventFlag(g_evf, EVF_WAKE);
    if (ev_id == 0x400000) g_resume_count++;     /* resume completed */
    return 0;
}

static PspSysEventHandler g_sysevent = {
    .size = sizeof(PspSysEventHandler),
    .name = "SidequestEv",
    .type_mask = 0x00FFFF00,
    .handler = sysevent_handler,
};

/* ---- self-wake --------------------------------------------------------------
 * The clock alarm lives in the power controller, which stays powered while the
 * PSP sleeps, so it can end the sleep without the POWER switch. Proven on the
 * PSP-3000 with Waketest (mode 1). If the calls are missing or fail, nothing
 * changes: the PSP sleeps and waits for POWER as before. */
static int (*g_rtc_tick)(u64 *tick);
static int (*g_rtc_set_alarm)(u64 *tick, int zero);

static void wake_lookup(void)
{
    static const char *M = "sceRTC_Service";
    g_rtc_tick = (void *)sctrlHENFindFunction(M, "sceRtc", 0x3F7AD767);
    if (!g_rtc_tick) g_rtc_tick = (void *)sctrlHENFindFunction(M, "sceRtc_driver", 0x3F7AD767);
    g_rtc_set_alarm = (void *)sctrlHENFindFunction(M, "sceRtc", 0x7D1FBED3);
    if (!g_rtc_set_alarm) g_rtc_set_alarm = (void *)sctrlHENFindFunction(M, "sceRtc_driver", 0xE09880CF);
    if (!g_rtc_set_alarm) g_rtc_set_alarm = (void *)sctrlHENFindFunction(M, "sceRtc_driver", 0x7D1FBED3);
}

int engine_self_wake(void)
{
    return g_cfg.wake_seconds > 0 && !engine_instant() && g_rtc_tick && g_rtc_set_alarm;
}

static void wake_arm(void)
{
    if (!engine_self_wake()) return;
    u64 tick = 0;
    int r = g_rtc_tick(&tick);
    if (r < 0 || !tick) { sq_log("wake alarm: clock read failed (%08X)", r); return; }
    tick += (u64)g_cfg.wake_seconds * 1000000ull;
    r = g_rtc_set_alarm(&tick, 0);
    sq_log("wake alarm: set for %d s, result %08X", g_cfg.wake_seconds, r);
}

static void find_me_area(void)
{
    u32 (*get_addr)(void) = (void *)sctrlHENFindFunction("sceSystemMemoryManager", "SysMemForKernel", 0xFDC97D28);
    u32 (*get_size)(void) = (void *)sctrlHENFindFunction("sceSystemMemoryManager", "SysMemForKernel", 0xD3CA555C);
    if (get_addr && get_size) {
        g_me_addr = KSEG0(get_addr());
        g_me_size = get_size();
        sq_log("engine: ME save area %08X + %08X (kernel)", g_me_addr, g_me_size);
        return;
    }
    /* Fallback: on 64MB models it is partition 11, the top 4MB of RAM. */
    PspSysMemPartition *p = sctrlGetMemoryPartition(11);
    if (p && p->size == 0x400000 && PHYS(p->address) >= SQ_UPPER_PHYS) {
        g_me_addr = KSEG0(p->address);
        g_me_size = p->size;
        sq_log("engine: ME save area %08X + %08X (partition 11)", g_me_addr, g_me_size);
    }
}

int engine_init(void)
{
    crc32_update(0, "", 0);                      /* build the table now, not in the hook */

    g_evf = sceKernelCreateEventFlag("SidequestWake", 0, 0, NULL);
    find_me_area();

    g_touch_and_go = (void *)sctrlHENFindFunction("scePower_Service", "scePower", 0x2875994B);
    g_orig_resume = (void *)sctrlHENFindFunction("sceSystemMemoryManager", "sceSuspendForKernel", 0xB2C9640B);
    wake_lookup();
    sq_log("wake alarm: clock=%d alarm=%d, %d s", g_rtc_tick != 0, g_rtc_set_alarm != 0, g_cfg.wake_seconds);
    if (!g_orig_resume) { sq_log("engine: kernel resume function not found"); return -1; }

    SceModule *power = sceKernelFindModuleByName("scePower_Service");
    if (power && sctrlHookImportByNID(power, "sceSuspendForKernel", 0xB2C9640B, sq_hook_entry) >= 0)
        g_hook_count = 1;
    sctrlFlushCache();

    sceKernelRegisterSysEventHandler(&g_sysevent);
    sq_log("engine: resume=%08X hooked=%d instant=%08X", (u32)g_orig_resume, g_hook_count, (u32)g_touch_and_go);
    return g_hook_count ? 0 : -2;
}

int engine_ready(char *err, int errlen)
{
    if (!g_hook_count || g_evf < 0) { sq_snprintf(err, errlen, "Could not hook the suspend path (see log)"); return -1; }
    u32 p = PHYS(g_me_addr);
    int ok = engine_me_in_lower() ||
             (g_me_size && !(g_me_size & 3) && !(g_me_addr & 3) &&
              p >= SQ_UPPER_PHYS && p + g_me_size <= SQ_UPPER_END);
    if (!ok) { sq_snprintf(err, errlen, "Media Engine save area not found (see log)"); return -2; }
    return 0;
}

void engine_set_stage(u32 base, u32 bytes) { g_stage_base = base; g_stage_cap = bytes; }
u32  engine_stage_base(void)   { return g_stage_base; }
u32  engine_me_addr(void)      { return g_me_addr; }
u32  engine_me_size(void)      { return g_me_size; }
int  engine_instant(void)      { return g_touch_and_go && !g_cfg.deep_sleep; }
int  engine_hook_count(void)   { return g_hook_count; }
u32  engine_hook_hits(void)    { return g_hook_hits; }
u32  engine_resume_count(void) { return g_resume_count; }
u32  engine_addr_orig(void)    { return (u32)g_orig_resume; }
u32  engine_addr_tag(void)     { return (u32)g_touch_and_go; }

int engine_take_events(u32 *out, int max)
{
    int n = g_ev_n < max ? g_ev_n : max;
    for (int i = 0; i < n; i++) out[i] = g_ev_ring[i];
    g_ev_n = 0;
    return n;
}

void engine_fill_identity(SqSlotHeader *h)
{
    h->fw_version = sceKernelDevkitVersion();
    h->ark_version = (sctrlHENGetVersion() << 16) | (sctrlHENGetMinorVersion() & 0xFFFF);
    h->model = sceKernelGetModel();
}

/* ------------------------------------------------------------------------ */
/*
 * Ask the PSP to suspend, then wait until it is running again.
 * Our thread gets the highest priority of anything outside the kernel while
 * it waits, so on_wake() runs before any game thread does.
 */
/* The clock service keeps the alarm after it has fired and puts it back at every
 * later resume, so every ordinary sleep would end by itself too. Sony's own
 * module cancels it by passing no tick (it does so when a game exits), and so
 * do we, once awake. This runs after a load as well: the loaded snapshot was
 * taken with the alarm set. */
static void wake_cancel(void)
{
    if (!g_rtc_set_alarm || g_cfg.wake_seconds <= 0 || engine_instant()) return;
    int r = g_rtc_set_alarm(NULL, 0);
    sq_log("wake alarm: cancelled, result %08X", r);
}

static int suspend_and_wait(void (*on_wake)(void))
{
    u32 bits = 0;
    u32 before = g_resume_count;
    SceUInt timeout = engine_instant() ? 30 * 1000 * 1000 : 600 * 1000 * 1000;
    int prio = sceKernelGetThreadCurrentPriority();

    sceKernelClearEventFlag(g_evf, 0);
    g_waiting = 1;
    sceKernelChangeThreadPriority(0, 6);

    wake_arm();
    int r = engine_instant() ? g_touch_and_go() : scePowerRequestSuspend();
    if (r >= 0)
        r = sceKernelWaitEventFlag(g_evf, EVF_WAKE, PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &bits, &timeout);
    g_waiting = 0;

    if (on_wake) on_wake();
    sceKernelChangeThreadPriority(0, prio);
    if (r < 0) { wake_cancel(); return -4; }

    for (int i = 0; i < 400 && g_resume_count == before; i++)   /* up to 10s for "resume completed" */
        sceKernelDelayThread(25 * 1000);
    sceKernelDelayThread(300 * 1000);                           /* let the Memory Stick settle */
    wake_cancel();
    return 0;
}

int engine_capture(SqSlotHeader *h, u32 *stream_len, void (*on_wake)(void))
{
    u32 hits = g_hook_hits;
    g_cap_status = 0;
    if (g_cfg.test == 1) {                      /* suspend test: our hook stays idle */
        int t = suspend_and_wait(on_wake);
        return t < 0 ? -4 : 10;
    }
    g_phase = PH_ARM_CAPTURE;
    int r = suspend_and_wait(on_wake);

    if (g_restored == SQ_RESTORED_MAGIC) {      /* we woke up inside a restored snapshot */
        g_restored = 0;
        g_phase = PH_IDLE;
        return 2;
    }
    int captured = (g_phase == PH_CAPTURED && g_hook_hits != hits);
    g_phase = PH_IDLE;
    if (r < 0) return -4;
    if (!captured) return -3;
    if (g_cap_status != 1) return -5;
    if (g_cfg.test == 2) return 11;

    memset(h, 0, sizeof(*h));
    h->magic = SQ_MAGIC;
    h->version = SQ_HDR_VERSION;
    h->header_size = SQ_HDR_SIZE;
    engine_fill_identity(h);
    h->nregions = g_nregions;
    for (int i = 0; i < g_nregions; i++) {
        h->regions[i].addr = g_regions[i].addr;
        h->regions[i].words = g_regions[i].words;
    }
    h->ctx_addr = (u32)&g_ctx;
    h->jumpctx_addr = (u32)sq_jumpctx;
    h->flag_addr = (u32)&g_restored;
    *stream_len = g_cap_len;
    return 1;
}

/* ------------------------------------------------------------------------ */
static int lower_word(u32 a)
{
    return !(a & 3) && (a >> 29) == 4 && PHYS(a) >= SQ_LOWER_PHYS && PHYS(a) < SQ_LOWER_PHYS + SQ_LOWER_SIZE;
}

int engine_validate(const SqSlotHeader *h, char *err, int errlen)
{
    SqSlotHeader cur;
    memset(&cur, 0, sizeof(cur));
    engine_fill_identity(&cur);

    if (h->magic != SQ_MAGIC || h->version != SQ_HDR_VERSION || h->header_size != SQ_HDR_SIZE) {
        sq_snprintf(err, errlen, "Made by a different Sidequest version"); return -1;
    }
    if (crc32_update(0, h, (u32)((u8 *)&h->header_crc - (u8 *)h)) != h->header_crc) {
        sq_snprintf(err, errlen, "Slot header is damaged"); return -2;
    }
    if (h->fw_version != cur.fw_version || h->model != cur.model || h->ark_version != cur.ark_version) {
        sq_snprintf(err, errlen, "Saved on a different firmware, ARK or PSP"); return -3;
    }
    /* Same memory areas as this system would save (staging must be reserved). */
    int same = (int)h->nregions == g_nregions && g_nregions > 0;
    for (int i = 0; same && i < g_nregions; i++)
        same = h->regions[i].addr == g_regions[i].addr && h->regions[i].words == g_regions[i].words;
    if (!same) { sq_snprintf(err, errlen, "Saved with a different memory layout"); return -4; }
    if (!lower_word(h->ctx_addr) || !lower_word(h->jumpctx_addr) || !lower_word(h->flag_addr)) {
        sq_snprintf(err, errlen, "Slot header is damaged"); return -5;
    }
    if (h->stream_len < 12 || (h->stream_len & 3)) {
        sq_snprintf(err, errlen, "Slot header is damaged"); return -6;
    }
    return 0;
}

int engine_restore(const SqSlotHeader *h, void (*on_wake)(void))
{
    u32 hits = g_hook_hits;
    g_rp_len = h->stream_len;
    g_rp_crc = h->stream_crc;
    g_rp_ctx = h->ctx_addr;
    g_rp_jumpctx = h->jumpctx_addr;
    g_rp_flag = h->flag_addr;
    g_rp_result = 0;

    sceKernelDcacheWritebackInvalidateRange((void *)g_stage_base, g_rp_len);
    g_phase = PH_ARM_RESTORE;
    int r = suspend_and_wait(on_wake);
    g_phase = PH_IDLE;

    /* Still here, so the swap did not happen. */
    if (r < 0) return -4;
    if (g_rp_result) return g_rp_result;
    return g_hook_hits == hits ? -3 : -7;
}
