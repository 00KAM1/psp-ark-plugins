TARGET = sidequest
OBJS = src/main.o src/util.o src/compress.o src/engine.o src/engine_asm.o src/staging.o src/flashfix.o \
       src/slots.o src/blit.o src/ui.o exports.o

CFLAGS = -O2 -G0 -Wall -Wextra -Wno-unused-parameter -fno-pic -fno-builtin-printf -std=gnu99
ASFLAGS = $(CFLAGS)

BUILD_PRX = 1
USE_KERNEL_LIBS = 1
PSP_FW_VERSION = 661

LDFLAGS = -nostartfiles
LIBS = -lpspsystemctrl_kernel -lpsppower_driver -lpsprtc_driver -lpspthreadman_kernel_660 \
       -lpspsysmem_kernel_660 -lpsploadcore_kernel_660

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak

# the export table (module_start, module_info, module_reboot_before) is generated from exports.exp
EXTRA_CLEAN += exports.c
exports.o: exports.c

# every object depends on every header, so a changed header always rebuilds everything
$(OBJS): $(wildcard src/*.h)
