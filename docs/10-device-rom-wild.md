# User's Exact ROM Found — HyperOS 2.0.105.0.VNPMIXM & the "wild" kernel

Status: ✅ ROM located from official Xiaomi CDN mirrors; downloading to storagebox.

## 1. The ROM (verified via XM Firmware Updater database)

| Field | Value |
|---|---|
| Device | **POCO F6 Global (peridot)** |
| Build | **OS2.0.105.0.VNPMIXM** (HyperOS 2, Android 15, Global stable) |
| Date | 2025-06-10 |
| Fastboot package | `peridot_global_images_OS2.0.105.0.VNPMIXM_20250610.0000.00_15.0_global_ad5484aedb.tgz` (8.74 GB) |
| Mirrors (official Xiaomi CDN) | `https://cdnorg.d.miui.com/OS2.0.105.0.VNPMIXM/<pkg>` · `bigota.d.miui.com` · `bn.d.miui.com` · `hugeota.d.miui.com` (same path) |
| On-device kernel | **6.1.75-wild** (user-verified) |

Source page (XiaomiFirmwareUpdater/xmfirmwareupdater.github.io):
`pages/hyperos/updates/peridot/OS2.0.105.0.VNPMIXM.md` — lists both the Recovery zip
(`peridot_global-ota_full-...-1d9c56508c.zip`, 6.0 GB) and the **Fastboot tgz** (9.2 GB).

## 2. The "wild" kernel — what it means

- Stock peridot kernel on HyperOS 2 is `6.1.75-wild…` — Xiaomi's **HyperOS 2 / A15 kernel
  generation** ("wild" = internal platform tag, not present in the HyperOS 1 export).
- **MiCode has NO published source for it**: `git ls-remote` shows only `peridot-u-oss`
  (HyperOS 1 / Android 14). No GitHub mirrors found (searched repos + code, 2026-09).
- Consequence: our CI kernel is built from `peridot-u-oss` = **android14-6.1** lineage.
  Whether the OS2.0.105.0 stock modules load on it depends on their vermagic/KMI —
  **to be verified empirically** by unpacking this fastboot ROM:
  1. `boot.img` → kernel → `strings Image | grep "Linux version"` (UTS_RELEASE)
  2. `vendor_boot` / `vendor_dlkm` modules → `modinfo <ko> | grep vermagic`
  3. Compare against our built kernel's release + `android/abi_gki_aarch64*` symbol lists.
- Fallbacks if KMI mismatch: (a) ship the ROM's matching `vendor_dlkm`/`system_dlkm`
  modules alongside our kernel (they're in this tgz), or (b) treat the menu as the only
  kernel modification and keep stock kernel for daily driving until Xiaomi publishes source.

## 3. Downloaded artifacts (storagebox)

- `/mnt/storagebox/peridot_global_images_OS2.0.105.0.VNPMIXM.1.tgz` (8.74 GB, aria2 16-conn)
- After extraction: `stock/` dir with boot/init_boot/vendor_boot/dtbo/vbmeta + module sets.
- Repacked flashables will land in `/mnt/storagebox/flashables/`:
  - `boot-multiboot.img` (from CI)
  - `init_boot.mb.img` (stock init_boot + bootmenu as /init)
  - `menu-test-boot.img` (zero-flash trial)
