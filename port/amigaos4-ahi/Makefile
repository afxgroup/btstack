#
# Makefile for bluetooth.audio - AHI sub-driver that plays through a Bluetooth sink
#
# Cross-compilation for AmigaOS 4 (PowerPC). Laid out like the USB Audio
# driver's, which this was built from.
#

CROSS_COMPILE = ppc-amigaos-
CC      = $(CROSS_COMPILE)gcc
STRIP   = $(CROSS_COMPILE)strip
HOSTCC  = gcc
RM      = rm -f
CRT    ?= newlib

# includes/ has our headers; the usbfd folder has the protocol shared with
# BluetoothService, which is where BTAudioRing and the commands are defined
INCPATH = -I. -Iincludes -I../amigaos4-usbfd

OPTIMIZE = -O2

# Uncomment for a build that writes its diagnostics to the serial console.
#DEBUG  = -gstabs -DDEBUG

# -ffreestanding and -fno-tree-loop-distribute-patterns: no CRT runs here, so
# INewlib is NULL and any call into libc faults. Memory work goes through
# IUtility->ClearMem and IExec->CopyMem, and these flags stop the compiler
# turning ordinary loops into the libc calls we are avoiding.
CFLAGS = -mcrt=$(CRT) -mstrict-align -ffreestanding -fno-tree-loop-distribute-patterns -Wall -Wwrite-strings $(DEBUG) $(OPTIMIZE) $(INCPATH)
LINK   = -mcrt=$(CRT) -nostartfiles

TARGET   = bluetooth.audio
MODEFILE = BLUETOOTH

OBJS = \
    DevInit/bluetooth_audio_init.o \
    device/bluetooth_audio_main.o \
    hw/bluetooth_play_proc.o

###################################################################

.PHONY: all clean install

all: $(TARGET) $(MODEFILE)

$(TARGET): $(OBJS)
	$(CC) $(LINK) -o $(TARGET).debug $(OBJS) -Wl,--cref,-M,-Map=$@.map
	$(STRIP) -R.comment $(TARGET).debug -o $(TARGET)

$(MODEFILE): gen_modefile
	./gen_modefile

gen_modefile: gen_modefile.c
	$(HOSTCC) -o $@ $<

clean:
	$(RM) $(TARGET) $(TARGET).debug $(TARGET).map $(MODEFILE) gen_modefile $(OBJS)
	@echo "Done!!"

install:
	copy $(TARGET) DEVS:AHI/
	copy $(MODEFILE) DEVS:AudioModes/
	@echo "Done!!"

###################################################################

DevInit/bluetooth_audio_init.o: DevInit/bluetooth_audio_init.c \
    includes/bluetooth_audio.h includes/bluetooth.audio_rev.h \
    DevInit/bluetooth_audio_vectors.c
	$(CC) -c $(CFLAGS) DevInit/bluetooth_audio_init.c -o $@

device/bluetooth_audio_main.o: device/bluetooth_audio_main.c \
    includes/bluetooth_audio.h includes/bluetooth.audio_rev.h
	$(CC) -c $(CFLAGS) device/bluetooth_audio_main.c -o $@

hw/bluetooth_play_proc.o: hw/bluetooth_play_proc.c \
    includes/bluetooth_audio.h
	$(CC) -c $(CFLAGS) hw/bluetooth_play_proc.c -o $@
