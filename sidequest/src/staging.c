/*
 * Sidequest - work out what a snapshot must contain and where to stage it.
 *
 * Extra RAM (0x0A000000..0x0BFFFFFF) is looked at partition by partition,
 * WITHOUT allocating anything:
 *   - a partition that reports all of its memory free is free;
 *   - any other partition is "busy", and so is the Media Engine's save area.
 * Busy memory belongs to the running system: it is saved in every snapshot
 * and never written to. Staging is the largest gap between busy areas.
 *
 * Earlier versions reserved the free memory with the kernel's allocator.
 * On real hardware (PSP-3000, ARK-5) that had lasting side effects: after
 * one allocation in extra RAM, exiting the game shut the PSP down, even if
 * the memory had been released again. Nothing else uses this memory while
 * ARK's Extra RAM option is off, the game is frozen whenever we use it, and
 * the free check is repeated immediately before every use.
 */
#include <pspkernel.h>
#include <pspsysmem_kernel.h>
#include <string.h>
#include <systemctrl.h>
#include "common.h"
#include "engine.h"
#include "staging.h"

#define MAX_BUSY   (SQ_MAX_REGIONS - 1)
#define MIN_STAGE  (8 * 1024 * 1024)

typedef struct { u32 lo, hi; } Range;
static int g_verbose = 1;

static u32    g_cap;                    /* staging bytes found by staging_alloc() */
static u32    g_base;                   /* KSEG0 start of staging */

int staging_supported_model(int model)
{
    /* 64MB models with a Memory Stick slot: PSP-2000 (1) and PSP-3000 (2, 3, 6, 8).
     * Not the 1000 (32MB), the Go (own pause feature, internal storage) or the Street. */
    return model == 1 || model == 2 || model == 3 || model == 6 || model == 8;
}

u32 staging_capacity(void) { return g_cap; }

void staging_free(void)        { }      /* nothing is held */
void staging_release_all(void) { }

/* Sort busy ranges and join the ones that touch or overlap (two partitions
 * can describe the same memory, as 8 and 12 do on a PSP-3000). Returns the
 * new count. Pure function, also used by the PC tests. */
int staging_merge(Range *busy, int nb)
{
    for (int i = 1; i < nb; i++)
        for (int j = i; j > 0 && busy[j].lo < busy[j - 1].lo; j--) {
            Range t = busy[j]; busy[j] = busy[j - 1]; busy[j - 1] = t;
        }
    int n = 0;
    for (int i = 0; i < nb; i++) {
        if (n && busy[i].lo <= busy[n - 1].hi) {
            if (busy[i].hi > busy[n - 1].hi) busy[n - 1].hi = busy[i].hi;
        } else {
            busy[n++] = busy[i];
        }
    }
    return n;
}

/* Pure layout step, also used by the PC tests: given the busy ranges inside
 * [lo, hi), sorted or not, return the largest free gap. */
u32 staging_largest_gap(Range *busy, int nb, u32 lo, u32 hi, u32 *gap_lo)
{
    for (int i = 1; i < nb; i++)                        /* insertion sort by start */
        for (int j = i; j > 0 && busy[j].lo < busy[j - 1].lo; j--) {
            Range t = busy[j]; busy[j] = busy[j - 1]; busy[j - 1] = t;
        }
    u32 best = 0, cur = lo;
    *gap_lo = lo;
    for (int i = 0; i <= nb; i++) {
        u32 end = (i < nb) ? busy[i].lo : hi;
        if (end > cur && end - cur > best) { best = end - cur; *gap_lo = cur; }
        if (i < nb && busy[i].hi > cur) cur = busy[i].hi;
    }
    return best;
}

int staging_alloc(char *err, int errlen)
{
    g_cap = 0;
    engine_set_stage(0, 0);
    if (engine_ready(err, errlen) < 0) return -1;

    const u32 lo_lim = SQ_UPPER_PHYS, hi_lim = SQ_UPPER_END;
    Range busy[MAX_BUSY + 4];
    int nb = 0;
    u32 me_lo = 0, me_hi = 0;
    if (!engine_me_in_lower()) {
        me_lo = PHYS(engine_me_addr());
        me_hi = me_lo + engine_me_size();
        busy[nb].lo = me_lo; busy[nb].hi = me_hi; nb++;
    }

    for (int pid = 1; pid <= 12; pid++) {
        PspSysMemPartition *p = sctrlGetMemoryPartition(pid);
        if (!p || p->size == 0) continue;
        u32 start = PHYS(p->address), end = start + p->size;
        u32 lo = start > lo_lim ? start : lo_lim;
        u32 hi = end < hi_lim ? end : hi_lim;
        /* the ME area is already accounted for */
        if (lo >= me_lo && lo < me_hi) lo = me_hi;
        if (hi > me_lo && hi <= me_hi) hi = me_lo;
        if (lo >= hi) continue;
        if (lo < me_lo && hi > me_hi) hi = me_lo;       /* straddles it: keep the lower part */

        if (pid == 2) {
            sq_log("staging: game partition reaches %08X..%08X", lo, hi);
            sq_snprintf(err, errlen, "Set ARK 'Extra RAM' to Off for this game");
            return -2;
        }
        u32 maxfree = sceKernelPartitionMaxFreeMemSize(pid);
        if (maxfree >= p->size) continue;               /* completely free */

        /* Something lives here: it becomes part of the snapshot. */
        if (nb >= MAX_BUSY + 4) goto too_many;
        busy[nb].lo = lo; busy[nb].hi = hi; nb++;
        if (g_verbose) sq_log("staging: partition %d %08X..%08X in use (max free %08X)", pid, lo, hi, maxfree);
    }

    nb = staging_merge(busy, nb);
    if (nb > MAX_BUSY) goto too_many;
    u32 gap_lo;
    u32 gap = staging_largest_gap(busy, nb, lo_lim, hi_lim, &gap_lo);
    if (gap < MIN_STAGE) {
        sq_log("staging: largest free gap is only %08X bytes at %08X", gap, gap_lo);
        /* put the numbers on screen: a photo of the menu is then enough */
        sq_snprintf(err, errlen, "No room: gap %dK, P9 max %dK, total %dK", gap >> 10,
                    sceKernelPartitionMaxFreeMemSize(9) >> 10, sceKernelPartitionTotalFreeMemSize(9) >> 10);
        return -3;
    }

    SqRegion reg[SQ_MAX_REGIONS];
    reg[0].addr = KSEG0(SQ_LOWER_PHYS);
    reg[0].words = SQ_LOWER_SIZE / 4;
    for (int i = 0; i < nb; i++) {                      /* busy[] is sorted now */
        reg[1 + i].addr = KSEG0(busy[i].lo);
        reg[1 + i].words = (busy[i].hi - busy[i].lo) / 4;
    }
    engine_set_regions(reg, 1 + nb);
    g_cap = gap - SQ_STUB_SIZE;
    g_base = KSEG0(gap_lo);
    engine_set_stage(g_base, g_cap);
    g_verbose = 0;                                      /* log the partition details once */
    return 0;

too_many:
    sq_log("staging: extra RAM is too fragmented");
    sq_snprintf(err, errlen, "Extra RAM is too fragmented to use");
    return -4;
}

void staging_describe(void)
{
    SqRegion reg[SQ_MAX_REGIONS];
    int n = engine_regions(reg);
    for (int i = 0; i < n; i++)
        sq_log("layout: snapshot area %d = %08X + %08X", i, reg[i].addr, reg[i].words * 4);
    sq_log("layout: staging holds %08X bytes", g_cap);
}

void *staging_ptr(void) { return (void *)g_base; }
