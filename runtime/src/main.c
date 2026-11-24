/*
 * Runtime - how long the PSP has been on and what that cost in battery, in every
 * program: XMB, games, PS1 games, homebrew, Shelf. Kernel plugin for ARK-5, loaded "always".
 *
 * It draws nothing and hooks nothing. Every two minutes one thread reads the clock
 * and the battery and brings three small files in ms0:/RUNTIME up to date (a folder of
 * its own: this plugin runs inside games, so inside every Sidequest state, and Shelf's
 * folder is kept out of reach of a write made after a load):
 *   uptime.txt       the power-on in progress (Shelf's PS-button menu shows it)
 *   battery.log      one line per finished power-on
 *   battery_use.log  battery at every program start, on waking, and every 5 percent
 */
#include <pspkernel.h>
#include <pspinit.h>
#include <psprtc.h>
#include <string.h>
#include <systemctrl.h>
#include <systemctrl_se.h>
#include "track.h"

PSP_MODULE_INFO("Runtime", 0x1007, 1, 0);

#define GA_VERSION   "1.0"
#define GA_STAMP     __DATE__ " " __TIME__
#define GA_DIR       "ms0:/RUNTIME"
#define GA_REC       GA_DIR "/uptime.txt"
#define GA_ENDED     GA_DIR "/battery.log"
#define GA_USE       GA_DIR "/battery_use.log"
#define GA_USE_OLD   GA_DIR "/battery_use.old"
#define GA_DIAG      GA_DIR "/runtime.log"
#define GA_USE_MAX   (128 * 1024)
#define GA_EVERY     (120 * 1000 * 1000)          /* microseconds between looks */

static int (*g_pct)(void);
static int (*g_charging)(void);
static int  g_first = 1;                          /* no look has been written since this plugin was loaded */
static char g_program[48];

static void find_power(void)
{
    if (!g_pct) g_pct = (void *)sctrlHENFindFunction("scePower_Service", "scePower", 0x2085D15D);
    if (!g_charging) g_charging = (void *)sctrlHENFindFunction("scePower_Service", "scePower", 0x1E490401);
}

static gu32 now_minutes(void)
{
    ScePspDateTime t;
    memset(&t, 0, sizeof(t));
    if (sceRtcGetCurrentClockLocalTime(&t) < 0) return 0;
    return ga_minutes(t.year, t.month, t.day, t.hour, t.minute);
}

static int read_text(const char *path, char *buf, int size)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return -1;
    int n = sceIoRead(fd, buf, size - 1);
    sceIoClose(fd);
    buf[n > 0 ? n : 0] = 0;
    return n;
}

static int write_text(const char *path, const char *text, int flags)
{
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | flags, 0777);
    if (fd < 0) return -1;
    int len = (int)strlen(text), n = sceIoWrite(fd, text, len);
    sceIoClose(fd);
    return n == len ? 0 : -1;
}

static void add_line(const char *path, char *line, int size)
{
    int n = (int)strlen(line);
    if (n > size - 3) n = size - 3;
    line[n] = '\r'; line[n + 1] = '\n'; line[n + 2] = 0;
    write_text(path, line, PSP_O_APPEND);
}

/* what is running, for the use log: "XMB", the ISO's file name, or the folder of an EBOOT */
static void name_program(void)
{
    const char *exe = sceKernelInitFileName();
    const char *iso = sctrlSEGetUmdFile();
    const char *src = NULL, *pre = "";
    int key = sceKernelInitKeyConfig();
    g_program[0] = 0;
    if (key == PSP_INIT_KEYCONFIG_VSH) { ga_put(g_program, sizeof(g_program), 0, "XMB"); return; }
    if (key == PSP_INIT_KEYCONFIG_POPS) pre = "PS1 ";
    if (exe && (strstr(exe, "VBOOT.PBP") || strstr(exe, "/ARK_01234/"))) { ga_put(g_program, sizeof(g_program), 0, "launcher"); return; }
    if (iso && iso[0] && key != PSP_INIT_KEYCONFIG_POPS) {
        src = strrchr(iso, '/'); src = src ? src + 1 : iso;
    } else if (exe && exe[0]) {
        /* ms0:/PSP/GAME/<folder>/EBOOT.PBP: the folder says more than the file */
        const char *end = strrchr(exe, '/'), *a = exe;
        if (end) { for (const char *c = exe; c < end; c++) if (*c == '/') a = c + 1; }
        static char folder[40];
        int n = end && end > a ? (int)(end - a) : 0;
        if (n > 0 && n < (int)sizeof(folder)) { memcpy(folder, a, n); folder[n] = 0; src = folder; }
        else src = exe;
    }
    int p = ga_put(g_program, sizeof(g_program), 0, pre);
    ga_put(g_program, sizeof(g_program), p, src ? src : "?");
}

/*
 * The Memory Stick and Sidequest. A state holds the system's whole memory, the
 * file system's included: a write that is half done when the state is taken
 * would be finished, after a load, on a stick that has changed since. So the
 * two sides shake hands:
 *   - we say when we are writing (runtimeCardWriting, which Sidequest asks
 *     before it sleeps: it waits for us, or takes no state);
 *   - Sidequest says when it is at the stick (sidequestCardBusy: 1 while it
 *     takes or loads a state, 2 after a load that could not refresh the
 *     stick), and then we do not start a look.
 * We raise our mark first and ask second; Sidequest raises its own first and
 * asks second, so one of the two always sees the other. Without Sidequest, or
 * with one older than this, everything is as before.
 */
static volatile int g_card_writing;
static int g_card_held;                         /* looks put off, for one line in runtime.log afterwards */

int runtimeCardWriting(void) { return g_card_writing; }

/* Looked up every time: Sidequest loads after us, or not at all. */
static int card_begin(void)
{
    g_card_writing = 1;
    int (*busy)(void) = (void *)sctrlHENFindFunction("Sidequest", "SidequestLib", 0x5C4B5001);
    if (!busy || !busy()) return 0;
    g_card_writing = 0;
    g_card_held++;
    return -1;
}

static int look_inner(void);

/* One look. 0: the record was written. -2: Sidequest is at the stick, nothing was read or written. */
static int look(void)
{
    if (card_begin() < 0) return -2;
    sceIoMkdir(GA_DIR, 0777);
    int r = look_inner();
    if (g_card_held && r == 0) {                /* said once, when the stick is ours again */
        char line[96];
        int p = ga_put(line, sizeof(line), 0, "card guard: looks put off while Sidequest was at the stick: ");
        char num[12]; int n = g_card_held, i = sizeof(num) - 1;
        num[i] = 0;
        do { num[--i] = (char)('0' + n % 10); n /= 10; } while (n && i > 0);
        p = ga_put(line, sizeof(line), p, num + i);
        ga_put(line, sizeof(line), p, "\r\n");
        write_text(GA_DIAG, line, PSP_O_APPEND);
        g_card_held = 0;
    }
    g_card_writing = 0;
    return r;
}

static int look_inner(void)
{
    char buf[96], line[200];
    GaRec r;
    gu32 now = now_minutes();
    if (!now) return -1;
    find_power();
    int pct = g_pct ? g_pct() : -1;
    int chg = g_charging ? g_charging() > 0 : 0;

    ga_parse(read_text(GA_REC, buf, sizeof(buf)) > 0 ? buf : NULL, &r);
    int what = ga_step(&r, now, pct, g_first, line, sizeof(line) - 2);
    if (line[0]) add_line(GA_ENDED, line, sizeof(line));
    if (ga_use_line(&r, now, pct, chg, what, g_first ? g_program : NULL, line, sizeof(line) - 2)) {
        SceIoStat st;
        if (sceIoGetstat(GA_USE, &st) >= 0 && st.st_size > GA_USE_MAX) {
            sceIoRemove(GA_USE_OLD);
            if (sceIoRename(GA_USE, GA_USE_OLD) < 0) sceIoRemove(GA_USE);
        }
        add_line(GA_USE, line, sizeof(line));
    }
    ga_format(&r, buf, sizeof(buf));
    if (write_text(GA_REC, buf, PSP_O_TRUNC) < 0) return -1;
    if (g_first) {                                 /* once per load: what this build found, for the first runs on a real PSP */
        int p = ga_put(line, sizeof(line), 0, "Runtime " GA_VERSION " (built " GA_STAMP ") in ");
        p = ga_put(line, sizeof(line), p, g_program);
        p = ga_put(line, sizeof(line), p, g_pct ? ", battery reading found" : ", battery reading NOT found");
        p = ga_put(line, sizeof(line), p, what == GA_NEW ? ", new power-on" : ", same power-on");
        ga_put(line, sizeof(line), p, "\r\n");
        write_text(GA_DIAG, line, PSP_O_TRUNC);
    }
    g_first = 0;
    return 0;
}

static int runtime_thread(SceSize args, void *argp)
{
    /* The XMB loads plugins before the memory stick and the clock's time zone are ready. */
    while (g_first) {
        sceKernelDelayThread(10 * 1000 * 1000);
        look();
    }
    for (;;) {
        sceKernelDelayThread(GA_EVERY);
        for (int i = 0; i < 30 && look() == -2; i++)    /* a state is being taken or loaded: look again when it is over */
            sceKernelDelayThread(2 * 1000 * 1000);
    }
    return 0;
}

int module_start(SceSize args, void *argp)
{
    name_program();
    /* In a game or in Shelf the stick is ready: look now, so the record is fresh
     * before the program starts (Shelf reads it at once, and a state cannot be loaded sooner). */
    if (sceKernelInitKeyConfig() != PSP_INIT_KEYCONFIG_VSH) {
        look();
    }
    SceUID th = sceKernelCreateThread("Runtime", runtime_thread, 0x18, 0x4000, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    return 0;
}

int module_stop(SceSize args, void *argp) { return 0; }
