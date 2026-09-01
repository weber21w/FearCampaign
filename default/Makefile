###########################
# Fear Campaign Makefile
###########################

## General flags
PROJECT = FearCampaign
GAME = FearCampaign
MCU = atmega644
TARGET = $(GAME).elf
CC = avr-gcc

KERNEL_DIR = ../../../kernel
VIDEO_MODE_DIR = $(realpath ../videoMode)

ifeq ($(JAMMA),1)
INFO = ../gameinfo-jamma.properties
else
INFO = ../gameinfo.properties
endif

ifeq ($(strip $(UZEBIN_DIR)),)
PACKROM = ../../../bin/packrom
else
PACKROM = $(UZEBIN_DIR)packrom
endif

## Kernel settings
KERNEL_OPTIONS = -DVIDEO_MODE=0 -DVIDEO_MODE_PATH=$(VIDEO_MODE_DIR)
KERNEL_OPTIONS += -DINTRO_LOGO=0 -DNO_EEPROM_FORMAT=1
KERNEL_OPTIONS += -DCONTROLLERS_VSYNC_READ=0 -DSNES_MOUSE=0
KERNEL_OPTIONS += -DSOUND_MIXER=MIXER_TYPE_INLINE -DSOUND_CHANNEL_5_ENABLE=0
KERNEL_OPTIONS += -DUART=0 -DINCLUDE_DEFAULT_WAVES=0 -DENABLE_MIXER=0
KERNEL_OPTIONS += -DCUSTOM_SOUND_MIXER=1

ifeq ($(JAMMA),1)
KERNEL_OPTIONS += -DFEARCAMPAIGN_JAMMA=1
else
KERNEL_OPTIONS += -DFEARCAMPAIGN_JAMMA=0
endif

## Compiler and linker flags
COMMON = -mmcu=$(MCU)
CFLAGS = $(COMMON)
CFLAGS += -Wall -Wextra -gdwarf-2 -std=gnu99 -DF_CPU=28636360UL -Os
CFLAGS += -fsigned-char -ffunction-sections -fdata-sections -fno-toplevel-reorder
CFLAGS += -MD -MP -MT $(*F).o -MF dep/$(@F).d
CFLAGS += $(KERNEL_OPTIONS)

ASMFLAGS = $(CFLAGS) -x assembler-with-cpp -Wa,-gdwarf2

LDFLAGS = $(COMMON)
LDFLAGS += -Wl,-Map=$(GAME).map -Wl,-gc-sections

HEX_FLASH_FLAGS = -R .eeprom
HEX_EEPROM_FLAGS = -j .eeprom
HEX_EEPROM_FLAGS += --set-section-flags=.eeprom="alloc,load"
HEX_EEPROM_FLAGS += --change-section-lma .eeprom=0 --no-change-warnings

INCLUDES = -I"$(KERNEL_DIR)" -I".." -I"../videoMode"

## Objects
OBJECTS = uzeboxVideoEngineCore.o uzeboxCore.o pokeyMixerCore.o \
          uzeboxVideoEngine.o spiram.o \
          main.o missile.o input.o framebuffer.o video.o pokey.o highscore.o

## Build
all: $(TARGET) $(GAME).hex $(GAME).eep $(GAME).lss $(GAME).uze size

## Kernel files
uzeboxVideoEngineCore.o: $(KERNEL_DIR)/uzeboxVideoEngineCore.s
	$(CC) $(INCLUDES) $(ASMFLAGS) -c $< -o $@

uzeboxCore.o: $(KERNEL_DIR)/uzeboxCore.c
	$(CC) $(INCLUDES) $(CFLAGS) -Wno-unused-parameter -c $< -o $@

uzeboxVideoEngine.o: $(KERNEL_DIR)/uzeboxVideoEngine.c
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@

spiram.o: $(KERNEL_DIR)/spiram.s
	$(CC) $(INCLUDES) $(ASMFLAGS) -c $< -o $@

## Custom POKEY mixer
pokeyMixerCore.o: ../pokeyMixerCore.s ../data/pokey_poly17.inc
	$(CC) $(INCLUDES) $(ASMFLAGS) -c $< -o $@

## Game files
framebuffer.o: ../framebuffer.c ../data/ui.inc
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@

missile.o: ../missile.c ../data/logo.inc
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@

pokey.o: ../pokey.c ../data/pokey_sound.inc
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@

%.o: ../%.c
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@

## Link
$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@

%.hex: $(TARGET)
	avr-objcopy -O ihex $(HEX_FLASH_FLAGS) $< $@

%.eep: $(TARGET)
	-avr-objcopy $(HEX_EEPROM_FLAGS) -O ihex $< $@ || exit 0

%.lss: $(TARGET)
	avr-objdump -h -S $< > $@

%.uze: %.hex $(INFO)
	$(PACKROM) $< $@ $(INFO)

size: $(TARGET)
	@echo
	@avr-size -C --mcu=$(MCU) $(TARGET)

.PHONY: all clean jamma size

jamma:
	$(MAKE) clean
	$(MAKE) JAMMA=1 all

clean:
	-rm -rf $(OBJECTS) $(GAME).elf $(GAME).hex $(GAME).eep $(GAME).lss $(GAME).map $(GAME).uze dep/*

-include $(shell mkdir dep 2>/dev/null) $(wildcard dep/*)
