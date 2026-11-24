#include <stdio.h>
#include <string.h>
#include "../src/track.h"
static int fails, passes;
#define CHECK(c, m) do { if (c) { passes++; printf("PASS %s\n", m); } else { fails++; printf("FAIL %s\n", m); } } while (0)

/* the "memory stick": the record as text, exactly as the plugin keeps it */
static char g_file[96];
static char g_ended[400], g_use[2000];
static int look(gu32 now, int pct, int chg, int first, const char *prog)
{
    GaRec r; char line[200];
    ga_parse(g_file[0] ? g_file : NULL, &r);
    int what = ga_step(&r, now, pct, first, line, sizeof(line));
    if (line[0]) { strcat(g_ended, line); strcat(g_ended, "\n"); }
    if (ga_use_line(&r, now, pct, chg, what, first ? prog : NULL, line, sizeof(line))) { strcat(g_use, line); strcat(g_use, "\n"); }
    ga_format(&r, g_file, sizeof(g_file));
    return what;
}
static GaRec rec(void) { GaRec r; ga_parse(g_file, &r); return r; }

int main(void)
{
    gu32 t = ga_minutes(2026, 11, 24, 9, 0);
    char d[24]; ga_date(t, d, sizeof(d));
    CHECK(t == (9824u * 1440 + 540) && !strcmp(d, "Tue Nov 24 09:00"), "minutes since 2000 and the date text match Shelf's");
    CHECK(ga_minutes(1999, 1, 1, 0, 0) == 0 && ga_minutes(2026, 13, 1, 0, 0) == 0, "a clock that is not set gives 0");

    /* power on into the XMB */
    CHECK(look(t, 100, 0, 1, "XMB") == GA_NEW && rec().start == t && rec().start_pct == 100 && rec().owner == GA_OWNER, "no record: a new power-on starts");
    CHECK(look(t + 2, 99, 0, 0, NULL) == GA_SAME && rec().seen == t + 2 && rec().start == t, "two minutes later: the same power-on");
    /* a game is started: the PSP restarts, the plugin is loaded again */
    CHECK(look(t + 3, 99, 0, 1, "Ys.iso") == GA_SAME && rec().start == t, "a new program within minutes: still the same power-on");
    for (int i = 1; i <= 30; i++) look(t + 3 + i * 2, 99 - i / 3, 0, 0, NULL);
    CHECK(rec().start == t && rec().seen == t + 63 && rec().seen_pct == 89, "an hour of play: start kept, last look and battery follow");

    /* a Sidequest load: the plugin's memory goes back an hour, the file does not */
    CHECK(look(t + 64, 89, 0, 0, NULL) == GA_SAME && rec().start == t && rec().asleep == 0, "after a state load (old memory, current file): nothing jumps");
    /* a state from another power-on is loaded right after the game started */
    CHECK(look(t + 65, 89, 0, 0, NULL) == GA_SAME, "  ... the loaded copy never thinks it is new");

    /* sleep for three hours: same plugin in memory, no looks */
    CHECK(look(t + 65 + 180, 88, 0, 0, NULL) == GA_WOKE && rec().asleep == 180 && rec().start == t, "three hours without a look, plugin still loaded: asleep, same power-on");
    CHECK(look(t + 65 + 182, 88, 0, 0, NULL) == GA_SAME && rec().asleep == 180, "  ... counted once");

    /* power off overnight, on again */
    gu32 n = t + 1440;
    CHECK(look(n, 60, 1, 1, "XMB") == GA_NEW && rec().start == n && rec().start_pct == 60 && rec().asleep == 0, "a day without a look, plugin freshly loaded: a new power-on");
    printf("  ended: %s", g_ended);
    CHECK(!strcmp(g_ended, "Tue Nov 24 09:00 to Tue Nov 24 13:07 | 1h 07m on, 3h 00m asleep | battery 100% to 88% | -12%\n"), "  ... and one line for the one before, with the sleep apart");

    /* the clock is set back two hours */
    look(n + 10, 60, 1, 0, NULL);
    CHECK(look(n + 10 - 120, 61, 1, 0, NULL) == GA_SAME && rec().seen == n - 110 && rec().seen - rec().start == 10, "clock set back: no new power-on, the time so far is kept");

    /* Shelf's own record (five fields, no Runtime) is taken over */
    strcpy(g_file, "14147820 80 14147830 78 1\r\n");
    CHECK(look(14147832u, 78, 0, 1, "launcher") == GA_SAME && rec().start == 14147820u && rec().owner == GA_OWNER, "Shelf's own record, still fresh: carried on and taken over");
    strcpy(g_file, "14147820 80 14147830 78 0\r\n"); g_ended[0] = 0;
    CHECK(look(14149000u, 50, 0, 1, "XMB") == GA_NEW && strstr(g_ended, "0h 10m | battery 80% to 78% | -2%"), "Shelf's own record, old: closed with a line, new power-on");

    /* damage */
    GaRec r;
    ga_parse("", &r); CHECK(!r.ok, "empty file: no record");
    ga_parse("garbage here", &r); CHECK(!r.ok, "text: no record");
    ga_parse("14147830 80 14147820 78 2 0 80", &r); CHECK(!r.ok, "last look before the start: no record");
    ga_parse("14147820 999 14147830 -5 2 99999 300", &r);
    CHECK(r.ok && r.start_pct == -1 && r.seen_pct == -1 && r.asleep == 10 && r.mark_pct == -1, "wild numbers are cut to what can be true");
    ga_parse("99999999999999999999 1 2 3 4", &r); CHECK(!r.ok, "over-long numbers do not overflow");
    { char e[8]; ga_parse("14147820 80 14147830 78 2 0 80", &r); r.ok = 0; ga_parse(NULL, &r);
      CHECK(ga_step(&r, 0, 50, 1, e, sizeof(e)) == GA_SAME && !r.ok, "no clock reading: nothing is decided"); }
    char tiny[8]; ga_date(t, tiny, sizeof(tiny)); CHECK(strlen(tiny) == 7, "short buffers are never overrun");
    g_file[0] = 0; CHECK(look(t, -1, 0, 1, "XMB") == GA_NEW && rec().start_pct == -1, "no battery (mains only): -1, still tracked");
    look(t + 2000, -1, 0, 1, "XMB");

    /* the use log */
    g_file[0] = 0; g_use[0] = 0;
    look(t, 100, 0, 1, "XMB"); look(t + 2, 99, 0, 0, NULL); look(t + 3, 99, 0, 1, "Ys.iso");
    for (int i = 1; i <= 20; i++) look(t + 3 + i * 2, 99 - i / 2, 0, 0, NULL);
    look(t + 300, 88, 0, 0, NULL); look(t + 302, 9, 1, 1, "PS1 CHRONO");
    printf("%s", g_use);
    CHECK(!strcmp(g_use,
        "Tue Nov 24 09:00 | 100%          | power on XMB\n"
        "Tue Nov 24 09:03 |  99%          | start Ys.iso\n"
        "Tue Nov 24 09:23 |  94%\n"
        "Tue Nov 24 09:43 |  89%\n"
        "Tue Nov 24 14:00 |  88%          | woke\n"
        "Tue Nov 24 14:02 |   9% charging | start PS1 CHRONO\n"), "use log: power on, each program, every 5 percent, waking; columns line up");

    printf("\n%d passed, %d failure(s)\n", passes, fails);
    return fails != 0;
}
