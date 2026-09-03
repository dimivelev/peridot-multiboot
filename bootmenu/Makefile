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

# CI smoke test via qemu-user (no device needed)
smoketest: static
	qemu-aarch64 -L /usr/aarch64-linux-musl ./bootmenu --help || true

clean:
	rm -f bootmenu

.PHONY: all static clean smoketest
