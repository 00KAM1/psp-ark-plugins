/*
 * Emblem - the file that keeps what has been earned. Uses only sceIo*, so
 * the same file is built and tested on a PC. Nothing is sent anywhere.
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <string.h>
#include "common.h"
#include "ach.h"

int ac_count(const AcSet *set)
{
    int n = 0;
    for (int i = 1; i <= EM_MAX_ID; i++) n += set->has[i] != 0;
    return n;
}

/* One line of the file: "73 | 2026-11-22 14:03 | ..." */
static int take_line(const char *l, AcSet *set)
{
    int id = 0, digits = 0;
    const char *p = l;
    while (*p >= '0' && *p <= '9' && digits < 4) { id = id * 10 + (*p++ - '0'); digits++; }
    while (*p == ' ') p++;
    if (!digits || *p != '|' || id < 1 || id > EM_MAX_ID) return 0;
    if (set->has[id]) return 0;                         /* twice in the file: the first one stands */
    set->has[id] = 1;
    p++;
    while (*p == ' ') p++;
    int k = 0;                                          /* the date, if the line has one */
    while (k < 16 && p[k] && p[k] != '|' && p[k] != '\r') { set->date[id][k] = p[k]; k++; }
    while (k > 0 && set->date[id][k - 1] == ' ') k--;
    set->date[id][k] = 0;
    return 1;
}

/* The file is read a piece at a time, so it can be any length. */
int ac_load(const char *path, AcSet *set)
{
    static char buf[1024];
    char line[48];                                      /* the start of a line is all that is needed */
    int ll = 0, lines = 0, n;
    memset(set, 0, sizeof(*set));
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return 0;
    while ((n = sceIoRead(fd, buf, sizeof(buf))) > 0)
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') { line[ll] = 0; lines += take_line(line, set); ll = 0; }
            else if (ll < (int)sizeof(line) - 1) line[ll++] = buf[i];
        }
    sceIoClose(fd);
    line[ll] = 0;
    lines += take_line(line, set);
    return lines;
}

int ac_append(const char *path, const char *game, const char *title, int id, const char *stamp,
              const char *ach_title, int points)
{
    char line[240];
    SceIoStat st;
    int fresh = sceIoGetstat(path, &st) < 0, len = 0;
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd < 0) return -1;
    if (fresh) len = ac_snprintf(line, sizeof(line), "# Achievements earned in %s (%s), kept by Emblem\r\n", title, game);
    len += ac_snprintf(line + len, sizeof(line) - len, "%d | %.4s-%.2s-%.2s %.2s:%.2s | %s | %d pts\r\n", id,
                       stamp, stamp + 4, stamp + 6, stamp + 9, stamp + 11, ach_title, points);
    int w = sceIoWrite(fd, line, len);
    sceIoClose(fd);
    return w == len ? 0 : -2;
}

int ac_copy_file(const char *from, const char *to)
{
    static char buf[4096];
    SceUID in = sceIoOpen(from, PSP_O_RDONLY, 0);
    if (in < 0) return -1;
    SceUID out = sceIoOpen(to, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (out < 0) { sceIoClose(in); return -2; }
    int r = 0, n;
    while ((n = sceIoRead(in, buf, sizeof(buf))) > 0) if (sceIoWrite(out, buf, n) != n) { r = -3; break; }
    if (n < 0) r = -4;
    sceIoClose(in);
    sceIoClose(out);
    if (r) sceIoRemove(to);
    return r;
}
