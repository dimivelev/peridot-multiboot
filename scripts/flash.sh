#!/usr/bin/env bash
# flash.sh — flash / restore multiboot images on POCO F6 (peridot)
#
# Usage:
#   flash.sh boot     boot.img          # custom GKI kernel
#   flash.sh initboot init_boot.img     # bootmenu-injected init_boot
#   flash.sh vendorboot vendor_boot.img # multiboot.rc-injected vendor_boot
#   flash.sh restore  BACKUP_DIR        # restore ALL stock images
#
# PREREQUISITES (all mandatory, see docs/06-flash-and-rollback.md):
#   1. Xiaomi bootloader UNLOCKED (orange state) —Mi Unlock done, waiting period elapsed
#   2. FULL backup: adb pull of everything important (userdata WILL be wiped once by unlock)
#   3. Stock images extracted from the CURRENT HyperOS fastboot ROM for your exact build:
#      unzip -j stock-rom.zip 'images/*'   → boot.img init_boot.img vendor_boot.img vbmeta.img
#   4. vbmeta flashed once with verity/verification disabled (script does it)

set -euo pipefail
CMD="${1:-}"; IMG="${2:-}"
SLOT=$(fastboot getvar current-slot 2>/dev/null | awk '{print $2}' | tr -d ':')

flash_image() { # part file
  echo "[*] flashing $1 (slot $SLOT) ← $2"
  fastboot flash "$1${SLOT:+_$SLOT}" "$2"
}

case "$CMD" in
  boot)       test -f "$IMG"; flash_image boot "$IMG" ;;
  initboot)   test -f "$IMG"; flash_image init_boot "$IMG" ;;
  vendorboot) test -f "$IMG"; flash_image vendor_boot "$IMG" ;;
  disable-avb)
    echo "[*] flashing vbmeta with verification disabled"
    fastboot flash vbmeta --slot "$SLOT" --disable-verity --disable-verification vbmeta.img
    ;;
  restore)
    D="${2:?need backup dir}"; test -d "$D"
    for p in boot init_boot vendor_boot vbmeta vendor_boot_a vendor_boot_b; do
      test -f "$D/$p.img" && fastboot flash "$p" "$D/$p.img" || true
    done
    echo "[+] stock images restored — device is 100% stock again"
    ;;
  *)
    grep '^#' "$0" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac
echo "[+] done."
