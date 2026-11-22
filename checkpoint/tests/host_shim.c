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
#include <dirent.h>
#include <stdlib.h>
static DIR *g_dirs[16]; static char g_dirpath[16][512];
SceUID sceIoDopen(const char *p){ char q[512]; map(p,q); DIR *d=opendir(q); if(!d) return -1;
  for(int i=0;i<16;i++) { if(!g_dirs[i]){ g_dirs[i]=d; strcpy(g_dirpath[i],q); return 1000+i; } }
  closedir(d); return -1; }
int sceIoDread(SceUID id, SceIoDirent *e){ int i=id-1000; struct dirent *de=readdir(g_dirs[i]); if(!de) return 0;
  char q[1024]; snprintf(q,sizeof q,"%s/%s",g_dirpath[i],de->d_name); struct stat st; stat(q,&st);
  strcpy(e->d_name,de->d_name); e->d_stat.st_mode=S_ISDIR(st.st_mode)?FIO_S_IFDIR:0x2000; e->d_stat.st_size=st.st_size; return 1; }
int sceIoDclose(SceUID id){ int i=id-1000; closedir(g_dirs[i]); g_dirs[i]=NULL; return 0; }
int sceIoRmdir(const char *p){ char q[512]; map(p,q); return rmdir(q); }
int sceIoGetstat(const char *p, SceIoStat *s){ char q[512]; map(p,q); struct stat st; if(stat(q,&st)<0) return -1; s->st_size=st.st_size; s->st_mode=S_ISDIR(st.st_mode)?FIO_S_IFDIR:0x2000; return 0; }
