#!/usr/bin/env bash
# repack-vendorboot.sh — inject multiboot.rc into a vendor_boot.img
#
# Usage: repack-vendorboot.sh <stock-vendor_boot.img> <out.img> [multiboot.rc]
#
# multiboot.rc (second stage): reads /metadata/multiboot/active and bind-mounts
# the chosen ROM's images (loop) over /system /product /vendor /system_ext.

set -euo pipefail

IMG="$1"; OUT="$2"; RC="${3:-$(dirname "$0")/multiboot.rc}"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

bash "$(dirname "$0")/fetch-android-tools.sh" >/dev/null 2>&1 || true

echo "[*] unpacking $IMG"
mkdir "$WORK/unpacked"
unpack_bootimg --boot_img "$IMG" --out "$WORK/unpacked"

mkdir "$WORK/rd"
cd "$WORK/rd"
# vendor ramdisk can be multiple pieces; repack the first, keep rest untouched
RD=$(ls "$WORK/unpacked" | grep -E '^vendor_ramdisk' | head -1)
case "$(file -b "$WORK/unpacked/$RD")" in
  *gzip*)  gzip -dc "$WORK/unpacked/$RD" | cpio -idm --quiet ;;
  *LZ4*)   lz4 -dc "$WORK/unpacked/$RD" | cpio -idm --quiet ;;
  *)       cpio -idm --quiet < "$WORK/unpacked/$RD" ;;
esac

test -d system/etc/init || mkdir -p system/etc/init
cp "$RC" system/etc/init/multiboot.rc

find . | cpio -o -H newc --quiet | gzip -9 > "$WORK/vr.cpio.gz"

echo "[*] repacking $OUT"
mkbootimg \
  --vendor_boot "$OUT" \
  --header_version 4 --pagesize 4096 \
  --vendor_ramdisk "$WORK/vr.cpio.gz" \
  --dtb "$WORK/unpacked/dtb" \
  $(test -f "$WORK/unpacked/vendor_ramdisk_table" && echo "--vendor_ramdisk_table $WORK/unpacked/vendor_ramdisk_table") \
  $(test -f "$WORK/unpacked/bootconfig" && echo "--bootconfig $WORK/unpacked/bootconfig")

echo "[+] done: $OUT (multiboot.rc injected)"
