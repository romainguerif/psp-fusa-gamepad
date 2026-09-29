TARGET = loader
OBJS   =  fusaGamePad.o main.o

INCDIR = 
CFLAGS = -O2 -G0 -Wall
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSP Bridge
PSP_EBOOT_ICON = icon0.png
PSP_EBOOT_PIC1 = pic1.png

LIBDIR =
LIBS =

PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
