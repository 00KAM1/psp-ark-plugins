/* PC tests for the parts of Sidequest that do not need a PSP:
 * text formatting, CRC, config, snapshot packing, stream validation, slot files. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/common.h"
#include "../src/slots.h"

extern int g_fail_write_after;
static int fails;
#define CHECK(c, m) do { int _ok = (c); printf("%s %s\n", _ok ? "PASS" : "FAIL", m); if (!_ok) fails++; } while (0)

static u32 rng = 12345;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* Reference unpacker with full bounds checks. Returns 0 if the stream wrote
 * exactly `words` words at base and nothing else. */
static int unpack_checked(const u32 *s, u32 nwords_stream, u32 base, u32 *out, u32 words)
{
    const u32 *end = s + nwords_stream;
    u32 pos = 0; int seeked = 0;
    while (s < end) {
        u32 tag = *s & SQ_T_MASK, n = *s & SQ_N_MASK; s++;
        if (tag == SQ_T_END) return (s == end && pos == words && seeked) ? 0 : -1;
        if (tag == SQ_T_SEEK) { if (s >= end || *s++ != base || seeked) return -2; seeked = 1; continue; }
        if (!seeked || n > words - pos) return -3;
        if (tag == SQ_T_FILL) { if (s >= end) return -4; for (u32 i = 0; i < n; i++) out[pos++] = *s; s++; }
        else { if ((u32)(end - s) < n) return -5; memcpy(out + pos, s, n * 4); pos += n; s += n; }
    }
    return -6;
}

static void make_data(u32 *b, u32 n, int kind)
{
    for (u32 i = 0; i < n; ) {
        u32 run = 1 + rnd() % (kind == 0 ? 1 : kind == 1 ? 40 : 4);
        u32 v = (kind == 3) ? 0 : rnd() % (kind == 2 ? 3 : 0xFFFFFFFF);
        int repeat = (kind == 0) ? 0 : (rnd() & 1);
        for (u32 j = 0; j < run && i < n; j++, i++) b[i] = repeat ? v : rnd() % (kind == 2 ? 3 : 0xFFFFFFFF);
    }
}

int main(void)
{
    char b[128];

    sq_snprintf(b, sizeof b, "%d %u %x %08X %s|%-6.3s|%5s|%c|%-2d|%-2d|", -42, 7u, 0xbeefu, 0x1Au, "hi", "abcdef", "ab", 'Z', 1, 10);
    CHECK(!strcmp(b, "-42 7 beef 0000001A hi|abc   |   ab|Z|1 |10|"), "text formatting");
    sq_snprintf(b, 8, "%s", "truncate-me");
    CHECK(!strcmp(b, "truncat"), "text formatting truncates safely");
    sq_strcpy(b, 4, "abcdefgh");
    CHECK(!strcmp(b, "abc"), "string copy always terminates");

    CHECK(crc32_update(0, "123456789", 9) == 0xCBF43926u, "crc32 check value");
    CHECK(crc32_update(crc32_update(0, "1234", 4), "56789", 5) == 0xCBF43926u, "crc32 is incremental");

    system("mkdir -p ms0/SIDEQUEST");
    FILE *f = fopen("ms0/SIDEQUEST/config.txt", "w");
    fputs("# test\r\nenable_load = 1\r\nhold_ms=900  # comment\nhotkey = 0\ndeep_sleep=0\n", f);
    fclose(f);
    config_load();
    CHECK(g_cfg.enable_load == 1 && g_cfg.hold_ms == 900 && g_cfg.deep_sleep == 0, "config parses");
    CHECK(g_cfg.hotkey == 0x1, "an empty hotkey falls back to the default");

    /* ---- packing: round trip, measure, overflow ------------------------- */
    enum { N = 1 << 16 };
    u32 *src = malloc(N * 4), *out = malloc(N * 4), *pk = malloc((N + N / 2 + 64) * 4);
    int rt = 1, meas = 1, ovf = 1, never_big = 1;
    for (int iter = 0; iter < 400; iter++) {
        u32 n = (iter < 8) ? (u32)iter + 1 : 1 + rnd() % N;
        make_data(src, n, iter % 4);
        pk[0] = SQ_T_SEEK; pk[1] = 0x88000000u;
        u32 *e = sq_pack(src, n, pk + 2, pk + N + N / 2 + 60);
        if (!e) { rt = 0; break; }
        u32 used = (u32)(e - (pk + 2));
        if (used != sq_pack_measure(src, n)) meas = 0;
        if (used > n + n / 3 + 2) never_big = 0;          /* worst case: 1 header per 3 words */
        *e++ = SQ_T_END;
        if (unpack_checked(pk, (u32)(e - pk), 0x88000000u, out, n) || memcmp(src, out, n * 4)) rt = 0;
        SqRegion reg = { 0x88000000u, n };
        if (sq_stream_validate(pk, (u32)(e - pk) * 4, &reg, 1) != 0) rt = 0;
        /* one word too little room: must fail and leave the canary alone */
        u32 *tight = malloc((used + 1) * 4);
        tight[used - 1] = 0xCAFEF00D; tight[used] = 0xCAFEF00D;
        if (used > 1 && (sq_pack(src, n, tight, tight + used - 1) || tight[used - 1] != 0xCAFEF00D)) ovf = 0;
        free(tight);
    }
    CHECK(rt, "pack -> unpack returns the original data (400 random buffers)");
    CHECK(meas, "measured size always equals the packed size");
    CHECK(ovf, "packing stops cleanly when the output is full, never writes past the end");
    CHECK(never_big, "packed data never grows by more than a third");

    /* ---- validator: nothing unsafe is ever accepted ---------------------- */
    u32 n = 5000;
    make_data(src, n, 1);
    pk[0] = SQ_T_SEEK; pk[1] = 0x88000000u;
    u32 *e = sq_pack(src, n, pk + 2, pk + N); *e++ = SQ_T_END;
    u32 sw = (u32)(e - pk);
    SqRegion reg = { 0x88000000u, n };
    int bad_accept = 0, accepted = 0;
    u32 *m = malloc(sw * 4);
    for (int i = 0; i < 200000; i++) {
        memcpy(m, pk, sw * 4);
        u32 at = rnd() % sw;
        m[at] = (i & 1) ? rnd() : (m[at] ^ (1u << (rnd() % 32)));
        if (sq_stream_validate(m, sw * 4, &reg, 1) == 0) {
            accepted++;
            if (unpack_checked(m, sw, 0x88000000u, out, n) != 0) bad_accept++;
        }
    }
    CHECK(bad_accept == 0, "200000 damaged streams: none that could write out of bounds is accepted");
    int trunc_ok = 1;
    for (u32 cut = 0; cut < sw; cut++) if (sq_stream_validate(pk, cut * 4, &reg, 1) == 0) trunc_ok = 0;
    CHECK(trunc_ok, "every truncated stream is rejected");
    SqRegion wrong = { 0x88000004u, n }, shorter = { 0x88000000u, n - 1 };
    CHECK(sq_stream_validate(pk, sw * 4, &wrong, 1) && sq_stream_validate(pk, sw * 4, &shorter, 1) &&
          sq_stream_validate(pk, sw * 4 + 4, &reg, 1), "wrong address, wrong size or trailing data is rejected");

    /* ---- staging layout -------------------------------------------------- */
    typedef struct { u32 lo, hi; } Range;
    u32 staging_largest_gap(Range *busy, int nb, u32 lo, u32 hi, u32 *gap_lo);
    u32 g0;
    Range b1[] = { { 0x0BC00000, 0x0C000000 }, { 0x0BB00000, 0x0BC00000 } };       /* the real PSP-3000 case */
    CHECK(staging_largest_gap(b1, 2, 0x0A000000, 0x0C000000, &g0) == 0x01B00000 && g0 == 0x0A000000,
          "layout: staging is the 27MB below the busy partition and the ME area");
    Range b2[] = { { 0x0A000000, 0x0A400000 }, { 0x0BC00000, 0x0C000000 } };
    CHECK(staging_largest_gap(b2, 2, 0x0A000000, 0x0C000000, &g0) == 0x01800000 && g0 == 0x0A400000,
          "layout: a busy area at the start moves staging up");
    Range b3[] = { { 0x0A000000, 0x0C000000 } };
    CHECK(staging_largest_gap(b3, 1, 0x0A000000, 0x0C000000, &g0) == 0, "layout: no gap when everything is busy");
    CHECK(staging_largest_gap(b3, 0, 0x0A000000, 0x0C000000, &g0) == 0x02000000 && g0 == 0x0A000000, "layout: all free");

    int staging_merge(Range *busy, int nb);
    /* what the real PSP-3000 reported: partitions 8 and 12 describe the same 1MB */
    Range b4[] = { { 0x0BC00000, 0x0C000000 }, { 0x0BBAA400, 0x0BC00000 }, { 0x0BB00000, 0x0BC00000 } };
    int n4 = staging_merge(b4, 3);
    CHECK(n4 == 1 && b4[0].lo == 0x0BB00000 && b4[0].hi == 0x0C000000, "layout: overlapping busy areas are joined into one");
    Range b5[] = { { 0x0A100000, 0x0A200000 }, { 0x0B000000, 0x0B100000 } };
    CHECK(staging_merge(b5, 2) == 2, "layout: separate busy areas stay separate");

    /* ---- slot files ------------------------------------------------------ */
    SqSlotHeader h, r;
    memset(&h, 0, sizeof h);
    h.magic = SQ_MAGIC; h.version = SQ_HDR_VERSION; h.header_size = SQ_HDR_SIZE;
    strcpy(h.title, "Test Game");
    u32 len = 700 * 1024 + 12;
    u8 *img = malloc(len), *back = malloc(len);
    for (u32 i = 0; i < len; i++) img[i] = (u8)(i * 31 + 7);

    CHECK(sizeof(SqSlotHeader) <= SQ_HDR_SIZE, "header fits its 4KB block");
    slots_set_game("TESTGAME");
    CHECK(slot_write(2, &h, img, len, NULL) == 0, "slot write succeeds");
    CHECK(slot_read_header(2, &r) == 0 && !strcmp(r.title, "Test Game") && r.stream_len == len, "header reads back");
    CHECK(slot_read_stream(2, &r, back, len, NULL) == 0 && !memcmp(img, back, len), "data round-trips and its checksum verifies");
    CHECK(slot_read_stream(2, &r, back, len - 4, NULL) == -4, "a slot bigger than the buffer is refused before reading");

    f = fopen("ms0/SIDEQUEST/TESTGAME/STATE2.SQS", "r+b");
    fseek(f, SQ_HDR_SIZE + 5000, SEEK_SET); fputc(img[5000] ^ 0xFF, f); fclose(f);
    CHECK(slot_read_stream(2, &r, back, len, NULL) == -3, "damaged slot data is rejected");
    f = fopen("ms0/SIDEQUEST/TESTGAME/STATE2.SQS", "r+b");
    fseek(f, 40, SEEK_SET); fputc(0x5A, f); fclose(f);
    CHECK(slot_read_header(2, &r) < 0, "damaged slot header is rejected");

    CHECK(slot_write(0, &h, img, len, NULL) == 0, "write slot 1");
    g_fail_write_after = 2;
    h.title[0] = 'X';
    CHECK(slot_write(0, &h, img, len, NULL) == -2, "a failed write is reported");
    g_fail_write_after = -1;
    CHECK(slot_read_header(0, &r) == 0 && r.title[0] == 'T' &&
          slot_read_stream(0, &r, back, len, NULL) == 0, "the previous slot survives a failed write");
    FILE *t = fopen("ms0/SIDEQUEST/TESTGAME/SUSPEND.TMP", "rb"), *k = fopen("ms0/SIDEQUEST/TESTGAME/SUSPEND.BAK", "rb");
    CHECK(!t && !k, "no temporary files left behind");
    CHECK(slot_write(0, &h, img, len, NULL) == 0 && slot_read_header(0, &r) == 0 && r.title[0] == 'X',
          "overwriting a slot replaces it");
    k = fopen("ms0/SIDEQUEST/TESTGAME/SUSPEND.BAK", "rb");
    CHECK(!k, "backup removed after a successful overwrite");
    CHECK(slot_delete(0) == 0 && slot_read_header(0, &r) < 0, "delete removes the slot");
    CHECK(slot_read_header(5, &r) < 0, "a missing slot reads as empty");

    printf("\n%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
