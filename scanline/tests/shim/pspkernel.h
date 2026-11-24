#pragma once
#include "psptypes.h"
#include "pspiofilemgr.h"
int sceKernelCpuSuspendIntr(void);
void sceKernelCpuResumeIntr(int flags);
int sceKernelDelayThread(unsigned int us);
SceUID sceKernelAllocPartitionMemory(int part, const char *name, int type, unsigned int size, void *addr);
void *sceKernelGetBlockHeadAddr(SceUID id);
int sceKernelFreePartitionMemory(SceUID id);
