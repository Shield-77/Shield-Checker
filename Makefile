ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set)
endif

include $(DEVKITARM)/3ds_rules

TARGET := ShieldChecker
BUILD := build
APP_TITLE := Shield Checker
APP_AUTHOR := Yanis
APP_DESCRIPTION := Verification en lecture seule de l'etat systeme 3DS
CIA_VERSION := 7
MAKEROM := tools/makerom.exe
BANNERTOOL := tools/bannertool.exe
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := -O2 -g -Wall -Wextra -mword-relocations -ffunction-sections -fdata-sections $(ARCH)
CFLAGS += -Iinclude -I$(DEVKITPRO)/libctru/include -I$(DEVKITPRO)/portlibs/3ds/include -D__3DS__
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(BUILD)/$(TARGET).map
LIBS := -L$(DEVKITPRO)/portlibs/3ds/lib -L$(DEVKITPRO)/libctru/lib -lcurl -ljansson -lmbedtls -lmbedx509 -lmbedcrypto -lz -lctru -lm

.PHONY: all cia clean
ROMFS_FILES := $(filter-out romfs/bgm64.ogg,$(wildcard romfs/*.*))
all: cia
cia: $(BUILD)/$(TARGET).cia

$(BUILD)/romfs-lite.stamp: $(ROMFS_FILES)
	@mkdir -p $(BUILD)/romfs-lite
	@cp $(ROMFS_FILES) $(BUILD)/romfs-lite/
	@touch $@

$(BUILD)/$(TARGET).cia: $(BUILD)/$(TARGET).elf $(BUILD)/romfs-lite.stamp Makefile app.rsf icon.png banner.png assets/silence.wav $(MAKEROM) $(BANNERTOOL)
	@"$(BANNERTOOL)" makesmdh -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" -i icon.png -o $(BUILD)/icon.icn
	@"$(BANNERTOOL)" makebanner -i banner.png -a assets/silence.wav -o $(BUILD)/banner.bnr
	@"$(MAKEROM)" -f cia -o "$@.tmp" -target t -ver $(CIA_VERSION) -desc app:4 -rsf app.rsf -exefslogo -elf $(BUILD)/$(TARGET).elf -icon $(BUILD)/icon.icn -banner $(BUILD)/banner.bnr
	@mv -f "$@.tmp" "$@"

$(BUILD)/$(TARGET).elf: source/main.c source/diagnostics.c source/diagnostics.h romfs/font.bin romfs/ca-bundle.pem Makefile
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o "$@" source/main.c source/diagnostics.c $(LIBS)

clean:
	@rm -rf $(BUILD)
