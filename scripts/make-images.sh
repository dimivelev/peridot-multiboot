#!/usr/bin/env bash
# make-images.sh (v1 sketch) — build per-ROM image files for the multiboot scheme
#
# Usage: make-images.sh <rom-fastboot.zip> <outdir>
# Extracts super payload logical partitions into plain image files that
# multiboot-mount can losetup+bind (docs/02 §2.1 Scheme A).
#
# NOTE: modern ROMs ship dynamic partitions inside a super.img (sparse) — this
# script extracts the payload.bin via Android payload dumper (cirom tool) and
# splits logical partitions. Iterate on-device; see docs/07 Phase 3.

set -euo pipefail
ZIP="$1"; OUT="$2"
mkdir -p "$OUT"
echo "[*] extracting payload.bin from $ZIP"
unzip -o -j "$ZIP" payload.bin -d "$OUT" >/dev/null
echo "[*] dumping partitions (requires python3 + pip install payload-dumper-go || pip install cirom)"
command -v payload-dumper-go >/dev/null 2>&1 && \
  payload-dumper-go -p -o "$OUT" "$OUT/payload.bin" || \
  python3 -m pip install --quiet payload_dumper && python3 -m payload_dumper --partitions system,product,vendor,system_ext -o "$OUT" "$OUT/payload.bin"
echo "[+] images in $OUT — place under /data/multiboot/romN/ on device"
