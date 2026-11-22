/* sceIo* on POSIX: "ms0:/X" is ./ms0/X; plus the plugin's formatter from libc */
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include "shim/pspiofilemgr.h"
int g_fail_write;
static void map(const char *p, char *out){ snprintf(out, 512, "./%s", p); char *c = strchr(out, ':'); if (c) memmove(c, c + 1, strlen(c)); }
SceUID sceIoOpen(const char *p, int fl, int m){ char q[512]; map(p,q); int f;
  if ((fl&3)==1) f=O_RDONLY; else if ((fl&3)==2) f=O_WRONLY; else f=O_RDWR;
  if (fl&PSP_O_CREAT) f|=O_CREAT; if (fl&PSP_O_TRUNC) f|=O_TRUNC; if (fl&PSP_O_APPEND) f|=O_APPEND;
  int fd=open(q,f,0644); return fd<0? -1: fd; }
int sceIoRead(SceUID fd, void *b, SceSize n){ return read(fd,b,n); }
int sceIoWrite(SceUID fd, const void *b, SceSize n){ if (g_fail_write) return n/2; return write(fd,b,n); }
int sceIoClose(SceUID fd){ return close(fd); }
int sceIoGetstat(const char *p, SceIoStat *s){ char q[512]; map(p,q); struct stat st; if(stat(q,&st)<0) return -1; s->st_size=st.st_size; return 0; }
int ac_snprintf(char *buf, int size, const char *fmt, ...){ va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, size, fmt, ap); va_end(ap); return r; }
int sceIoRemove(const char *p){ char q[512]; map(p,q); return unlink(q); }
