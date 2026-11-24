#pragma once
#include "psptypes.h"
#include "pspiofilemgr.h"
int sceKernelCpuSuspendIntr(void);
void sceKernelCpuResumeIntr(int flags);
int sceKernelDelayThread(unsigned int us);
