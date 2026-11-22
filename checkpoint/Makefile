TARGET = checkpoint
OBJS = src/main.o src/util.o src/vault.o src/journey.o src/jtext.o src/jfont_data.o src/hook.o src/blit.o src/ui.o exports.o

CFLAGS = -O2 -G0 -Wall -Wextra -Wno-unused-parameter -fno-pic -fno-builtin-printf -std=gnu99
ASFLAGS = $(CFLAGS)

BUILD_PRX = 1
USE_KERNEL_LIBS = 1
PSP_FW_VERSION = 661

LDFLAGS = -nostartfiles
LIBS = -lpspsystemctrl_kernel -lpsprtc_driver -lpspthreadman_kernel_660 -lpspsysmem_kernel_660 -lpsploadcore_kernel_660

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak

EXTRA_CLEAN += exports.c
exports.o: exports.c
$(OBJS): $(wildcard src/*.h)
