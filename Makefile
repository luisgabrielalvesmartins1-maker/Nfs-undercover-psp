TARGET = nfs_viewer
OBJS   = main.o

CFLAGS   = -O2 -G0 -Wall
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS  = $(CFLAGS)

LIBS = -lpspgum -lpspgu -lpspctrl -lpspdebug -lpspdisplay -lpspge -lpng -lz -lm

EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = NFS PSP

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
