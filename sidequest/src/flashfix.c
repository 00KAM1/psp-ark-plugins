/*
 * Sidequest - keeping the PSP's internal flash readable after a load.
 *
 * The flash driver (lfatfs.prx) keeps tables in RAM of where each logical
 * block of the internal flash currently sits on the chip. Every write moves a
 * block, and the PSP rewrites its registry file each time it goes to sleep. A
 * loaded state brings back the tables from the moment it was saved, so blocks
 * that moved since then read as blank: the registry cannot be found, text
 * drawn with the system fonts disappears (font setup reads the registry), and
 * anything written through the old tables can destroy the settings storage.
 * A game restart does not help: the tables are handed from one kernel to the
 * next. Only a cold boot scans the chip.
 *
 * What Sidequest does:
 *   - flash1 (settings storage) is detached while a state is saved, so the
 *     state holds no file-system view of it;
 *   - a loaded state, as its very first step, has the driver scan the chip
 *     again with the driver's own routine, then attaches flash1.
 *
 * The scan calls code inside Sony's driver at fixed offsets, so it is tied to
 * one exact build of it: firmware 6.61, as analysed from a PSP-3000 under
 * ARK-5 on 2026-11-23. Its sizes and a checksum of its code are checked every
 * time; if anything differs, nothing is called. The driver is not always
 * loaded at the same address, so everything is an offset from its start and
 * the checksum ignores address fields.
 *
 *   +0x29C8  lock: sceKernelPowerLock(0) + lock "SceLfatfsMutex"
 *   +0x9958  build one segment (state, index, segment): fills that segment's
 *            table from the labels on the chip and lists its spare blocks
 *            again (erasing each spare block, as at a cold boot)
 *   +0xBC08  the lflash state block
 */
#include <pspkernel.h>
#include <psploadcore.h>
#include <string.h>
#include "common.h"

#define LF_TEXT     0x0000B7A8u
#define LF_DATA     0x00000304u
#define LF_BSS      0x000045BCu
#define LF_IMAGE    0x00010068u
#define LF_MCRC     0x0EAD0B9Eu        /* checksum of the code with every address field blanked */
#define LF_STATE    0xBC08u
#define LF_MUTEX_AT 0xB7C4u
#define LF_LOCK     0x29C8u
#define LF_BUILD    0x9958u
#define LF_STUB_UNLOCK   0xAF50u       /* sceKernelUnlockMutex */
#define LF_STUB_PWUNLOCK 0xB060u       /* sceKernelPowerUnlock */
#define LF_STUB_FREEFPL  0xAFB8u       /* sceKernelFreeFpl */
#define LF_SEGS     8
#define LF_LPS      0x1E0              /* logical blocks per segment */
#define LF_PPS      0x1F8              /* physical blocks per segment */

typedef struct {
    u32 index;
    u16 lbn0, pbn0, lbn1, pbn1;
    u16 *lpt;                          /* logical block -> physical block */
    u32 nfree;
    u32 *head, *tail;                  /* spare blocks: nodes of { prev, next, u16 block } */
} LfSeg;

FlashFix g_ff;

static int kram(u32 a) { return a >= 0x88000000u && a < 0x88400000u && !(a & 1); }
static int is_jump(u32 at) { return (*(u32 *)at >> 26) == 2 && *(u32 *)(at + 4) == 0; }

/* The code's checksum with everything that depends on where the module was
 * loaded blanked out: words that point into the module become offsets, jump
 * targets and 16-bit immediates are dropped. Same value at any address. */
static u32 masked_crc(u32 base)
{
    u32 crc = 0;
    for (u32 i = 0; i < LF_TEXT; i += 4) {
        u32 v = *(u32 *)(base + i);
        if (v >= base && v < base + LF_IMAGE) v -= base;
        else {
            u32 op = v >> 26;
            if (op == 2 || op == 3) v &= 0xFC000000u;
            else if (op != 0 && op != 0x1C && op != 0x1F) v &= 0xFFFF0000u;
        }
        crc = crc32_update(crc, &v, 4);
    }
    return crc;
}

/* 0 when the driver in memory is the one this code was written for.
 * *base_out is where it was found (0 if not found at all). */
int ff_check(u32 *base_out)
{
    SceModule *m = sceKernelFindModuleByName("sceLFatFs_Driver");
    if (base_out) *base_out = m ? m->text_addr : 0;
    if (!m) return -1;
    u32 base = m->text_addr;
    if (!kram(base) || (base & 3) || m->text_size != LF_TEXT || m->data_size != LF_DATA || m->bss_size != LF_BSS) return -2;
    if (masked_crc(base) != LF_MCRC) return -3;
    if (!is_jump(base + LF_STUB_UNLOCK) || !is_jump(base + LF_STUB_PWUNLOCK) || !is_jump(base + LF_STUB_FREEFPL)) return -4;
    u32 *st = (u32 *)(base + LF_STATE);
    if (st[36 / 4] != 0x20) return -5;                         /* pages per block */
    u32 *geo = (u32 *)st[60 / 4];
    if ((u32)geo < base || (u32)geo >= base + LF_IMAGE) return -6;
    if (geo[8 / 4] != LF_SEGS || geo[12 / 4] != LF_PPS || geo[16 / 4] != LF_LPS) return -7;
    LfSeg *seg = (LfSeg *)st[52 / 4];
    if (!kram((u32)seg) || ((u32)seg & 3)) return -8;
    if ((int)st[88 / 4] <= 0 || (int)*(u32 *)(base + LF_MUTEX_AT) <= 0) return -9;
    for (int i = 0; i < LF_SEGS; i++) {
        if (seg[i].index != (u32)i || seg[i].lbn0 != i * LF_LPS || seg[i].pbn0 != i * LF_PPS ||
            seg[i].lbn1 != (i + 1) * LF_LPS || seg[i].pbn1 != (i + 1) * LF_PPS) return -10;
        if (!kram((u32)seg[i].lpt) || seg[i].nfree > 64) return -11;
        if (seg[i].nfree && (!kram((u32)seg[i].head) || !kram((u32)seg[i].tail))) return -12;
    }
    return 0;
}

/* Has the driver rebuild its tables from the chip. Quiet: it runs before the
 * Memory Stick is known to be fresh, so nothing is logged here; the result is
 * kept in g_ff and written by ff_report(). Returns 0 when the tables are good. */
int ff_rescan(void)
{
    FlashFix *r = &g_ff;
    u32 base = 0;
    memset(r, 0, sizeof(*r));
    r->ran = 1;
    r->verify = ff_check(&base);
    r->base = base;
    if (r->verify) return -1;

    u32 *st = (u32 *)(base + LF_STATE);
    LfSeg *seg = (LfSeg *)st[52 / 4];
    SceUID mutex = *(SceUID *)(base + LF_MUTEX_AT);
    int (*lock)(void) = (void *)(base + LF_LOCK);
    int (*build)(void *, int, void *) = (void *)(base + LF_BUILD);
    int (*unlock)(SceUID, int) = (void *)(base + LF_STUB_UNLOCK);
    int (*pw_unlock)(int) = (void *)(base + LF_STUB_PWUNLOCK);
    int (*free_fpl)(SceUID, void *) = (void *)(base + LF_STUB_FREEFPL);
    u32 t0 = sceKernelGetSystemTimeLow();

    r->lock = lock();
    if (r->lock) return -2;                                    /* nothing touched */

    /* Walk every spare list first: one that does not look right stops
     * everything before anything is changed. */
    int bad = 0;
    for (int i = 0; i < LF_SEGS && !bad; i++) {
        u32 *n = seg[i].nfree ? seg[i].head : NULL;
        u32 count = 0;
        while (n && count < 64) { if (!kram((u32)n)) { bad = 1; break; } n = (u32 *)n[1]; count++; }
        if (n || count != seg[i].nfree) bad = 1;
    }
    if (bad) {
        r->lists_bad = 1;
        unlock(mutex, 1);
        pw_unlock(0);
        return -3;                                             /* nothing touched */
    }
    /* Give the list nodes back to the driver's pool; the scan lists the spare
     * blocks afresh. */
    for (int i = 0; i < LF_SEGS; i++) {
        u32 *n = seg[i].nfree ? seg[i].head : NULL;
        while (n) { u32 *next = (u32 *)n[1]; free_fpl((SceUID)st[88 / 4], n); n = next; }
        seg[i].nfree = 0; seg[i].head = NULL; seg[i].tail = NULL;
    }

    int rc = 0;
    for (int i = 0; i < LF_SEGS; i++) {
        u16 was[LF_LPS];                                       /* this segment's table before, to count what changed */
        memcpy(was, seg[i].lpt, sizeof(was));
        r->result = build(st, i, &seg[i]);
        r->built = i + 1;
        if (r->result) { rc = -4; break; }
        r->spares += seg[i].nfree;
        for (int k = 0; k < LF_LPS; k++) if (was[k] != seg[i].lpt[k]) r->changed++;
    }
    unlock(mutex, 1);
    pw_unlock(0);
    r->ms = (sceKernelGetSystemTimeLow() - t0) / 1000;
    r->ok = rc == 0;
    return rc;
}

void ff_report(void)
{
    FlashFix *r = &g_ff;
    if (!r->ran) return;
    if (r->verify)         sq_log("flash: re-scan NOT done, the flash driver is not the expected one (check %d, at %08X)", r->verify, r->base);
    else if (r->lock)      sq_log("flash: re-scan NOT done, the driver's lock gave %08X", r->lock);
    else if (r->lists_bad) sq_log("flash: re-scan NOT done, a spare list did not look right");
    else sq_log("flash: re-scan %s in %d ms, %d of %d segments, last result %08X, %d table entries corrected, %d spare blocks, driver at %08X",
                r->ok ? "done" : "FAILED PART-WAY", r->ms, r->built, LF_SEGS, r->result, r->changed, r->spares, r->base);
}

/* flash1 detach / attach. Quiet, for use around the sleep. */
int ff_detach(void)
{
    return sceIoUnassign("flash1:");
}

int ff_attach(void)
{
    int r = -1;
    for (int i = 0; i < 5; i++) {
        r = sceIoAssign("flash1:", "lflash0:0,1", "flashfat1:", IOASSIGN_RDWR, NULL, 0);
        if (r >= 0) break;
        sceKernelDelayThread(100 * 1000);
    }
    return r;
}
