TARGET = emblem
OBJS = src/main.o src/hook.o src/draw.o src/ach.o src/em.o src/view.o src/util.o exports.o

CFLAGS = -O2 -G0 -Wall -Wextra -Werror -Wno-unused-parameter -fno-pic -fno-builtin-printf -std=gnu99
ASFLAGS = $(CFLAGS)

BUILD_PRX = 1
USE_KERNEL_LIBS = 1
PSP_FW_VERSION = 661

LDFLAGS = -nostartfiles
LIBS = -lpspsystemctrl_kernel -lpspctrl_driver -lpspdebug -lpsprtc_driver -lpspinterruptmanager_kernel_660 \
       -lpspthreadman_kernel_660 -lpspsysmem_kernel_660 -lpsploadcore_kernel_660

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak

EXTRA_CLEAN += exports.c
exports.o: exports.c
$(OBJS): $(wildcard src/*.h)
