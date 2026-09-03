#!/usr/bin/env bash
# extract-rom-images.sh <HyperOS-fastboot-tgz> <outdir>
#
# Extracts the boot-critical stock images from a Xiaomi fastboot ROM package
# (peridot_fastboot_*.tgz — contains an images/ dir with partition images).
# These are needed by scripts/repack-initboot.sh / repack-vendorboot.sh and
# are your rollback copies (docs/06 §0.2).
#
# Usage: extract-rom-images.sh peridot_fastboot_os1.0.5.0.tgz ./stock

set -euo pipefail
TGZ="$1"; OUT="$2"
mkdir -p "$OUT"
KEEP='(boot|init_boot|vendor_boot|dtbo|vbmeta|vbmeta_system|recovery)\.img$'

echo "[*] listing package contents (first match wins, ~1-2 GB extract)"
tar -tzf "$TGZ" | grep -E "$KEEP" | head -40 || true

echo "[*] extracting…"
tar -xzf "$TGZ" -C "$OUT" --wildcards --no-anchored $(echo $KEEP | tr -d '()')

echo "[*] flattening"
find "$OUT" -name '*.img' -exec mv {} "$OUT/" \; 2>/dev/null || true
echo "[+] stock images in $OUT/"
ls -la "$OUT" | grep -E "boot|dtbo|vbmeta|recovery" || true
echo
echo "next: bash scripts/repack-initboot.sh $OUT/init_boot.img bootmenu/bootmenu init_boot.mb.img"
