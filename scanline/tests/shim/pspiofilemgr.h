#pragma once
#include "psptypes.h"
#define PSP_O_RDONLY 1
#define PSP_O_WRONLY 2
#define PSP_O_CREAT  0x200
#define PSP_O_APPEND 0x100
#define PSP_O_TRUNC  0x400
SceUID sceIoOpen(const char*, int, int); int sceIoRead(SceUID, void*, SceSize);
int sceIoWrite(SceUID, const void*, SceSize); int sceIoClose(SceUID);
int sceIoRemove(const char*); int sceIoRename(const char*, const char*); int sceIoMkdir(const char*, int);
