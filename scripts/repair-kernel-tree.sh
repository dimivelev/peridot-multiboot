#!/usr/bin/env bash
# repair-kernel-tree.sh <kernel_src_dir>
#
# MiCode/Xiaomi_Kernel_OpenSource exports an INCOMPLETE tree: drivers/misc/Kconfig
# sources drivers/misc/hwid/Kconfig, which lives in a separate Xiaomi repo that is
# not shipped. This script:
#   1. fetches the device-agnostic MI hwid driver from a sibling 6.1 kernel tree
#      (Pzqqt/android_kernel_xiaomi_marble)
#   2. iteratively probes `make gki_defconfig` and stubs any remaining missing
#      Kconfig `source` targets so configure can complete
set -euo pipefail
KS="$1"

# --- 1) known missing Xiaomi drivers -----------------------------------------
if ! test -f "$KS/drivers/misc/hwid/Kconfig"; then
  mkdir -p "$KS/drivers/misc/hwid"
  base="https://raw.githubusercontent.com/Pzqqt/android_kernel_xiaomi_marble/melt-rebase/drivers/misc/hwid"
  for f in Kconfig Makefile hwid.c hwid.h; do
    curl -fsSL --retry 2 -o "$KS/drivers/misc/hwid/$f" "$base/$f"
  done
  echo "[+] fetched drivers/misc/hwid (4 files) from Pzqqt/android_kernel_xiaomi_marble"
fi

# --- 2) probe-and-stub any other missing Kconfig sources ----------------------
cd "$KS"
for i in 1 2 3 4 5; do
  set +e
  probe=$(make O=out ARCH=arm64 gki_defconfig 2>&1)
  missing=$(printf '%s\n' "$probe" | grep -oE 'can.t open file "[^"]+"' | grep -oE '"[^"]+"' | tr -d '"' || true)
  set -e
  if [ -z "$missing" ]; then
    echo "[+] Kconfig sources complete (iteration $i)"
    break
  fi
  echo "[!] missing Kconfig sources: $missing"
  for f in $missing; do
    mkdir -p "$(dirname "$f")"
    echo "# stub: referenced by tree but absent from MiCode export (see scripts/repair-kernel-tree.sh)" > "$f"
    echo "[+] stubbed $f"
  done
done
