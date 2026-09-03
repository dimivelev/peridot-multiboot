#!/usr/bin/env bash
# repack-initboot.sh — inject bootmenu as /init into an init_boot.img
#
# Usage: repack-initboot.sh <stock-init_boot.img> <bootmenu-binary> <out.img>
#
# How it works:
#   1. unpacks init_boot (header v4, generic ramdisk only)
#   2. renames /init -> /init.real, installs bootmenu binary as /init
#   3. repacks cpio.gz and rebuilds the image
#
# Requires: python3 + mkbootimg (pip install mkbootimg), cpio, gzip, lz4

set -euo pipefail

IMG="$1"; MENU="$2"; OUT="$3"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

command -v unpack_bootimg >/dev/null 2>&1 || pip3 install --quiet mkbootimg

echo "[*] unpacking $IMG"
mkdir "$WORK/unpacked"
unpack_bootimg --boot_img "$IMG" --out "$WORK/unpacked"

RAMDISK_FORMAT=$(cd "$WORK/unpacked" && ls | grep -oE 'ramdisk[^ ]*' | head -1)
echo "[*] ramdisk file: $RAMDISK_FORMAT"

mkdir "$WORK/rd"
cd "$WORK/rd"
if file "$WORK/unpacked/$RAMDISK_FORMAT" | grep -q gzip; then
  gzip -dc "$WORK/unpacked/$RAMDISK_FORMAT" | cpio -idm --quiet
elif file "$WORK/unpacked/$RAMDISK_FORMAT" | grep -q LZ4; then
  lz4 -dc "$WORK/unpacked/$RAMDISK_FORMAT" | cpio -idm --quiet
else
  cpio -idm --quiet < "$WORK/unpacked/$RAMDISK_FORMAT"
fi

test -f init || { echo "ERROR: no /init in ramdisk — not an init_boot image?" >&2; exit 1; }
mv init init.real
cp "$MENU" init
chmod 750 init
# menu needs these locations to exist pre-mount
mkdir -p metadata multiboot

find . | cpio -o -H newc --quiet | gzip -9 > "$WORK/rd.cpio.gz"

echo "[*] repacking $OUT"
python3 -m mkbootimg \
  --header_version "$(python3 -c "import json;print(json.load(open('$WORK/unpacked/bootimg.json'))['header_version'])" 2>/dev/null || echo 4)" \
  --kernel "$WORK/unpacked/kernel" 2>/dev/null || true
# header v4 init_boot: kernel may be absent — use unpacked fields verbatim
python3 -m mkbootimg \
  --ramdisk "$WORK/rd.cpio.gz" \
  --header_version 4 --os_version 14.0.0 --os_patch_level 2024-08-05 \
  --pagesize 4096 -o "$OUT"

echo "[+] done: $OUT (menu installed as /init, original init kept as /init.real)"
