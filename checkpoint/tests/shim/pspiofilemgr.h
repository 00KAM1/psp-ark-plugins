#pragma once
#include "psptypes.h"
#define PSP_O_RDONLY 1
#define PSP_O_WRONLY 2
#define PSP_O_CREAT  0x200
#define PSP_O_APPEND 0x100
#define PSP_O_TRUNC  0x400
#define PSP_SEEK_SET 0
SceUID sceIoOpen(const char*, int, int); int sceIoRead(SceUID, void*, SceSize);
int sceIoWrite(SceUID, const void*, SceSize); int sceIoClose(SceUID);
int sceIoLseek32(SceUID, int, int); int sceIoRemove(const char*);
int sceIoRename(const char*, const char*); int sceIoMkdir(const char*, int);
#define FIO_S_IFDIR 0x1000
typedef struct { int st_mode; unsigned int st_attr; long long st_size; } SceIoStat;
typedef struct { SceIoStat d_stat; char d_name[256]; void *d_private; int dummy; } SceIoDirent;
SceUID sceIoDopen(const char*); int sceIoDread(SceUID, SceIoDirent*); int sceIoDclose(SceUID);
int sceIoRmdir(const char*); int sceIoGetstat(const char*, SceIoStat*);
