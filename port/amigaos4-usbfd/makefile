#
# Makefile for bt.usbfd
#
# Bluetooth USB hotplug helper: starts the Bluetooth service when a controller
# is plugged in. The service itself is a separate program, see README.md.
#

TARGET = bt.usbfd

CC      = ppc-amigaos-gcc
STRIP   = ppc-amigaos-strip
CRT    ?= newlib

CFLAGS  = -mcrt=$(CRT) -mstrict-align -Wall -Wwrite-strings \
          -O3 -fomit-frame-pointer \
          -I. -D__NOLIBBASE__ -D__NOGLOBALIFACE__

LDFLAGS = -mcrt=$(CRT) -nostartfiles -Wl,--cref,-M,-Map=$(TARGET).map

SRCS = init.c \
       fdmain.c \
       main/obtain.c \
       main/release.c \
       main/getattrsa.c \
       main/runfunction.c \
       main/runinterface.c

OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(TARGET).debug
	$(STRIP) -R.comment $< -o $@

$(TARGET).debug: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	copy $(TARGET) DEVS:USB/fdclasses/
	copy bt.fdclass DEVS:USB/fdclasses/

clean:
	rm -f $(TARGET) $(TARGET).debug $(TARGET).map $(OBJS)
	@echo "Done!!"

.PHONY: all clean install
