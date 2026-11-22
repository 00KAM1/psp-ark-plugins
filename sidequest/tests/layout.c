#include "../src/common.h"
typedef struct { u32 lo, hi; } Range;
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
