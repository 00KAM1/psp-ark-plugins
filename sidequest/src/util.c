/*
 * Sidequest - logging, config, CRC32, and a tiny vsnprintf
 * (the kernel C library has snprintf but no va_list variant).
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdarg.h>
#include <string.h>
#include "common.h"

SqConfig g_cfg;

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
/* Logging: appends to ms0:/SIDEQUEST/sidequest.log                        */
/* Never call from the suspend hook (the Memory Stick is asleep by then).    */
/* ------------------------------------------------------------------------ */
void sq_log(const char *fmt, ...)
{
    char line[256];
    va_list ap; va_start(ap, fmt);
    int n = sq_vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 3) n = sizeof(line) - 3;
    line[n++] = '\r'; line[n++] = '\n'; line[n] = 0;
    SceUID fd = sceIoOpen(SQ_LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) { sceIoWrite(fd, line, n); sceIoClose(fd); }
}

/* ------------------------------------------------------------------------ */
/* CRC32 (IEEE, reflected), table built on first use.                        */
/* ------------------------------------------------------------------------ */
static u32 crc_table[256];
static int crc_ready;

u32 crc32_update(u32 crc, const void *data, u32 len)
{
    if (!crc_ready) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            crc_table[i] = c;
        }
        crc_ready = 1;
    }
    const u8 *p = data;
    crc = ~crc;
    while (len--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ------------------------------------------------------------------------ */
/* Config: ms0:/SIDEQUEST/config.txt, "key = value" lines, '#' comments.    */
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
    if (!strcmp(key, "enable_load"))     g_cfg.enable_load = parse_int(val) != 0;
    else if (!strcmp(key, "resume_prompt")) g_cfg.resume_prompt = parse_int(val) != 0;
    else if (!strcmp(key, "load_mute"))  g_cfg.load_mute = parse_int(val) != 0;
    else if (!strcmp(key, "load_brightness")) { int b = parse_int(val); if (b >= 0 && b <= 100) g_cfg.load_brightness = b; }
    else if (!strcmp(key, "wake_seconds")) { int w = parse_int(val); if (w == 0 || (w >= 2 && w <= 30)) g_cfg.wake_seconds = w; }
    else if (!strcmp(key, "deep_sleep")) g_cfg.deep_sleep = parse_int(val) != 0;
    else if (!strcmp(key, "test"))       g_cfg.test = parse_int(val);
    else if (!strcmp(key, "hotkey"))     g_cfg.hotkey = (u32)parse_int(val);
    else if (!strcmp(key, "hold_ms")) {
        int v = parse_int(val);
        if (v >= 100 && v <= 5000) g_cfg.hold_ms = v;
    }
}

void config_load(void)
{
    g_cfg.enable_load = 1;
    g_cfg.resume_prompt = 1;
    g_cfg.load_mute = 1;
    g_cfg.load_brightness = 70;
    g_cfg.wake_seconds = 3;
    g_cfg.hold_ms = 1000;
    g_cfg.deep_sleep = 1;                       /* instant suspend breaks game exit on real hardware */
    g_cfg.test = 0;
    /* L + R + UP + SELECT. ARK-5 uses L+R+DOWN+SELECT/START for exit. */
    g_cfg.hotkey = 0x0001;                      /* SELECT */

    SceUID fd = sceIoOpen(SQ_CONFIG_PATH, PSP_O_RDONLY, 0);
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
    if (!g_cfg.hotkey) g_cfg.hotkey = 0x0001;
}
