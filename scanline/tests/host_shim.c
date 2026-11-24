/* sceIo* on top of POSIX: "ms0:/X" is ./ms0/X */
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "shim/pspiofilemgr.h"
static void map(const char *p, char *out){ snprintf(out, 512, "./%s", p); char *c = strchr(out, ':'); if (c) memmove(c, c + 1, strlen(c)); }
SceUID sceIoOpen(const char *p, int fl, int m){ char q[512]; map(p,q); int f;
  if ((fl&3)==1) f=O_RDONLY; else if ((fl&3)==2) f=O_WRONLY; else f=O_RDWR;
  if (fl&PSP_O_CREAT) f|=O_CREAT; if (fl&PSP_O_TRUNC) f|=O_TRUNC; if (fl&PSP_O_APPEND) f|=O_APPEND;
  int fd=open(q,f,0644); return fd<0? -1: fd; }
int sceIoRead(SceUID fd, void *b, SceSize n){ return read(fd,b,n); }
int sceIoWrite(SceUID fd, const void *b, SceSize n){ return write(fd,b,n); }
int sceIoClose(SceUID fd){ return close(fd); }
int sceIoRemove(const char *p){ char q[512]; map(p,q); return unlink(q); }
int sceIoRename(const char *a, const char *b){ char x[512],y[512]; map(a,x); map(b,y); return rename(x,y); }
int sceIoMkdir(const char *p, int m){ char q[512]; map(p,q); return mkdir(q,0755); }

/* ---- Sidequest's "is the stick yours?" answer, set by the tests ----------- */
int host_sidequest = -1;                        /* -1: no Sidequest loaded; else what sidequestCardBusy() returns */
int host_busy_for = 0;                          /* > 0: that many more delays, then Sidequest is done */
int host_delays = 0;
static int host_busy(void) { return host_sidequest; }
unsigned long sctrlHENFindFunction(const char *mod, const char *lib, unsigned int nid)
{
    if (host_sidequest < 0 || strcmp(mod, "Sidequest") || strcmp(lib, "SidequestLib") || nid != 0x5C4B5001) return 0;
    return (unsigned long)host_busy;
}
int sceKernelCpuSuspendIntr(void) { return 0; }
void sceKernelCpuResumeIntr(int flags) { (void)flags; }
int sceKernelDelayThread(unsigned int us)
{
    (void)us; host_delays++;
    if (host_busy_for > 0 && --host_busy_for == 0) host_sidequest = 0;
    return 0;
}
