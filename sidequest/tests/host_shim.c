/* Maps sceIo* to POSIX, "ms0:/" -> ./ms0/ */
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "shim/pspiofilemgr.h"
int g_fail_write_after = -1;   /* inject a short write after N writes */
static void map(const char *p, char *out){ snprintf(out, 512, "./%s", p); char *c = strchr(out, ':'); if (c) memmove(c, c + 1, strlen(c)); }
SceUID sceIoOpen(const char *p, int fl, int m){ char q[512]; map(p,q); int f=0;
  if ((fl&3)==1) f=O_RDONLY; else if ((fl&3)==2) f=O_WRONLY; else f=O_RDWR;
  if (fl&PSP_O_CREAT) f|=O_CREAT; if (fl&PSP_O_TRUNC) f|=O_TRUNC; if (fl&PSP_O_APPEND) f|=O_APPEND;
  int fd=open(q,f,0644); return fd<0? -1: fd; }
int sceIoRead(SceUID fd, void *b, SceSize n){ return read(fd,b,n); }
int sceIoWrite(SceUID fd, const void *b, SceSize n){ if (g_fail_write_after==0) return n/2; if (g_fail_write_after>0) g_fail_write_after--; return write(fd,b,n); }
int sceIoClose(SceUID fd){ return close(fd); }
int sceIoLseek32(SceUID fd, int o, int w){ return lseek(fd,o,w); }
int sceIoRemove(const char *p){ char q[512]; map(p,q); return unlink(q); }
int sceIoRename(const char *a, const char *b){ char x[512],y[512]; map(a,x); map(b,y); return rename(x,y); }
int sceIoMkdir(const char *p, int m){ char q[512]; map(p,q); return mkdir(q,0755); }

int sceIoRmdir(const char *p){ char q[512]; map(p,q); return rmdir(q); }
