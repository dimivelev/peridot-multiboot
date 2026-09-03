# bootmenu — static aarch64 build
#   musl toolchain (recommended): make CC=aarch64-linux-musl-gcc static
#   zig:                          make CC="zig cc -target aarch64-linux-musl" static
#   NDK:                          make CC=aarch64-linux-android24-clang static LDFLAGS="-static"

CC      ?= clang
CFLAGS  ?= -Os -Wall -Wextra -Wno-unused-function
LDFLAGS ?= -static
STRIP   ?= strip

all: bootmenu

bootmenu: bootmenu.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

static: CFLAGS += -static -fno-stack-protector -fomit-frame-pointer
static: clean bootmenu
	$(STRIP) bootmenu || true

# menu-ramdisk.img: cpio with ONLY /init = bootmenu. Combined with the multiboot
# kernel into `menu-test-boot.img` — try the menu on real hardware with zero
# permanent changes:
#     fastboot boot menu-test-boot.img
# (menu shows, keys work or 5 s timeout, device reboots — nothing was flashed)
testramdisk: static
	mkdir -p build/initramfs
	cp bootmenu build/initramfs/init
	chmod 750 build/initramfs/init
	cd build/initramfs && find . | cpio -o -H newc --quiet | gzip -9 > "$(CURDIR)/menu-ramdisk.img"
	@echo "[+] $(CURDIR)/menu-ramdisk.img ready"

clean:
	rm -rf bootmenu build menu-ramdisk.img

.PHONY: all static clean smoketest
