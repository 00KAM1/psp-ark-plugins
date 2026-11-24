/*
 * Checkpoint - logging, config and a tiny vsnprintf
 * (the kernel C library has snprintf but no va_list variant).
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdarg.h>
#include <string.h>
#include <systemctrl.h>
#include "common.h"

BtConfig g_cfg;

/* ------------------------------------------------------------------------ */
/* Minimal vsnprintf: %s %c %d %u %x %X %p, optional '0' flag and width.     */
/* ------------------------------------------------------------------------ */
static void put(char *buf, int size, int *pos, char c)
{
    if (*pos < size - 1) buf[*pos] = c;
    (*pos)++;
}

int sq_vsnprintf(char *buf, int size, const char *fmt, va_list ap)
{
    int pos = 0;
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(buf, size, &pos, *fmt); continue; }
        fmt++;
        int zero = 0, left = 0, width = 0, prec = -1;
        for (;; fmt++) {
            if (*fmt == '0') zero = 1;
            else if (*fmt == '-') left = 1;
            else break;
        }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++; prec = 0;
            while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        if (*fmt == 'l') fmt++;
        char tmp[16]; int n = 0;
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = strlen(s);
            if (prec >= 0 && len > prec) len = prec;
            if (!left) for (int i = len; i < width; i++) put(buf, size, &pos, ' ');
            for (int i = 0; i < len; i++) put(buf, size, &pos, s[i]);
            if (left) for (int i = len; i < width; i++) put(buf, size, &pos, ' ');
            continue;
        }
        case 'c': put(buf, size, &pos, (char)va_arg(ap, int)); continue;
        case 'd': case 'u': {
            int v = va_arg(ap, int);
            u32 u = (u32)v; int neg = 0;
            if (*fmt == 'd' && v < 0) { neg = 1; u = (u32)(-v); }
            do { tmp[n++] = '0' + (u % 10); u /= 10; } while (u);
            if (neg) tmp[n++] = '-';
            break;
        }
        case 'p': zero = 1; width = 8; /* fall through */
        case 'x': case 'X': {
            u32 u = va_arg(ap, u32);
            const char *dig = (*fmt == 'x') ? "0123456789abcdef" : "0123456789ABCDEF";
            do { tmp[n++] = dig[u & 15]; u >>= 4; } while (u);
            break;
        }
        case '%': put(buf, size, &pos, '%'); continue;
        default:  put(buf, size, &pos, *fmt); continue;
        }
        int digits = n;
        if (!left) for (int i = digits; i < width; i++) put(buf, size, &pos, zero ? '0' : ' ');
        while (n) put(buf, size, &pos, tmp[--n]);
        if (left) for (int i = digits; i < width; i++) put(buf, size, &pos, ' ');
    }
    if (size > 0) buf[pos < size ? pos : size - 1] = 0;
    return pos;
}

/* Copy that always terminates and never reads past the source's end. */
void sq_strcpy(char *dst, int size, const char *src)
{
    int i = 0;
    if (size <= 0) return;
    for (; src && src[i] && i < size - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

int sq_snprintf(char *buf, int size, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = sq_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

/* ------------------------------------------------------------------------ */
/* Logging: appends to ms0:/CHECKPOINT/checkpoint.log                        */
/* Never call from the suspend hook (the Memory Stick is asleep by then).    */
/* ------------------------------------------------------------------------ */
/* The Memory Stick and Sidequest.                                          */
/* A state holds the system's whole memory, the file system's included. A   */
/* write that is half done when the state is taken would be finished, after */
/* a load, on a stick that has changed since. So the two sides shake hands: */
/*   - we say when we are writing (checkpointCardWriting, which Sidequest asks  */
/*     before it sleeps: it waits for us, or takes no state);               */
/*   - Sidequest says when it is at the stick (sidequestCardBusy: 1 while   */
/*     it takes or loads a state, 2 after a load that could not refresh the */
/*     stick), and then we do not start a write.                            */
/* We raise our mark first and ask second; Sidequest raises its own first   */
/* and asks second, so one of the two always sees the other.                */
/* Without Sidequest, or with one older than this, everything is as before. */
/* ------------------------------------------------------------------------ */
static volatile int g_card_writing;
static volatile int g_card_held;                /* writes put off, for one line in the log afterwards */

int checkpointCardWriting(void) { return g_card_writing; }

static void card_mark(int d, int held)
{
    int k = sceKernelCpuSuspendIntr();
    g_card_writing += d;
    g_card_held += held;
    sceKernelCpuResumeIntr(k);
}

/* Looked up every time: Sidequest may load after us, or not at all. */
int bt_card_busy(void)
{
    int (*busy)(void) = (void *)sctrlHENFindFunction("Sidequest", "SidequestLib", 0x5C4B5001);
    return busy ? busy() : 0;
}

static int card_begin(int count)
{
    card_mark(1, 0);
    int b = bt_card_busy();
    if (!b) return 0;
    card_mark(-1, count);
    return b == 2 ? -2 : -1;
}

/* Before any write: 0 = go ahead, and call bt_card_end() when done. Below 0 =
 * Sidequest is at the stick: do not write now (-2: not again in this program). */
int  bt_card_begin(void) { return card_begin(1); }
void bt_card_end(void)   { card_mark(-1, 0); }

/* For a write that must not be lost: waits while a state is taken or loaded. */
int bt_card_wait(void)
{
    for (int i = 0; i < 150; i++) {             /* 15 s of time awake; a save or a load is over long before */
        int r = card_begin(i == 0);
        if (r == 0 || r == -2) return r;
        sceKernelDelayThread(100 * 1000);
    }
    return -1;
}

/* ------------------------------------------------------------------------ */
void sq_log(const char *fmt, ...)
{
    char line[352];
    if (card_begin(0) < 0) return;              /* a log line is not worth a write at the wrong moment */
    va_list ap; va_start(ap, fmt);
    int n = sq_vsnprintf(line, 254, fmt, ap);
    va_end(ap);
    if (n > 253) n = 253;
    line[n++] = '\r'; line[n++] = '\n'; line[n] = 0;
    if (g_card_held) {                          /* said once, when the stick is ours again */
        int h = g_card_held;
        card_mark(0, -h);
        n += sq_snprintf(line + n, (int)sizeof(line) - n, "card guard: %d write(s) put off while Sidequest was at the stick\r\n", h);
    }
    SceUID fd = sceIoOpen(BT_LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) { sceIoWrite(fd, line, n); sceIoClose(fd); }
    bt_card_end();
}

/* ------------------------------------------------------------------------ */
/* Config: ms0:/CHECKPOINT/config.txt, "key = value" lines, '#' comments.    */
/* ------------------------------------------------------------------------ */
static int parse_int(const char *s)
{
    int v = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        for (;; s++) {
            int d;
            if (*s >= '0' && *s <= '9') d = *s - '0';
            else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
            else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
            else break;
            v = v * 16 + d;
        }
    } else {
        while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    }
    return neg ? -v : v;
}

static void apply(const char *key, const char *val)
{
    int v = parse_int(val);
    if (!strcmp(key, "enabled"))            g_cfg.enabled = v != 0;
    else if (!strcmp(key, "keep"))        { if (v >= 1 && v <= 30) g_cfg.keep = v; }
    else if (!strcmp(key, "min_minutes")) { if (v >= 0 && v <= 600) g_cfg.min_minutes = v; }
    else if (!strcmp(key, "hotkey"))        g_cfg.hotkey = (u32)v;
    else if (!strcmp(key, "quit_screen"))   g_cfg.quit_screen = v != 0;
    else if (!strcmp(key, "warn_minutes")) { if (v >= 1 && v <= 600) g_cfg.warn_minutes = v; }
    else if (!strcmp(key, "journey"))        g_cfg.journey = v != 0;
    else if (!strcmp(key, "journey_detail")) g_cfg.journey_detail = v != 0;
    else if (!strcmp(key, "hold_ms"))     { if (v >= 50 && v <= 5000) g_cfg.hold_ms = v; }
}

void config_load(void)
{
    g_cfg.enabled = 1;
    g_cfg.keep = 10;
    g_cfg.min_minutes = 10;
    g_cfg.quit_screen = 1;
    g_cfg.warn_minutes = 10;
    g_cfg.journey = 1;
    g_cfg.journey_detail = 0;
    g_cfg.hotkey = 0x0301;                      /* L + R + SELECT */
    g_cfg.hold_ms = 100;                        /* feels instant, still ignores a slip */

    SceUID fd = sceIoOpen(BT_CONFIG_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) return;
    static char buf[2048];
    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    buf[n] = 0;

    char *line = buf;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *cr = strchr(line, '\r'); if (cr) *cr = 0;
        char *hash = strchr(line, '#'); if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            char *k = line, *v = eq + 1;
            while (*k == ' ' || *k == '\t') k++;
            char *e = k + strlen(k);
            while (e > k && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
            apply(k, v);
        }
        line = next;
    }
    g_cfg.hotkey &= 0xF3F9;
    if (!g_cfg.hotkey) g_cfg.hotkey = 0x0301;
}
