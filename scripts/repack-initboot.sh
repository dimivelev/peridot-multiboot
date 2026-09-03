#!/usr/bin/env bash
# repack-initboot.sh — inject bootmenu as /init into an init_boot.img
#
# Usage: repack-initboot.sh <stock-init_boot.img> <bootmenu-binary> <out.img>
#
# How it works:
#   1. unpacks init_boot (header v4, generic ramdisk only)
#   2. renames /init -> /init.real, installs bootmenu binary as /init
#   3. repacks the ramdisk and rebuilds the image (header v4 preserved)
#
# Requires: AOSP tools via scripts/fetch-android-tools.sh, cpio, gzip, lz4

set -euo pipefail

IMG="$1"; MENU="$2"; OUT="$3"
ORIG_PWD=$(pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

bash "$(dirname "$0")/fetch-android-tools.sh" >/dev/null 2>&1 || true

# compression sniffing without the `file` utility
detect_extract() { # <archive> <dest-dir>
  local src="$1" dst="$2" magic
  magic=$(od -An -tx1 -N4 "$src" | tr -d ' \n')
  case "$magic" in
    1f8b*)    gzip -dc "$src" | (cd "$dst" && cpio -idm --quiet) ;;
    02214c18|04224d18) lz4 -dc "$src" | (cd "$dst" && cpio -idm --quiet) ;;
    *)        (cd "$dst" && cpio -idm --quiet < "$src") ;;
  esac
}

echo "[*] unpacking $IMG"
mkdir "$WORK/unpacked"
"${HOME}/.local/bin/unpack_bootimg" --boot_img "$IMG" --out "$WORK/unpacked" >/dev/null

RAMDISK=$(ls "$WORK/unpacked" | grep -E '^ramdisk' | head -1)
test -n "$RAMDISK" || { echo "ERROR: no ramdisk in image — is this really init_boot?" >&2; exit 1; }
echo "[*] ramdisk file: $RAMDISK"

mkdir "$WORK/rd"
cd "$WORK/rd"
detect_extract "$WORK/unpacked/$RAMDISK" "$WORK/rd"

test -f init || { echo "ERROR: no /init in ramdisk — not an init_boot image?" >&2; exit 1; }
mv init init.real
cp "$MENU" init
chmod 750 init
# menu needs these locations to exist pre-mount
mkdir -p metadata multiboot

find . | cpio -o -H newc --quiet | gzip -9 > "$WORK/rd.cpio.gz"

echo "[*] repacking $OUT"
cd "$ORIG_PWD"
"${HOME}/.local/bin/mkbootimg" \
  --ramdisk "$WORK/rd.cpio.gz" \
  --header_version 4 --os_version 14.0.0 --os_patch_level 2024-08-05 \
  --pagesize 4096 -o "$OUT"

echo "[+] done: $OUT (menu installed as /init, original init kept as /init.real)"
