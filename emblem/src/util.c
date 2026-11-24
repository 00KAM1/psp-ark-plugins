/*
 * Emblem - log and a small vsnprintf (the kernel C library has no va_list
 * form). The formatter is Checkpoint's.
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdarg.h>
#include <string.h>
#include <systemctrl.h>
#include "common.h"

static void put(char *buf, int size, int *pos, char c)
{
    if (*pos < size - 1) buf[*pos] = c;
    (*pos)++;
}

static int ac_vsnprintf(char *buf, int size, const char *fmt, va_list ap)
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
        char tmp[16]; int n = 0;
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)strlen(s);
            if (prec >= 0 && len > prec) len = prec;
            if (!left) for (int i = len; i < width; i++) put(buf, size, &pos, ' ');
            for (int i = 0; i < len; i++) put(buf, size, &pos, s[i]);
            if (left) for (int i = len; i < width; i++) put(buf, size, &pos, ' ');
            continue;
        }
        case 'd': case 'u': {
            int v = va_arg(ap, int);
            u32 u = (u32)v; int neg = 0;
            if (*fmt == 'd' && v < 0) { neg = 1; u = (u32)(-v); }
            do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
            if (neg) tmp[n++] = '-';
            break;
        }
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

int ac_snprintf(char *buf, int size, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = ac_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

/* ------------------------------------------------------------------------ */
/* The Memory Stick and Sidequest.                                          */
/* A state holds the system's whole memory, the file system's included. A   */
/* write that is half done when the state is taken would be finished, after */
/* a load, on a stick that has changed since. So the two sides shake hands: */
/*   - we say when we are writing (emblemCardWriting, which Sidequest asks  */
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

int emblemCardWriting(void) { return g_card_writing; }

static void card_mark(int d, int held)
{
    int k = sceKernelCpuSuspendIntr();
    g_card_writing += d;
    g_card_held += held;
    sceKernelCpuResumeIntr(k);
}

/* Looked up every time: Sidequest may load after us, or not at all. */
int ac_card_busy(void)
{
    int (*busy)(void) = (void *)sctrlHENFindFunction("Sidequest", "SidequestLib", 0x5C4B5001);
    return busy ? busy() : 0;
}

static int card_begin(int count)
{
    card_mark(1, 0);
    int b = ac_card_busy();
    if (!b) return 0;
    card_mark(-1, count);
    return b == 2 ? -2 : -1;
}

/* Before any write: 0 = go ahead, and call ac_card_end() when done. Below 0 =
 * Sidequest is at the stick: do not write now (-2: not again in this program). */
int  ac_card_begin(void) { return card_begin(1); }
void ac_card_end(void)   { card_mark(-1, 0); }

/* For a write that must not be lost: waits while a state is taken or loaded. */
int ac_card_wait(void)
{
    for (int i = 0; i < 150; i++) {             /* 15 s of time awake; a save or a load is over long before */
        int r = card_begin(i == 0);
        if (r == 0 || r == -2) return r;
        sceKernelDelayThread(100 * 1000);
    }
    return -1;
}

/* Appends a line to ms0:/ACHIEVE/achieve.log. Never called from the display hook. */
void ac_log(const char *fmt, ...)
{
    char line[316];
    if (card_begin(0) < 0) return;              /* a log line is not worth a write at the wrong moment */
    va_list ap; va_start(ap, fmt);
    int n = ac_vsnprintf(line, 218, fmt, ap);
    va_end(ap);
    if (n > 217) n = 217;
    line[n++] = '\r'; line[n++] = '\n'; line[n] = 0;
    if (g_card_held) {                          /* said once, when the stick is ours again */
        int h = g_card_held;
        card_mark(0, -h);
        n += ac_snprintf(line + n, (int)sizeof(line) - n, "card guard: %d write(s) put off while Sidequest was at the stick\r\n", h);
    }
    SceUID fd = sceIoOpen(AC_LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) { sceIoWrite(fd, line, n); sceIoClose(fd); }
    ac_card_end();
}
