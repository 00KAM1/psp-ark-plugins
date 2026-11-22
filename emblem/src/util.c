/*
 * Emblem - log and a small vsnprintf (the kernel C library has no va_list
 * form). The formatter is Checkpoint's.
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdarg.h>
#include <string.h>
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

/* Appends a line to ms0:/ACHIEVE/achieve.log. Never called from the display hook. */
void ac_log(const char *fmt, ...)
{
    char line[220];
    va_list ap; va_start(ap, fmt);
    int n = ac_vsnprintf(line, (int)sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 3) n = (int)sizeof(line) - 3;
    line[n++] = '\r'; line[n++] = '\n'; line[n] = 0;
    SceUID fd = sceIoOpen(AC_LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) { sceIoWrite(fd, line, n); sceIoClose(fd); }
}
