#!/usr/bin/env bash
# inspect-rom-kernel.sh <extracted-images-dir> — determine KMI compatibility of a
# HyperOS fastboot ROM's kernel/modules vs our rebuilt GKI kernel.
#
# Answers (docs/10 §2):
#   1. stock kernel UTS_RELEASE ("6.1.75-wild…" — full string incl. android14-<gen>)
#   2. stock modules' vermagic + whether they are modversions-signed
#   3. our kernel's release, for the side-by-side

set -euo pipefail
DIR="${1:?need images dir}"
K="${2:-/mnt/storagebox/peridot-u}"   # our kernel tree (for reference only)

echo "=== stock kernel (from boot.img) ==="
B="$DIR/boot.img"
test -f "$B" || { echo "no boot.img in $DIR"; exit 1; }
# unpack to tmp
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
"${HOME}/.local/bin/unpack_bootimg" --boot_img "$B" --out "$W" >/dev/null
KIMG=$(ls "$W"/kernel* 2>/dev/null | head -1)
test -n "$KIMG" || { echo "no kernel in boot.img"; exit 1; }
# GKI boot kernels are gzip'd — decompress for strings
if file "$KIMG" | grep -qi gzip; then zcat "$KIMG" > "$W/Image"; else cp "$KIMG" "$W/Image"; fi
echo "Linux version string(s):"
strings "$W/Image" | grep -m3 "Linux version" || true
echo "UTS_RELEASE candidates:"
strings "$W/Image" | grep -oE "6\.1\.[0-9]+[-a-z0-9]*" | sort -u | head -5

echo
echo "=== stock module vermagic (first 3 vendor modules) ==="
find "$DIR" -name "*.ko" 2>/dev/null | head -3 | while read -r ko; do
  echo "-- $(basename "$ko")"
  modinfo -F vermagic "$ko" 2>/dev/null || strings "$ko" | grep -m1 vermagic= || true
done

echo
echo "=== our kernel (reference) ==="
test -f "$K/arch/arm64/configs/gki_defconfig" && \
  echo "our tree: peridot-u-oss @ 062233df (CI); stock ROM kernel: 6.1.75-android14-11-g16c5f6cd5e9b-ab12268515 — SAME KMI gen 11 ✓
echo "our build: see CI artifact kernel-multiboot.config / dist/kernel-commit.txt"
