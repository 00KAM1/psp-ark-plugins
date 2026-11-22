/*
 * Reading UTF-8 and looking up 8x8 shapes for Japanese text. Every character,
 * English or Japanese, takes one 8-dot cell on screen, so layouts count cells.
 * No system calls: the same file is compiled and tested on a PC.
 */
#include "jtext.h"

unsigned jt_next(const char **sp)
{
    const unsigned char *s = (const unsigned char *)*sp;
    unsigned c = s[0];
    int n = c < 0x80 ? 1 : c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    if (!c) return 0;
    if (n == 1) { *sp += 1; return c; }
    for (int i = 1; i < n; i++) if ((s[i] & 0xC0) != 0x80) n = 0;
    if ((n == 3 && c == 0xE0 && s[1] < 0xA0) || (n == 3 && c == 0xED && s[1] > 0x9F) ||
        (n == 4 && c == 0xF0 && s[1] < 0x90) || (n == 4 && c == 0xF4 && s[1] > 0x8F)) n = 0;
    if (!n) { *sp += 1; return '?'; }
    unsigned cp = n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
    for (int i = 1; i < n; i++) cp = (cp << 6) | (s[i] & 0x3F);
    *sp += n;
    return cp;
}

static int find(const unsigned short *t, int n, unsigned cp)
{
    int lo = 0, hi = n - 1;
    if (cp > 0xFFFF) return -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (t[mid] == cp) return mid;
        if (t[mid] < cp) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

unsigned jt_fold(unsigned cp)
{
    if (cp < 0x80) return cp;
    int i = find(jfont_fold_from, jfont_fold_count, cp);
    return i < 0 ? cp : jfont_fold_to[i];
}

const unsigned char *jt_glyph(unsigned cp)
{
    int i = find(jfont_cp, jfont_count, cp);
    return i < 0 ? 0 : &jfont_bits[i * 8];
}

int jt_cols(const char *s)
{
    int n = 0;
    while (jt_next(&s)) n++;
    return n;
}

int jt_fit(char *dst, int size, const char *src, int cols)
{
    int o = 0, n = 0;
    while (n < cols) {
        const char *start = src;
        if (!jt_next(&src)) break;
        int len = (int)(src - start);
        if (o + len > size - 1) break;
        for (int i = 0; i < len; i++) dst[o++] = start[i];
        n++;
    }
    if (size > 0) dst[o] = 0;
    return n;
}
