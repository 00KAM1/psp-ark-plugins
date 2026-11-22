/* pieces of util.c the vault needs, for the PC build */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "../src/common.h"
BtConfig g_cfg;
int sq_snprintf(char *buf, int size, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, size, fmt, ap); va_end(ap); return r; }
