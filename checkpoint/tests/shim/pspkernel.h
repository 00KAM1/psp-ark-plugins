#pragma once
#include "psptypes.h"
#include "pspiofilemgr.h"

SceUID sceKernelAllocPartitionMemory(int part, const char *name, int type, unsigned int size, void *addr);
void *sceKernelGetBlockHeadAddr(SceUID id);
int sceKernelFreePartitionMemory(SceUID id);
#ifndef PSP_MEMORY_PARTITION_KERNEL
#define PSP_MEMORY_PARTITION_KERNEL 1
#define PSP_SMEM_High 1
#endif
