TARGET = videobg
OBJS = src/main.o src/klog.o src/gedump.o

BUILD_PRX = 1
PRX_EXPORTS = exports.exp
USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1

INCDIR = src
CFLAGS = -O2 -G0 -Wall -fno-pic
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBDIR =
LDFLAGS = -nostartfiles
LIBS = -lpspsystemctrl_kernel -lpspctrl_driver -lpspdisplay_driver -lpsploadcore_kernel_660 -lpspsysmem_kernel_660 -lansic

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak
