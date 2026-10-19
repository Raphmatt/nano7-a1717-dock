# Builds build/a1717_patch.bin, a flat image to upload to LOAD_ADDR and call at LOAD_ADDR|1.
CROSS     ?= arm-none-eabi-
CC        := $(CROSS)gcc
OBJCOPY   := $(CROSS)objcopy
LOAD_ADDR ?= 0x0867F8E4

CFLAGS  := -mcpu=cortex-a8 -mthumb -Os -ffreestanding -nostdlib -fno-builtin \
           -ffunction-sections -fdata-sections -Wall -Wextra
LDFLAGS := -nostdlib -Wl,--gc-sections -Wl,--build-id=none -Wl,-T,src/link.ld \
           -Wl,--defsym=LOAD_ADDR=$(LOAD_ADDR)

all: build/a1717_patch.bin

build/a1717_patch.elf: src/a1717_patch.c src/link.ld
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/a1717_patch.c

build/a1717_patch.bin: build/a1717_patch.elf
	$(OBJCOPY) -O binary $< $@
	@echo "$@: $$(wc -c < $@ | tr -d ' ') bytes, load at $(LOAD_ADDR)"

clean:
	rm -rf build
.PHONY: all clean
