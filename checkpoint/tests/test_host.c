/* PC tests for the vault: real folders under ./ms0, with injected write failures. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../src/common.h"
#include "../src/jtext.h"
extern int g_fail_write_after;
static int fails;
#define CHECK(c, what) do { if (c) printf("PASS %s\n", what); else { printf("FAIL %s\n", what); fails++; } } while (0)

static void put(const char *path, const char *data, int len)
{
    FILE *f = fopen(path, "wb"); if (!f) { perror(path); exit(1); }
    fwrite(data, 1, len, f); fclose(f);
}
static int get(const char *path, char *out, int max)
{
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    int n = fread(out, 1, max, f); fclose(f); return n;
}
static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static int count_versions(const char *save) { BtVersion v[BT_MAX_VERSIONS]; return vault_list_versions(save, v, BT_MAX_VERSIONS); }
static void stamp(char *out, int minute_of_day, int sec)
{ sprintf(out, "20261119_%02d%02d%02d", minute_of_day / 60, minute_of_day % 60, sec); }

static char big[200000];

int main(void)
{
    char s[32], buf[300000];
    BtVersion v[BT_MAX_VERSIONS];
    system("rm -rf ms0 && mkdir -p ms0/PSP/SAVEDATA/ULUS10545SAVE00");
    for (unsigned i = 0; i < sizeof(big); i++) big[i] = (char)(i * 7 + (i >> 9));
    #define LIVE "ms0/PSP/SAVEDATA/ULUS10545SAVE00/"
    #define VAULT "ms0/CHECKPOINT/ULUS10545SAVE00/"
    put(LIVE "DATA.BIN", big, sizeof(big));
    put(LIVE "PARAM.SFO", "sfo-v1", 6);
    put(LIVE "ICON0.PNG", "icon", 4);

    CHECK(vault_name_ok("ULUS10545SAVE00") && !vault_name_ok("../X") && !vault_name_ok("") && !vault_name_ok("A/B") && !vault_name_ok("A B"), "only plain folder names are accepted");
    CHECK(vault_backup("../PSP", "20261119_090000", 10, 10, 0, 0xFFFFFFFF) < 0, "a path-like save name is refused");
    CHECK(vault_backup("NOSUCHSAVE", "20261119_090000", 10, 10, 0, 0xFFFFFFFF) == BT_NOTHING, "a save that does not exist is skipped");
    CHECK(vault_stamp_minutes("20261119_0914") == 0 && vault_stamp_minutes("20261119_091400") + 6 == vault_stamp_minutes("20261119_092059"), "stamps are validated and ordered");

    stamp(s, 9 * 60, 0);
    CHECK(vault_backup("ULUS10545SAVE00", s, 10, 10, 0, 0xFFFFFFFF) == BT_OK, "first backup");
    CHECK(get(VAULT "20261119_090000/DATA.BIN", buf, sizeof(buf)) == (int)sizeof(big) && !memcmp(buf, big, sizeof(big)), "the copy is byte-identical");
    CHECK(exists(VAULT "20261119_090000/PARAM.SFO") && exists(VAULT "20261119_090000/ICON0.PNG") && !exists(VAULT "NEW.TMP"), "all files copied, no temp folder left");

    stamp(s, 9 * 60 + 1, 0);
    CHECK(vault_backup("ULUS10545SAVE00", s, 10, 10, 0, 0xFFFFFFFF) == BT_SAME && count_versions("ULUS10545SAVE00") == 1, "an unchanged save makes no new version");

    put(LIVE "PARAM.SFO", "sfo-v2", 6);
    stamp(s, 9 * 60 + 3, 0);
    CHECK(vault_backup("ULUS10545SAVE00", s, 10, 10, 0, 0xFFFFFFFF) == BT_OK && count_versions("ULUS10545SAVE00") == 1 &&
          exists(VAULT "20261119_090300") && !exists(VAULT "20261119_090000"), "a save 3 minutes later replaces the newest version");

    put(LIVE "PARAM.SFO", "sfo-v3", 6);
    stamp(s, 9 * 60 + 4, 0);
    CHECK(vault_backup("ULUS10545SAVE00", s, 10, 10, 1, 0xFFFFFFFF) == BT_OK && count_versions("ULUS10545SAVE00") == 2, "a forced backup always adds a version");

    for (int k = 0; k < 14; k++) {                      /* 14 saves, 15 minutes apart */
        char d[16]; sprintf(d, "sfo-%02d", k);
        put(LIVE "PARAM.SFO", d, 6);
        stamp(s, 10 * 60 + k * 15, 0);
        if (vault_backup("ULUS10545SAVE00", s, 10, 10, 0, 0xFFFFFFFF) != BT_OK) { fails++; printf("FAIL spaced backup %d\n", k); }
    }
    int n = vault_list_versions("ULUS10545SAVE00", v, BT_MAX_VERSIONS);
    CHECK(n == 10, "history is capped at 10 versions");
    CHECK(!strcmp(v[0].stamp, "20261119_131500") && !strcmp(v[9].stamp, "20261119_110000"), "newest first; the oldest were dropped");
    int ordered = 1; for (int i = 1; i < n; i++) if (strcmp(v[i - 1].stamp, v[i].stamp) <= 0) ordered = 0;
    CHECK(ordered, "versions are listed newest to oldest");

    /* failed copy: nothing appears, nothing is lost */
    put(LIVE "PARAM.SFO", "sfo-XX", 6);
    g_fail_write_after = 2;
    stamp(s, 14 * 60, 0);
    CHECK(vault_backup("ULUS10545SAVE00", s, 10, 10, 0, 0xFFFFFFFF) == BT_E_COPY, "a failed copy is reported");
    g_fail_write_after = -1;
    CHECK(count_versions("ULUS10545SAVE00") == 10 && !exists(VAULT "NEW.TMP") && !exists(VAULT "20261119_140000") && exists(VAULT "20261119_131500"), "after a failed copy the history is unchanged");
    CHECK(vault_backup("ULUS10545SAVE00", s, 10, 10, 0, 100) == BT_E_SPACE && count_versions("ULUS10545SAVE00") == 10, "refused when the stick is nearly full");

    /* leftover temp folder from a power cut */
    system("mkdir -p " VAULT "NEW.TMP && echo junk > " VAULT "NEW.TMP/DATA.BIN");
    CHECK(vault_backup("ULUS10545SAVE00", s, 10, 10, 0, 0xFFFFFFFF) == BT_OK && !exists(VAULT "NEW.TMP") &&
          get(VAULT "20261119_140000/DATA.BIN", buf, sizeof(buf)) == (int)sizeof(big), "a leftover temp folder is cleaned up");

    /* restore */
    put(LIVE "DATA.BIN", "ruined", 6);
    put(LIVE "EXTRA.BIN", "new file the old version never had", 34);
    CHECK(vault_restore("ULUS10545SAVE00", "20261119_120000") == BT_OK, "restore an older version");
    CHECK(get(LIVE "DATA.BIN", buf, sizeof(buf)) == (int)sizeof(big) && !memcmp(buf, big, sizeof(big)) &&
          get(LIVE "PARAM.SFO", buf, 16) == 6 && !memcmp(buf, "sfo-08", 6), "the live save now equals that version");
    CHECK(!exists(LIVE "EXTRA.BIN") && !exists(LIVE "DATA.BIN.NEW") && !exists(LIVE "PARAM.SFO.NEW"), "files the version lacks are gone, no .NEW files left");
    CHECK(exists(VAULT "20261119_120000/DATA.BIN"), "restoring does not consume the version");

    put(LIVE "DATA.BIN", "current progress", 16);
    g_fail_write_after = 1;
    CHECK(vault_restore("ULUS10545SAVE00", "20261119_113000") == BT_E_COPY, "a failed restore is reported");
    g_fail_write_after = -1;
    CHECK(get(LIVE "DATA.BIN", buf, 64) == 16 && !memcmp(buf, "current progress", 16) && !exists(LIVE "DATA.BIN.NEW") && !exists(LIVE "ICON0.PNG.NEW") && !exists(LIVE "PARAM.SFO.NEW"),
          "after a failed restore the live save is untouched");
    CHECK(vault_restore("ULUS10545SAVE00", "20260101_000000") < 0 && get(LIVE "DATA.BIN", buf, 64) == 16, "restoring a version that does not exist changes nothing");

    /* restore into a save folder the game has since deleted */
    system("rm -rf ms0/PSP/SAVEDATA/ULUS10545SAVE00");
    CHECK(vault_restore("ULUS10545SAVE00", "20261119_131500") == BT_OK && get(LIVE "DATA.BIN", buf, sizeof(buf)) == (int)sizeof(big), "a deleted save can be brought back");

    /* listing and deleting */
    system("mkdir -p ms0/PSP/SAVEDATA/ULUS10545SAVE01 ms0/PSP/SAVEDATA/NPJH50000X ms0/CHECKPOINT/ULUS10545EMPTY && echo a > ms0/PSP/SAVEDATA/ULUS10545SAVE01/D && echo b > ms0/PSP/SAVEDATA/NPJH50000X/D");
    vault_backup("ULUS10545SAVE01", "20261119_150000", 10, 10, 0, 0xFFFFFFFF);
    vault_backup("NPJH50000X", "20261119_150000", 10, 10, 0, 0xFFFFFFFF);
    char names[8][BT_NAME_MAX];
    CHECK(vault_list_saves("ULUS10545", names, 8) == 2 && vault_list_saves("", names, 8) == 3 && vault_list_saves("ZZZZ", names, 8) == 0, "saves are listed by game, folders without versions are skipped");
    CHECK(vault_delete_version("ULUS10545SAVE01", "20261119_150000") == BT_OK && !exists("ms0/CHECKPOINT/ULUS10545SAVE01"), "deleting the last version removes the folder");
    CHECK(vault_delete_version("ULUS10545SAVE00", "20261119_140000") == BT_OK && count_versions("ULUS10545SAVE00") == 9, "deleting one version leaves the others");

    /* pinning */
    {
        system("rm -rf ms0/BACKTRACK ms0/CHECKPOINT ms0/PSP/SAVEDATA/PINGAME && mkdir -p ms0/PSP/SAVEDATA/PINGAME");
        #define PL "ms0/PSP/SAVEDATA/PINGAME/"
        #define PV "ms0/CHECKPOINT/PINGAME/"
        char d[16];
        for (int k = 0; k < 4; k++) { sprintf(d, "pin-%02d", k); put(PL "DATA.BIN", d, 6); stamp(s, 8 * 60 + k * 15, 0); vault_backup("PINGAME", s, 10, 10, 0, 0xFFFFFFFF); }
        CHECK(vault_set_pin("PINGAME", "20261119_081500", 1) == BT_OK && exists(PV "20261119_081500.PIN"), "a version can be pinned");
        CHECK(vault_set_pin("PINGAME", "20260101_000000", 1) == BT_E_MISSING, "pinning a version that does not exist is refused");
        n = vault_list_versions("PINGAME", v, BT_MAX_VERSIONS);
        CHECK(n == 4 && v[2].pinned && !v[0].pinned && !v[1].pinned && !v[3].pinned, "the list shows which version is pinned");
        for (int k = 4; k < 20; k++) { sprintf(d, "pin-%02d", k); put(PL "DATA.BIN", d, 6); stamp(s, 8 * 60 + k * 15, 0); vault_backup("PINGAME", s, 10, 10, 0, 0xFFFFFFFF); }
        n = vault_list_versions("PINGAME", v, BT_MAX_VERSIONS);
        int pins = 0; for (int i = 0; i < n; i++) pins += v[i].pinned;
        CHECK(n == 11 && pins == 1 && exists(PV "20261119_081500/DATA.BIN") && !exists(PV "20261119_080000") && !exists(PV "20261119_083000"),
              "after 16 more saves: 10 rolling versions plus the pinned one, older unpinned ones gone");
        CHECK(get(PV "20261119_081500/DATA.BIN", buf, 16) == 6 && !memcmp(buf, "pin-01", 6), "the pinned version still holds its own data");
        /* a pinned newest version is not replaced by a save a minute later */
        vault_set_pin("PINGAME", "20261119_124500", 1);
        put(PL "DATA.BIN", "pin-AA", 6);
        CHECK(vault_backup("PINGAME", "20261119_124600", 10, 10, 0, 0xFFFFFFFF) == BT_OK && exists(PV "20261119_124500/DATA.BIN") && exists(PV "20261119_124600/DATA.BIN"),
              "a pinned newest version is not replaced by a quick follow-up save");
        /* unpinning returns it to the rolling history */
        CHECK(vault_set_pin("PINGAME", "20261119_081500", 0) == BT_OK && !exists(PV "20261119_081500.PIN"), "a version can be unpinned");
        put(PL "DATA.BIN", "pin-BB", 6);
        vault_backup("PINGAME", "20261119_140000", 10, 10, 0, 0xFFFFFFFF);
        CHECK(!exists(PV "20261119_081500"), "an unpinned old version is dropped at the next save");
        CHECK(vault_delete_version("PINGAME", "20261119_124500") == BT_OK && !exists(PV "20261119_124500") && !exists(PV "20261119_124500.PIN"), "deleting a pinned version removes its marker too");
        /* the limit */
        n = vault_list_versions("PINGAME", v, BT_MAX_VERSIONS);
        int okpins = 0, r11 = 0;
        for (int i = 0; i < n && i < 11; i++) { int r = vault_set_pin("PINGAME", v[i].stamp, 1); if (r == BT_OK) okpins++; else r11 = r; }
        CHECK(n >= 11 ? (okpins == 10 && r11 == BT_E_PINS) : okpins == n, "at most 10 versions of a save can be pinned");
        /* restore leaves no marker in the live save */
        vault_restore("PINGAME", v[1].stamp);
        CHECK(!exists(PL "DATA.BIN.PIN") && get(PL "DATA.BIN", buf, 16) == 6, "restoring a pinned version copies only the save files");
    }

    /* PARAM.SFO text */
    {
        unsigned char sfo[256]; memset(sfo, 0, sizeof(sfo));
        const char *k1 = "CATEGORY", *k2 = "SAVEDATA_DETAIL", *k3 = "SAVEDATA_TITLE";
        unsigned keys = 20 + 3 * 16, data = keys + 48, o = 0, d = 0;
        memcpy(sfo, "\0PSF", 4); sfo[4] = 1; sfo[5] = 1; memcpy(sfo + 8, &keys, 4); memcpy(sfo + 12, &data, 4); sfo[16] = 3;
        const char *ks[3] = { k1, k2, k3 }; const char *vs[3] = { "MS", "Chapter 3\nLv 24", "Ys Felghana" };
        for (int i = 0; i < 3; i++) {
            unsigned char *e = sfo + 20 + i * 16; unsigned short ko = o, fmt = 0x0204; unsigned len = strlen(vs[i]) + 1, max = 32;
            memcpy(e, &ko, 2); memcpy(e + 2, &fmt, 2); memcpy(e + 4, &len, 4); memcpy(e + 8, &max, 4); memcpy(e + 12, &d, 4);
            strcpy((char *)sfo + keys + o, ks[i]); o += strlen(ks[i]) + 1;
            strcpy((char *)sfo + data + d, vs[i]); d += max;
        }
        put("ms0/t.sfo", (char *)sfo, sizeof(sfo));
        char out[64];
        CHECK(vault_sfo_string("ms0:/t.sfo", "SAVEDATA_DETAIL", out, sizeof(out)) == 0 && !strcmp(out, "Chapter 3 Lv 24"), "save details are read from PARAM.SFO, line breaks become spaces");
        CHECK(vault_sfo_string("ms0:/t.sfo", "SAVEDATA_TITLE", out, 6) == 0 && !strcmp(out, "Ys Fe"), "long text is cut to fit");
        CHECK(vault_sfo_string("ms0:/t.sfo", "NOPE", out, sizeof(out)) < 0 && vault_sfo_string("ms0:/PSP/SAVEDATA/ULUS10545SAVE00/DATA.BIN", "X", out, sizeof(out)) < 0, "missing keys and non-SFO files are rejected");
        memset(sfo + 8, 0xFF, 12); put("ms0/bad.sfo", (char *)sfo, sizeof(sfo));
        CHECK(vault_sfo_string("ms0:/bad.sfo", "SAVEDATA_TITLE", out, sizeof(out)) < 0, "a corrupt SFO is rejected without reading out of bounds");
    }

    /* ---- journey log ---- */
    {
        static JrEntry e[8]; int total; char first[32], l[2][JR_LINE_MAX], txt[100000];
        #define JR "ms0/CHECKPOINT/JOURNEY/ULUS10545"
        CHECK(journey_load("ULUS10545", e, 8, &total, first) == 0 && total == 0, "journey: no file reads as empty");
        CHECK(journey_note("ULUS10545", "ULUS10545SAVE00", "20261114_192205", "Town of Redmont", 25 * 60 + 9) == 1, "journey: first description makes an entry");
        CHECK(journey_note("ULUS10545", "ULUS10545SAVE00", "20261114_193000", "Town of Redmont", 33 * 60) == 0, "journey: same description again adds nothing");
        CHECK(journey_note("ULUS10545", "ULUS10545SAVE00", "20261114_204100", "  Tigray |  Quarry ", 100 * 60) == 1, "journey: a new description makes an entry");
        CHECK(journey_note("ULUS10545", "ULUS10545SAVE01", "20261114_204500", "Tigray / Quarry", 101 * 60) == 1, "journey: each save folder is compared with itself");
        CHECK(journey_note("ULUS10545", "ULUS10545SAVE00", "20261115_210500", "Town of Redmont", 190 * 60) == 1, "journey: going back to an earlier place is an entry");
        CHECK(journey_note("ULUS10545", "ULUS10545SAVE00", "20261115_210900", "", 191 * 60) == 0, "journey: an empty description is not logged");
        CHECK(journey_note("../X", "ULUS10545SAVE00", "20261115_210900", "x", 0) < 0 && journey_note("ULUS10545", "A/B", "20261115_210900", "x", 0) < 0
              && journey_note("ULUS10545", "ULUS10545SAVE00", "2026", "x", 0) < 0, "journey: path-like names and bad stamps are refused");
        int n = journey_load("ULUS10545", e, 8, &total, first);
        CHECK(n == 4 && total == 4 && !strcmp(first, "20261114_192200"), "journey: all entries read back, first date known");
        CHECK(!strcmp(e[0].text, "Town of Redmont") && e[0].secs == 25 * 60 && !strcmp(e[0].save, "ULUS10545SAVE00") && !strcmp(e[0].stamp, "20261114_192200"), "journey: an entry keeps date, time played, text and save");
        CHECK(!strcmp(e[1].text, "Tigray / Quarry") && e[3].secs == 190 * 60, "journey: text is cleaned of separators and extra spaces");
        memset(txt, 0, sizeof(txt)); get(JR ".TXT", txt, sizeof(txt) - 1);
        CHECK(!strncmp(txt, "# Journey log of ULUS10545", 26) && strstr(txt, "2026-11-14 19:22 | 0h 25m | Town of Redmont | ULUS10545SAVE00\r\n"), "journey: the file is plain readable text");
        n = journey_load("ULUS10545", e, 2, &total, first);
        CHECK(n == 2 && total == 4 && !strcmp(e[0].save, "ULUS10545SAVE01") && !strcmp(e[1].text, "Town of Redmont") && !strcmp(first, "20261114_192200"), "journey: a short list keeps the newest entries");

        FILE *f = fopen(JR ".TXT", "ab");
        fputs("garbage line\r\n2026-11-16 | broken\r\n\r\n", f);
        for (int i = 0; i < 500; i++) fputc('x', f);
        fputs(" | 1h 00m | long | S\r\n2026-11-16 22:30 | 4h 05m | Zone of Lava | ULUS10545SAVE00", f);   /* no line end */
        fclose(f);
        n = journey_load("ULUS10545", e, 8, &total, first);
        CHECK(n == 5 && !strcmp(e[4].text, "Zone of Lava") && e[4].secs == 4 * 3600 + 5 * 60, "journey: damaged and over-long lines are skipped");
        CHECK(journey_note("ULUS10545", "ULUS10545SAVE00", "20261116_230000", "Zone of Lava", 0) == 0, "journey: a hand-edited last line still counts as the last entry");

        g_fail_write_after = 0;
        CHECK(journey_note("ULUS10545", "ULUS10545SAVE00", "20261117_201200", "Elderm Mountains", 0) < 0, "journey: a failed write is reported");
        g_fail_write_after = -1;
        n = journey_load("ULUS10545", e, 8, &total, first);
        CHECK(n >= 5 && !strcmp(e[4].text, "Zone of Lava"), "journey: earlier entries survive a failed write");

        CHECK(journey_time_add("ULUS10545", 0) == 0, "journey: time starts at zero");
        CHECK(journey_time_add("ULUS10545", 90) == 90 && journey_time_add("ULUS10545", 30) == 120 && journey_time_add("ULUS10545", 0) == 120, "journey: time played adds up and is kept");
        g_fail_write_after = 0;
        CHECK(journey_time_add("ULUS10545", 60) == 180, "journey: a failed time write still reports the total");
        g_fail_write_after = -1;
        CHECK(journey_time_add("ULUS10545", 0) == 120 && !exists(JR ".NEW"), "journey: a failed time write leaves the old total");
        rename(JR ".TIM", JR ".NEW");
        CHECK(journey_time_add("ULUS10545", 0) == 120 && journey_time_add("ULUS10545", 5) == 125 && exists(JR ".TIM"), "journey: an interrupted time write is recovered");
        put(JR ".TIM", "rubbish", 7);
        CHECK(journey_time_add("ULUS10545", 0) == 0, "journey: a damaged time file reads as zero");

        CHECK(journey_wrap("Town of Redmont", 24, l) == 1 && !strcmp(l[0], "Town of Redmont"), "journey: short text stays on one line");
        CHECK(journey_wrap("Elderm Mountains Summit Approach", 24, l) == 2 && !strcmp(l[0], "Elderm Mountains Summit") && !strcmp(l[1], "Approach"), "journey: long text breaks at a space");
        CHECK(journey_wrap("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ", 24, l) == 2 && strlen(l[0]) == 24 && strlen(l[1]) == 24, "journey: one long word is cut, never overflows");
        CHECK(journey_weekday(2026, 11, 14) == 6 && journey_weekday(2026, 11, 20) == 5 && journey_weekday(2024, 2, 29) == 4 && journey_weekday(2000, 1, 1) == 6, "journey: weekdays are right");

        static JrEntry many[40]; u16 first_of[16];
        for (int i = 0; i < 40; i++) { memset(&many[i], 0, sizeof(many[i])); strcpy(many[i].text, i % 5 == 4 ? "A place with a very long name indeed" : "Short"); }
        int pages = journey_paginate(many, 9, 24, 18, first_of, 16);
        CHECK(pages == 3 && first_of[0] == 0 && first_of[1] == 4 && first_of[2] == 8, "journey: four short entries to a page, a long one moves on");
        CHECK(journey_paginate(many, 0, 24, 18, first_of, 16) == 0, "journey: no entries, no pages");
        CHECK(journey_paginate(many, 40, 24, 18, first_of, 3) == 3, "journey: the page table is never overrun");
        int ok = 1;
        for (int total_n = 1; total_n <= 40; total_n++) {
            int pg = journey_paginate(many, total_n, 24, 18, first_of, 16);
            for (int k = 0; k < pg; k++) {
                int end = k + 1 < pg ? first_of[k + 1] : total_n, rows = 0;
                for (int i = first_of[k]; i < end; i++) rows += 2 + journey_wrap(many[i].text, 24, l) + 1;
                if (rows - 1 > 18 || end <= first_of[k]) ok = 0;
            }
        }
        CHECK(ok, "journey: no page ever holds more lines than it has");
    }

    /* ---- journey log: text outside ASCII ---- */
    {
        static JrEntry e[8]; int total; char first[32], out[200], txt[100000];
        #define JJ "ms0/CHECKPOINT/JOURNEY/NPJH50000"
        #define MACHI "\xE3\x81\xBE\xE3\x81\xA1"                 /* two hiragana */
        #define MORI  "\xE3\x82\x82\xE3\x82\x8A"                 /* two other hiragana, same length */
        CHECK(journey_note("NPJH50000", "NPJH50000S0", "20261121_100000", MACHI, 600) == 1, "journey: Japanese text makes an entry");
        CHECK(journey_note("NPJH50000", "NPJH50000S0", "20261121_100500", MACHI, 900) == 0, "journey: the same Japanese text adds nothing");
        CHECK(journey_note("NPJH50000", "NPJH50000S0", "20261121_101000", MORI, 1200) == 1, "journey: different Japanese text of the same length is a new entry");
        CHECK(journey_note("NPJH50000", "NPJH50000S0", "20261121_102000", MORI "\xE3\x80\x80" "Lv 3 ", 1500) == 1, "journey: mixed text makes an entry");
        memset(txt, 0, sizeof(txt)); get(JJ ".TXT", txt, sizeof(txt) - 1);
        CHECK(strstr(txt, " | 0h 10m | " MACHI " | NPJH50000S0\r\n") && strstr(txt, " | " MORI " Lv 3 | "), "journey: the file holds the real text, wide space made plain");
        int n = journey_load("NPJH50000", e, 8, &total, first);
        CHECK(n == 3 && !strcmp(e[0].text, MACHI) && !strcmp(e[1].text, MORI), "journey: Japanese text reads back unchanged");
        journey_ascii(out, sizeof(out), e[2].text);
        CHECK(!strcmp(out, "?? Lv 3"), "journey: on screen each unknown character is one question mark");
        journey_ascii(out, sizeof(out), "Town of Redmont");
        CHECK(!strcmp(out, "Town of Redmont"), "journey: plain text is shown as it is");
        journey_clean(out, sizeof(out), "A\xE3\x81" "B\xFF" "C\xC0\x80" "D");
        CHECK(!strcmp(out, "A??B?C??D"), "journey: bytes that are not valid text become question marks");
        char longj[400]; longj[0] = 0;
        for (int i = 0; i < 60; i++) strcat(longj, MACHI);
        journey_clean(out, JR_TEXT_MAX, longj);
        CHECK(strlen(out) == 93 && !strncmp(out + 90, "\xE3\x81\xBE", 3), "journey: long text is cut between characters, never inside one");
        CHECK(journey_note("NPJH50000", "NPJH50000S0", "20261121_103000", longj, 1800) == 1 && journey_note("NPJH50000", "NPJH50000S0", "20261121_104000", longj, 2100) == 0, "journey: long Japanese text is still recognised as unchanged");
        strcpy(out, MACHI " X"); journey_ascii(out, sizeof(out), out);
        CHECK(!strcmp(out, "?? X"), "journey: display text can be made in place");

        unsigned char sfo[20 + 16 + 16 + 16] = { 0, 'P', 'S', 'F', 1, 1, 0, 0, 36, 0, 0, 0, 52, 0, 0, 0, 1, 0, 0, 0,
            0, 0, 4, 2, 7, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0 };
        memcpy(sfo + 36, "SAVEDATA_TITLE", 15);
        memcpy(sfo + 52, MACHI "\n", 7);
        put("ms0/j.sfo", (char *)sfo, sizeof(sfo));
        CHECK(vault_sfo_text("ms0:/j.sfo", "SAVEDATA_TITLE", out, sizeof(out)) == 0 && !strcmp(out, MACHI " "), "the save's own text can be read without changing it");
        CHECK(vault_sfo_string("ms0:/j.sfo", "SAVEDATA_TITLE", out, sizeof(out)) == 0 && !strcmp(out, "?????? "), "the menu form still replaces what the font cannot draw");
    }

    /* ---- Japanese text on screen ---- */
    {
        char l[2][JR_LINE_MAX], out[200]; const char *p;
        #define KA  "\xE3\x81\x8B"
        #define KAN "\xE6\xBC\xA2"
        p = "A" KA "\xFF" "z"; 
        CHECK(jt_next(&p) == 'A' && jt_next(&p) == 0x304B && jt_next(&p) == '?' && jt_next(&p) == 'z' && jt_next(&p) == 0 && jt_next(&p) == 0, "text: UTF-8 is read a character at a time, bad bytes become '?'");
        CHECK(jt_cols("Lv 3 " KA KAN) == 7 && jt_cols("") == 0, "text: every character counts as one cell");
        CHECK(jt_glyph(0x304B) && jt_glyph(0x6F22) && jt_glyph(0x30FC) && jt_glyph(0xFF21) && !jt_glyph(0x41) && !jt_glyph(0x1F600) && !jt_glyph(0xAC00), "text: kana, kanji and full-width letters have shapes; others do not");
        const unsigned char *g = jt_glyph(0x4E00);                      /* the kanji "one": a single bar */
        int rows = 0; for (int i = 0; i < 8; i++) rows += g[i] != 0;
        CHECK(rows == 1, "text: a glyph's dots are where they should be");
        CHECK(jt_fold(0xFF76) == 0x30AB && jt_fold(0xE9) == 'e' && jt_fold(0xC9) == 'E' && jt_fold(0x2019) == '\'' && jt_fold(0x304B) == 0x304B && jt_fold('a') == 'a', "text: half-width kana and accented letters are mapped to shapes the font has");
        CHECK(jt_fit(out, sizeof(out), KA KAN "abc", 3) == 3 && !strcmp(out, KA KAN "a"), "text: fitting counts characters, not bytes");
        CHECK(jt_fit(out, 6, KA KAN, 5) == 1 && !strcmp(out, KA), "text: fitting never cuts a character to squeeze it in");
        int ordered = 1; for (int i = 1; i < jfont_count; i++) if (jfont_cp[i - 1] >= jfont_cp[i]) ordered = 0;
        for (int i = 1; i < jfont_fold_count; i++) if (jfont_fold_from[i - 1] >= jfont_fold_from[i]) ordered = 0;
        CHECK(ordered && jfont_count > 6000, "text: the font tables are in order for searching");
        char longj[400]; longj[0] = 0;
        for (int i = 0; i < 30; i++) strcat(longj, i % 2 ? KAN : KA);
        CHECK(journey_wrap(longj, 24, l) == 2 && jt_cols(l[0]) == 24 && jt_cols(l[1]) == 6 && strlen(l[0]) == 72, "journey: Japanese text wraps between characters");
        CHECK(journey_wrap(KA KAN " " KA KAN KA KAN KA KAN KA KAN KA KAN KA KAN KA KAN KA KAN KA KAN KA KAN KA KAN KA, 24, l) == 2 && !strcmp(l[0], KA KAN) && jt_cols(l[1]) == 23, "journey: mixed text still breaks at a space");
        CHECK(journey_wrap(KA KAN, 24, l) == 1 && !strcmp(l[0], KA KAN), "journey: short Japanese text stays on one line");
    }

    /* ---- journey log: which text is news ---- */
    {
        char out[200];
        #define SD "\xE3\x82\xBB\xE3\x83\xBC\xE3\x83\x96\xE3\x83\x87\xE3\x83\xBC\xE3\x82\xBF"   /* "save data" in katakana */
        #define LV "\xE3\x83\xAC\xE3\x83\x99\xE3\x83\xAB"                                         /* "level" */
        journey_key(out, sizeof(out), "Okami " LV "12 Windaria Village  01:23:45");
        CHECK(!strcmp(out, "Okami " LV " Windaria Village"), "journey: numbers and times are left out when comparing");
        journey_key(out, sizeof(out), "Lv\xEF\xBC\x91\xEF\xBC\x92 2026/11/21");
        CHECK(!strcmp(out, "Lv"), "journey: full-width digits and dates are left out too");
        /* a game that calls every save "save data" and puts the place in the detail */
        CHECK(journey_pick("NPJH50342", "NPJH503420001", SD, "Okami " LV "1 Windaria Village 00:01", 0, out, sizeof(out)) == 1 && !strcmp(out, SD), "journey: the first save logs the title");
        CHECK(journey_pick("NPJH50342", "NPJH503420001", SD, "Okami " LV "2 Windaria Village 00:20", 0, out, sizeof(out)) == 0, "journey: a new level and play time alone are not news");
        CHECK(journey_pick("NPJH50342", "NPJH503420001", SD, "Okami " LV "3 Forest of Winds 01:05", 0, out, sizeof(out)) == 1 && !strcmp(out, "Okami " LV "3 Forest of Winds 01:05"), "journey: with a fixed title, a new place in the detail is news");
        CHECK(journey_pick("NPJH50342", "NPJH503420001", SD, "Okami " LV "4 Forest of Winds 01:30", 0, out, sizeof(out)) == 0, "journey: the same place again is not");
        CHECK(journey_pick("NPJH50342", "NPJH503420001", SD, "Okami " LV "4 Windaria Village 01:40", 0, out, sizeof(out)) == 1, "journey: going back is news");
        CHECK(journey_pick("NPJH50342", "NPJH503420002", SD, "Okami " LV "4 Windaria Village 01:41", 0, out, sizeof(out)) == 1 && !strcmp(out, SD), "journey: each save folder is remembered on its own");
        CHECK(journey_pick("NPJH50342", "NPJH503420001", SD, "Okami " LV "5 Windaria Village 02:00", 0, out, sizeof(out)) == 0, "journey: the first folder is still remembered");
        /* a game that puts the place in the title */
        CHECK(journey_pick("ULUS10558", "ULUS10558_003", "Town of Redmont", "Lv 3  0:25:10", 0, out, sizeof(out)) == 1 && !strcmp(out, "Town of Redmont"), "journey: a place in the title is logged");
        CHECK(journey_pick("ULUS10558", "ULUS10558_003", "Town of Redmont", "Lv 4  0:40:02", 0, out, sizeof(out)) == 0, "journey: same title, only numbers moved: nothing");
        CHECK(journey_pick("ULUS10558", "ULUS10558_003", "Tigray Quarry", "Lv 6  1:40:00", 0, out, sizeof(out)) == 1 && !strcmp(out, "Tigray Quarry"), "journey: a new title is logged, not the detail");
        CHECK(journey_pick("ULUS10558", "ULUS10558_003", "Elderm Mountains", "Lv 6  1:41:00", 1, out, sizeof(out)) == 0 && journey_pick("ULUS10558", "ULUS10558_003", "Elderm Mountains", "Chapter Two Lv 6", 1, out, sizeof(out)) == 1 && !strcmp(out, "Chapter Two Lv 6"), "journey: detail-only mode ignores the title");
        CHECK(journey_pick("ULUS10558", "ULUS10558_003", "", "", 0, out, sizeof(out)) == 0 && journey_pick("../X", "A", "t", "d", 0, out, sizeof(out)) == 0, "journey: empty texts and bad names give nothing");
        /* the memory survives a failed write and an interrupted one */
        g_fail_write_after = 0;
        CHECK(journey_pick("ULUS10558", "ULUS10558_003", "Illburns Ruins", "Lv 9", 0, out, sizeof(out)) == 1, "journey: news is still reported when the memory cannot be written");
        g_fail_write_after = -1;
        CHECK(!exists("ms0/CHECKPOINT/JOURNEY/ULUS10558.KNW") && exists("ms0/CHECKPOINT/JOURNEY/ULUS10558.KEY"), "journey: a failed write leaves the old memory in place");
        rename("ms0/CHECKPOINT/JOURNEY/NPJH50342.KEY", "ms0/CHECKPOINT/JOURNEY/NPJH50342.KNW");
        CHECK(journey_pick("NPJH50342", "NPJH503420001", SD, "Okami " LV "6 Windaria Village 02:10", 0, out, sizeof(out)) == 0, "journey: an interrupted memory write is recovered");
        /* many save folders: the file never overflows and the newest are kept */
        int ok = 1; char nm[32];
        for (int i = 0; i < 200; i++) {
            snprintf(nm, sizeof(nm), "MANY%05d", i);
            if (journey_pick("MANYGAME", nm, "A long title that takes up room in the file, number one", "and a long detail to go with it, also taking up room", 0, out, sizeof(out)) != 1) ok = 0;
        }
        CHECK(ok && journey_pick("MANYGAME", "MANY00199", "A long title that takes up room in the file, number one", "and a long detail to go with it, also taking up room", 0, out, sizeof(out)) == 0, "journey: two hundred save folders do not overflow the memory");
        struct stat st; stat("ms0/CHECKPOINT/JOURNEY/MANYGAME.KEY", &st);
        CHECK(st.st_size < 4096, "journey: the memory file stays small");
    }
    printf("%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
