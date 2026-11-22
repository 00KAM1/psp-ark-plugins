/*
 * Sidequest - snapshot packing.
 *
 * The packed stream is a list of 32-bit records:
 *   SQ_T_SEEK        , addr          set the write position
 *   SQ_T_FILL | n    , value         write `value` n times
 *   SQ_T_LIT  | n    , n words       copy n words
 *   SQ_T_END
 * Runs of 3 or more equal words become FILL records, everything else is
 * copied. That is enough to drop empty memory, and it keeps the unpacker
 * (hand-written assembly that runs while RAM is being replaced) tiny.
 *
 * This file has no PSP dependencies so it can be tested on a PC.
 */
#include "common.h"

#define MIN_RUN 3

/* Length of the run of equal words starting at p (at least 1). */
static inline u32 run_len(const u32 *p, const u32 *end)
{
    const u32 v = *p;
    const u32 *q = p + 1;
    while (q < end && *q == v) q++;
    return (u32)(q - p);
}

/*
 * Pack `words` words from src into dst. Returns the new dst, or NULL if the
 * output would pass dst_end (nothing past dst_end is ever written).
 */
u32 *sq_pack(const u32 *src, u32 words, u32 *dst, u32 *dst_end)
{
    const u32 *end = src + words;
    while (src < end) {
        u32 r = run_len(src, end);
        if (r >= MIN_RUN) {
            if (dst_end - dst < 2) return 0;
            *dst++ = SQ_T_FILL | r;
            *dst++ = *src;
            src += r;
            continue;
        }
        /* literal run: until the next run of MIN_RUN or the end */
        if (dst_end - dst < 2) return 0;
        u32 *head = dst++;
        u32 n = 0;
        while (src < end) {
            r = run_len(src, end);
            if (r >= MIN_RUN) break;
            if ((u32)(dst_end - dst) < r) return 0;
            for (u32 i = 0; i < r; i++) *dst++ = *src++;
            n += r;
        }
        *head = SQ_T_LIT | n;
    }
    return dst;
}

/* Same walk as sq_pack, but only counts the output words. */
u32 sq_pack_measure(const u32 *src, u32 words)
{
    const u32 *end = src + words;
    u32 out = 0;
    while (src < end) {
        u32 r = run_len(src, end);
        if (r >= MIN_RUN) { out += 2; src += r; continue; }
        out += 1;
        while (src < end) {
            r = run_len(src, end);
            if (r >= MIN_RUN) break;
            out += r; src += r;
        }
    }
    return out;
}

/*
 * Check that a stream writes exactly the given regions, in order, each one
 * completely and nothing else, and ends exactly at len_bytes. The unpacker
 * trusts the stream, so nothing is unpacked unless this returns 0.
 */
int sq_stream_validate(const u32 *s, u32 len_bytes, const SqRegion *regions, int nregions)
{
    if (len_bytes & 3) return -1;
    const u32 *end = s + len_bytes / 4;

    for (int i = 0; i < nregions; i++) {
        if (end - s < 2) return -2;
        if (s[0] != SQ_T_SEEK || s[1] != regions[i].addr) return -3;
        s += 2;
        u32 left = regions[i].words;
        while (left) {
            if (end - s < 2) return -4;
            u32 tag = *s & SQ_T_MASK, n = *s & SQ_N_MASK;
            if (n == 0 || n > left) return -5;
            if (tag == SQ_T_FILL) {
                s += 2;
            } else if (tag == SQ_T_LIT) {
                if ((u32)(end - s - 1) < n) return -6;
                s += 1 + n;
            } else {
                return -7;
            }
            left -= n;
        }
    }
    if (end - s != 1 || *s != SQ_T_END) return -8;
    return 0;
}
