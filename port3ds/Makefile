# Nintendo 3DS build (old 3DS compatible).
#   1. dotnet run --project cgen -- ../config/sotn.json build/gen   (or ./build_3ds.sh)
#   2. make -j$(nproc)
# Output: build/3ds/SymphonyRecomp.3dsx

DEVKITPRO ?= /c/devkitPro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
ROOT      := $(abspath ..)
GEN       := build/gen
OBJ       := build/3ds
PY        := python
TMP       ?= build/tmp
export TMP
export TEMP := $(TMP)

CC        := $(DEVKITARM)/bin/arm-none-eabi-gcc
TOOLS     := $(DEVKITPRO)/tools/bin

ARCH      := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -marm
CFLAGS    := $(ARCH) -O2 -std=gnu11 -mword-relocations -ffunction-sections -fomit-frame-pointer \
             -fno-strict-aliasing -D__3DS__ -DRT_FAST_NO_DITHER=1 -Iruntime/include -I$(DEVKITPRO)/libctru/include \
             -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable
GENFLAGS  := $(ARCH) -O2 -std=gnu11 -mword-relocations -ffunction-sections -fomit-frame-pointer \
             -fno-strict-aliasing -Iruntime/include -w
LDFLAGS   := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections -L$(DEVKITPRO)/libctru/lib
LIBS      := -lctru -lm

include $(GEN)/sources.mk

RT_SRC    := $(wildcard runtime/src/*.c)
RT_OBJ    := $(patsubst runtime/src/%.c,$(OBJ)/rt_%.o,$(RT_SRC))
GEN_OBJ   := $(patsubst %.c,$(OBJ)/gen_%.o,$(GEN_SOURCES))

APP_TITLE  := SymphonyRecomp
APP_DESC   := Castlevania SOTN static recompilation
APP_AUTHOR := SymphonyRecomp 3DS port

all: $(OBJ)/$(APP_TITLE).3dsx

$(OBJ)/fonts.c: $(ROOT)/RecompOne/RecompOne.Runtime/Bios/Fonts/font1.raw
	@mkdir -p $(OBJ)
	$(PY) tools/bin2c.py $@ g_font1=$(ROOT)/RecompOne/RecompOne.Runtime/Bios/Fonts/font1.raw g_font2=$(ROOT)/RecompOne/RecompOne.Runtime/Bios/Fonts/font2.raw

$(OBJ)/fonts.o: $(OBJ)/fonts.c
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/rt_%.o: runtime/src/%.c runtime/include/rt.h runtime/include/recomp.h runtime/src/gpu_fast.inc runtime/src/gpu_core.inc
	@mkdir -p $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/gen_%.o: $(GEN)/%.c runtime/include/recomp.h
	@mkdir -p $(OBJ)
	@echo gen $<
	@$(CC) $(GENFLAGS) -c $< -o $@

$(OBJ)/host_3ds.o: runtime/host/host_3ds.c runtime/include/rt.h
	@mkdir -p $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/$(APP_TITLE).elf: $(RT_OBJ) $(GEN_OBJ) $(OBJ)/fonts.o $(OBJ)/host_3ds.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS) -Wl,-Map,$(OBJ)/$(APP_TITLE).map

$(OBJ)/$(APP_TITLE).smdh: assets/icon.png
	$(TOOLS)/smdhtool --create "$(APP_TITLE)" "$(APP_DESC)" "$(APP_AUTHOR)" $< $@

$(OBJ)/$(APP_TITLE).3dsx: $(OBJ)/$(APP_TITLE).elf $(OBJ)/$(APP_TITLE).smdh
	$(TOOLS)/3dsxtool $< $@ --smdh=$(OBJ)/$(APP_TITLE).smdh
	@$(DEVKITARM)/bin/arm-none-eabi-size $<

clean:
	rm -rf $(OBJ)

.PHONY: all clean
