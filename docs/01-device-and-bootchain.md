# POCO F6 / Redmi Turbo 3 ("peridot") — Device & Boot Chain Technical Reference

> **Project:** peridot-multiboot — research doc 01 of N
> **Scope:** Hardware identity, partition layout, boot chain, bootloader unlock, GKI status, community landscape, and safety notes relevant to building a multi-OS boot menu.
> **Research date:** 2026 (all URLs verified live at time of writing)
> **Legend:** Facts are cited inline. Anything not directly verified for this exact device is marked **NEEDS VERIFICATION** and collected in §8.

---

## TL;DR

```
┌────────────────────────────────────────────────────────────────────────────────┐
│ POCO F6 / Redmi Turbo 3 ("peridot")                                            │
│                                                                                │
│ • SoC: Qualcomm SM8635 "Snapdragon 8s Gen 3" — HLOS codename "cliffs",         │
│   internal msm codename "palawan", part of the "pineapple" family              │
│   (build platform TARGET_BOARD_PLATFORM=pineapple). [pmos wiki, LOS tree]      │
│                                                                                │
│ • Virtual A/B device, boot header v4, dedicated SINGLE-SLOT recovery partition  │
│   (recovery is NOT A/B on this device). [LOS wiki yml, fstab]                  │
│                                                                                │
│ • Boot images: boot.img = GKI kernel (6.1, android14-6.1 KMI) + DTB only;      │
│   init_boot.img (8 MiB!) = generic ramdisk (first-stage init);                 │
│   vendor_boot.img (96 MiB) = vendor ramdisk (1st+2nd stage modules) + DTB      │
│   + bootconfig. [BoardConfig.mk, AOSP generic-boot]                            │
│                                                                                │
│ • AVB: vbmeta + vbmeta_system (system/product/system_ext/system_dlkm chained   │
│   via vbmeta_system). Unlock = orange state = verification errors tolerated.   │
│   [fstab, BoardConfig, AOSP device-state]                                      │
│                                                                                │
│ • Unlock via Xiaomi Mi Unlock tool only (fastboot flashing unlock does NOT     │
│   work); wipes data; account limited to 4 devices/year, up to 30-day wait.     │
│   [LOS install page]                                                           │
│                                                                                │
│ • XBL / ABL / TZ / hyp etc. can NOT be replaced after unlock — verified        │
│   against fused OEM keys by PBL; corrupted firmware ⇒ EDL only, which needs    │
│   an authorized account on modern Xiaomi devices. Hard-brick risk.             │
│                                                                                │
│ • Safely flashable after unlock: boot, init_boot, vendor_boot, dtbo, recovery, │
│   vbmeta(+vbmeta_system) with --disable-verity --disable-verification,         │
│   super contents via fastbootd.                                                │
│                                                                                │
│ • Best hook points for a boot menu: vendor_boot vendor ramdisk (init rc +      │
│   scripts, 96 MiB headroom) or kernel replacement in boot.img (KMI-matched);   │
│   init_boot is too small for a full menu environment.                          │
└────────────────────────────────────────────────────────────────────────────────┘
```

---

## 1. Device identity

### 1.1 Marketing names, variants, models

The single codename **peridot** covers two retail identities that share one LineageOS build ([LineageOS variant selector](https://wiki.lineageos.org/devices/peridot/)):

| Variant | Retail name | Model numbers | Released | Source |
|---|---|---|---|---|
| variant1 | **POCO F6** | 24069PC21G (global), 24069PC21I | 2024-05 | [LineageOS wiki peridot/variant1](https://wiki.lineageos.org/devices/peridot/variant1/) |
| variant2 | **Redmi Turbo 3** | 24069RA21C (China) | 2024-04 | [LineageOS wiki peridot/variant2](https://wiki.lineageos.org/devices/peridot/variant2/) |

LineageOS explicitly states: "These devices all use the same LineageOS build" and supports only the exact model numbers listed above ([install page](https://wiki.lineageos.org/devices/peridot/install/variant1)).

### 1.2 SoC: Qualcomm SM8635 "Snapdragon 8s Gen 3"

| Property | Value | Source |
|---|---|---|
| SoC | Qualcomm **SM8635**, marketed **Snapdragon 8s Gen 3** | [LineageOS wiki](https://wiki.lineageos.org/devices/peridot/variant1/), [POCO official specs (archived 2024)](https://www.po.co/global/product/poco-f6/specs) |
| Qualcomm platform family | **pineapple** (same HLOS build-config family as SM8650) | [LineageOS BoardConfig `TARGET_BOARD_PLATFORM := pineapple`](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/BoardConfig.mk) |
| HLOS codename | **cliffs** (SM8635), **cliffs7** (SM7675) | [postmarketOS wiki: Snapdragon 8s Gen 3/7+ Gen 3](https://wiki.postmarketos.org/wiki/Qualcomm_Snapdragon_8s_Gen_3/7%2B_Gen_3_(Palawan/Lamma)) |
| Internal msm codename | **palawan** (SM8635), **lamma** (SM7675) | same pmos wiki page |
| Mainline DT compatible | `qcom,cliffs` — peridot overlay: `qcom,msm-id = <614 0x10000>, <632 0x10000>` | [sm8635-devicetrees: peridot-sm8635-overlay.dts](https://github.com/LineageOS/android_kernel_xiaomi_sm8635-devicetrees) |
| CPU | 1× Cortex-X4 @ 3.0 GHz + 4× Cortex-A720 @ 2.8 GHz + 3× Cortex-A520 @ 2.0 GHz ("Kryo", arm64) | [LineageOS wiki](https://wiki.lineageos.org/devices/peridot/variant1/), [POCO specs](https://www.po.co/global/product/poco-f6/specs) |
| GPU | **Adreno 735** (1100 MHz per pmos) | both |
| Process / year | TSMC 4 nm, 2024 | [POCO specs](https://www.po.co/global/product/poco-f6/specs), [pmos wiki](https://wiki.postmarketos.org/wiki/Qualcomm_Snapdragon_8s_Gen_3/7%2B_Gen_3_(Palawan/Lamma)) |

**Relationship to SM8650/pineapple** (important for everything downstream): the pmos wiki describes the SM8635 as "flagship trimmed version of Snapdragon 8 Gen 3 (SM8650)" and states "both of them belong to pineapple family" ([pmos wiki](https://wiki.postmarketos.org/wiki/Qualcomm_Snapdragon_8s_Gen_3/7%2B_Gen_3_(Palawan/Lamma))). Concretely in this device's software stack:

- Xiaomi's board platform is `pineapple` (not "cliffs") — device trees, kernel configs (`vendor/pineapple_GKI.config`) and module lists (`modules.list.msm.pineapple`) are shared/piggybacked from the SM8650 tree, with a `peridot` overlay config on top ([LineageOS BoardConfig](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/BoardConfig.mk)).
- The downstream kernel tree contains clocks/pinctrl for *both* `pineapple` (SM8650) and `cliffs` (SM8635) — e.g. `gcc-pineapple.ko` **and** `gcc-cliffs.ko` in the first-stage module list ([sm8635 kernel: modules.list.msm.pineapple](https://github.com/LineageOS/android_kernel_xiaomi_sm8635/blob/HEAD/modules.list.msm.pineapple)).
- pmos warns: even though SM8635 is very similar to SM8650, mainline rework is significant; SM8635/SM7675 mentions existed in the 6.1 Android kernel but were dropped from 6.6+ ([pmos wiki](https://wiki.postmarketos.org/wiki/Qualcomm_Snapdragon_8s_Gen_3/7%2B_Gen_3_(Palawan/Lamma))).

Other devices on SM8635 (useful cross-reference for bring-up): Xiaomi Pad 7 Pro (`xiaomi-muyu`), Motorola Razr+ 2024 (`motorola-arcfox`), Qualcomm QRD8635 ([pmos wiki](https://wiki.postmarketos.org/wiki/Qualcomm_Snapdragon_8s_Gen_3/7%2B_Gen_3_(Palawan/Lamma))).

### 1.3 RAM / storage variants

| Config | Detail | Source |
|---|---|---|
| RAM / storage | **8 GB + 256 GB / 12 GB + 512 GB**, LPDDR5X + **UFS 4.0** | [POCO official specs (Wayback 2024)](https://www.po.co/global/product/poco-f6/specs) |
| LineageOS listing | "RAM: 8/12 GB, Storage: 256/512 GB" | [LineageOS wiki](https://wiki.lineageos.org/devices/peridot/variant1/) |
| UFS host controller in fstab | `/sys/devices/platform/soc/1d84000.ufshc`, f2fs userdata with metadata encryption (aes-256-xts wrappedkey) | [LineageOS fstab.qcom](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/rootdir/etc/fstab.qcom) |

Display: 6.67" 1220×2712 Super AMOLED, 120 Hz; battery 5000 mAh; no SD slot, no 3.5mm jack (peripherals list) ([LineageOS wiki](https://wiki.lineageos.org/devices/peridot/variant1/)).

### 1.4 Shipped / current software

| Item | Value | Source |
|---|---|---|
| Global launch (POCO F6) | May 2024, HyperOS (Android 14 base; the archived 2024 POCO specs page lists "HyperOS" as OS) | [LineageOS wiki (release)](https://wiki.lineageos.org/devices/peridot/variant1/), [POCO specs Wayback 2024-05-23](http://web.archive.org/web/20240523151545/https://www.po.co/global/product/poco-f6/specs) |
| China launch (Redmi Turbo 3) | April 2024 | [LineageOS wiki variant2](https://wiki.lineageos.org/devices/peridot/variant2/) |
| Exact shipped firmware string (e.g. `OS1.0.x.y`) | **NEEDS VERIFICATION** | — |
| Current major updates | HyperOS 2 (Android 15) and HyperOS 3 (Android 16) generations exist for peridot; LineageOS requires stock **"Android 15/16"** firmware before install | [LineageOS install page](https://wiki.lineageos.org/devices/peridot/install/variant1); [harunaltair/poco-f6-gki-kernel](https://github.com/harunaltair/poco-f6-gki-kernel) ("HyperOS 3 (Android 16)") |
| Vendor security patch of Lineage vendor tree | `VENDOR_SECURITY_PATCH := 2026-02-01` | [LineageOS BoardConfig](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/BoardConfig.mk) |

LineageOS strongly warns that installing its builds requires the correct **stock firmware generation** first ("Failing to install the correct firmware version prior to installation may result in failure to install LineageOS, unexpected crashes post-installation, or permanent damage to your device!") — firmware compatibility is a real constraint on this platform ([install page](https://wiki.lineageos.org/devices/peridot/install/variant1)).

---

## 2. Partition layout

### 2.1 Boot-critical partitions with known sizes (from LineageOS build config)

These sizes come straight from [`BoardConfig.mk`](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/BoardConfig.mk) (device-tree-verified):

| Partition | Size (bytes) | Size (MiB) | A/B? | Notes |
|---|---|---|---|---|
| `boot` | 100,663,296 | 96 | **A/B** (boot_a/boot_b) | GKI kernel + DTB (`BOARD_INCLUDE_DTB_IN_BOOTIMG := true`), header **v4** |
| `init_boot` | 8,388,608 | 8 | **A/B** | generic ramdisk (first-stage init only), header **v4** |
| `vendor_boot` | 100,663,296 | 96 | **A/B** | vendor ramdisk (kernel modules) + DTB + bootconfig, header **v4** |
| `dtbo` | 25,165,824 | 24 | **A/B** | device-tree overlays (`TARGET_NEEDS_DTBOIMAGE := true`) |
| `recovery` | 104,857,600 | 100 | **single, no slot suffix** | dedicated, self-contained recovery ramdisk |
| `super` | 9,126,805,504 | 8704 (8.5 GiB) | n/a (single) | holds all dynamic partitions; group size 9,122,611,200 B (8700 MiB) |
| `vbmeta` | — | — | **A/B** | top-level AVB metadata |
| `vbmeta_system` | — | — | **A/B** | AVB metadata for system/product/system_ext/system_dlkm (`BOARD_AVB_VBMETA_SYSTEM := system system_dlkm system_ext product`) |
| `metadata` | — | — | n/a | f2fs, formattable, holds dm-crypt/metadata-encryption keys (vold), checkpoint data |
| `misc` | — | — | n/a | BCB (bootloader control block) — `boot-recovery`, slot retry counters messaging |

A/B status for boot/dtbo/init_boot/vbmeta/vbmeta_system etc. is set by `AB_OTA_UPDATER := true` + `AB_OTA_PARTITIONS` in [BoardConfig.mk](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/BoardConfig.mk). The wiki metadata confirms `is_ab_device: true` and `recovery_partition_name: recovery` (no slot suffix) ([peridot_variant1.yml](https://github.com/LineageOS/lineage_wiki/blob/main/_data/devices/peridot_variant1.yml)). Per the [AOSP generic-boot doc](https://source.android.com/docs/core/architecture/partitions/generic-boot) this matches the "launch with Android 13, dedicated and non-A/B recovery" architecture. *(Note: the Lineage tree does list `recovery` inside `AB_OTA_PARTITIONS`, which is how the OTA updater packages it; the on-disk GPT has a single `recovery` partition — confirmed by fstab path `/dev/block/by-name/recovery`.)*

There is **no `vendor_kernel_boot`** partition on this device — it is not in `AB_OTA_PARTITIONS`, not in the firmware image list, and not referenced by the fstab ([BoardConfig.mk](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/BoardConfig.mk), [proprietary-firmware.txt](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/proprietary-firmware.txt)). Modules are instead split across vendor_boot/vendor_dlkm/system_dlkm (see §5.3).

### 2.2 Firmware partitions (Xiaomi firmware images, all A/B)

From [`proprietary-firmware.txt`](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/proprietary-firmware.txt) in the Lineage device tree — every entry is suffixed `;AB`:

| Image | Role (Qualcomm/Xiaomi boot chain) | Loaded by |
|---|---|---|
| `xbl` | eXtensible Boot Loader (XBL core) — first UFS-resident stage, UEFI-based | PBL (ROM) |
| `xbl_config` | XBL configuration image | XBL |
| `uefi` | UEFI environment image (XBL UEFI phase, separate partition on this generation) | XBL |
| `xbl_ramdump` | RAM-dump / crash-handler firmware | XBL (on crash) |
| `abl` | Android Bootloader (`abl.elf`) — UEFI application implementing fastboot + AVB | XBL/UEFI |
| `tz` | TrustZone (QTEE) firmware | XBL/UEFI |
| `hyp` | Qualcomm hypervisor firmware | XBL/UEFI |
| `aop`, `aop_config` | Always-On Processor firmware | XBL/UEFI |
| `cpucp`, `cpucp_dtb` | CPU Control Processor firmware + DTB | XBL/UEFI |
| `shrm` | Shared memory (SHRM) firmware, early boot | PBL/XBL |
| `devcfg` | Device configuration (security policies) | XBL |
| `keymaster` | KeyMaster/KeyMint TA (TrustZone app) | tz |
| `uefisecapp` | UEFI secure app (TrustZone app for UEFI services) | tz/UEFI |
| `imagefv` | Image firmware volume (UEFI FV blobs) | UEFI |
| `qupfw` | QUP (serial engine) firmware | ABL/kernel |
| `multiimgqti` | Multi-image QTI firmware bundle | kernel/userland |
| `modem` | Modem (MPSS) firmware — mounted at `/vendor/firmware_mnt` | PIL from kernel |
| `modemfirmware` | Modem firmware files — `/vendor/modem_firmware` | userland |
| `dsp` | ADSP/CDSP firmware — `/vendor/dsp` | PIL from kernel |
| `bluetooth` | Bluetooth (WCNSS) firmware — `/vendor/bt_firmware` | userland |
| `countrycode` | Xiaomi country/region config | userland |
| `featenabler` | Xiaomi feature-enabler config partition | userland |

*(Roles marked "Loaded by" follow the standard Qualcomm boot architecture for this generation; the per-device partition list itself is verified from the LineageOS tree. Qualcomm's own XBL documentation index: [docs.qualcomm.com XBL topic](https://docs.qualcomm.com/bundle/publicresource/topics/80-6520-2/xbl.html); the open-source part of the bootloader is Qualcomm's UEFI-based ABL at [CodeLinaro `clo/la/abl/tianocore`](https://git.codelinaro.org/clo/la/abl/tianocore).)*

### 2.3 Other partitions referenced by the fstab

From [`rootdir/etc/fstab.qcom`](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/rootdir/etc/fstab.qcom):

| Partition | Mount | FS | Notes |
|---|---|---|---|
| `system`, `system_ext`, `product`, `odm`, `vendor`, `vendor_dlkm`, `system_dlkm` | logical (inside `super`) | erofs or ext4 | dynamic partitions, first_stage_mount, dm-verity via `avb=vbmeta_system` (system side) / `avb=vbmeta` (vendor side) |
| `userdata` | `/data` | f2fs | file-based encryption `wrappedkey_v0`, metadata encryption, `checkpoint=fs` (virtual A/B) |
| `metadata` | `/metadata` | f2fs | keydirectory `/metadata/vold/metadata_encryption` |
| `persist` | `/mnt/vendor/persist` | ext4 | sensors/RIL calibration — **do not format** |
| `modem` | `/vendor/firmware_mnt` | vfat | modem firmware mount |
| `modemfirmware` | `/vendor/modem_firmware` | vfat | |
| `dsp` | `/vendor/dsp` | ext4 | |
| `bluetooth` | `/vendor/bt_firmware` | vfat | |
| `qmcs` | `/mnt/vendor/qmcs` | vfat | Xiaomi modem config |
| `spunvm` | `/mnt/vendor/spunvm` | vfat | SPU NVM |
| `misc` | (raw) | emmc | BCB |
| `boot`, `init_boot`, `vendor_boot`, `dtbo`, `recovery` | (raw) | emmc | AVB-protected, `avb=vbmeta` |

Partitions expected on Xiaomi UFS but **not directly verified from a peridot dump** — e.g. `frp`, `devinfo` (unlock state), `fsc`/`fsg`/`modemst1`/`modemst2` (modem NV), `logfs`, `apdp`/`msadp`, `config`, `dip`, `rescue` (mapped to `/cache` in the [TWRP tree](https://github.com/Nomishaw21/twrp_peridot)), `oplus`-style partitions do not exist here — are listed in §8.

### 2.4 `super` / dynamic partitions

`BOARD_SUPER_PARTITION_GROUPS := qti_dynamic_partitions` with members **odm, product, system, system_dlkm, system_ext, vendor, vendor_dlkm**; dynamic group size 9,122,611,200 bytes (super minus 4 MiB) ([BoardConfig.mk](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/BoardConfig.mk)). All are erofs or ext4 read-only, mounted by first-stage init with dm-verity. Because they live in `super`, flashing individual system-side images (e.g. a GSI or a second OS's system image) at runtime requires **fastbootd** (userspace fastboot in recovery), not the bootloader fastboot ([AOSP fastbootd doc](https://source.android.com/docs/core/architecture/bootloader/fastbootd)).

---

## 3. Boot chain in detail

### 3.1 Android boot-image architecture on this device (the Android 13+ split)

Per the [AOSP generic boot partition doc](https://source.android.com/docs/core/architecture/partitions/generic-boot):

- "In Android 12, the generic boot image … contains the generic ramdisk and the GKI kernel."
- "**For devices launching with Android 13, the generic ramdisk is removed from the boot image and placed in a separate `init_boot` image.** This change leaves the boot image with only the GKI kernel."
- "On devices that … use a dedicated recovery partition, no change in the recovery ramdisk is needed because the recovery ramdisk is self-contained."

peridot (launched with Android 14) follows the "launch with Android 13+, dedicated non-A/B recovery" layout:

| Image | Contents (per [AOSP generic-boot](https://source.android.com/docs/core/architecture/partitions/generic-boot) + [vendor-boot doc](https://source.android.com/docs/core/architecture/partitions/vendor-boot-partitions)) | peridot specifics |
|---|---|---|
| `boot` (header v4) | GKI kernel, generic cmdline, DTB (device merges DTB into bootimg) | `BOARD_BOOT_HEADER_VERSION := 4`, `BOARD_INCLUDE_DTB_IN_BOOTIMG := true`, pagesize 4096, base 0x0 |
| `init_boot` (header v4) | generic ramdisk: first-stage `init`, `system/etc/ramdisk/build.prop`, empty mount points, `first_stage_ramdisk/` | `BOARD_INIT_BOOT_HEADER_VERSION := 4`; **only 8 MiB** |
| `vendor_boot` (header v4) | vendor ramdisk fragments (kernel modules), device-specific cmdline, bootconfig, DTB | module lists §5.3; `BOARD_BOOTCONFIG` includes `androidboot.hardware=qcom`, `androidboot.load_modules_parallel=true`, `androidboot.usbcontroller=a600000.dwc3` |
| `recovery` (header v2) | self-contained recovery ramdisk + recovery cmdline + recovery DTBO | `BOARD_EXCLUDE_KERNEL_FROM_RECOVERY_IMAGE := true` (uses boot kernel; recovery is flashed together with boot-stack images per LOS) |

The device cmdline is tiny and mostly framework-agnostic (`sysctl.kernel.firmware_config.force_sysfs_fallback=1`, fingerprint strings), while hardware-relevant parameters travel via **bootconfig** in vendor_boot ([BoardConfig.mk](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/BoardConfig.mk); bootconfig spec: [AOSP implementing-bootconfig](https://source.android.com/docs/core/architecture/bootloader/implementing-bootconfig)).

### 3.2 Stage-by-stage flow

1. **PBL (Primary Boot Loader, in SoC ROM).** Mask-ROM code at reset. It verifies and loads XBL from UFS using the OEM root-of-trust hash burned into QFPROM fuses; this anchor cannot be rewritten in the field (see §4.3). General Qualcomm secure-boot architecture; also cited by AOSP: root of trust public key "embedded in the device and stored in a place so it can't be tampered with (typically read-only storage)" ([AOSP device-state](https://source.android.com/docs/security/features/verifiedboot/device-state)).
2. **XBL (xbl, xbl_config, uefi).** Qualcomm's UEFI-based secondary bootloader: initializes DRAM, clocks, security per `devcfg`, loads `aop`, `shrm`, `cpucp`, `tz`, `hyp`, `uefisecapp`, then launches the UEFI environment (this generation even keeps a dedicated `uefi` partition, see §2.2). Qualcomm XBL docs: [docs.qualcomm.com](https://docs.qualcomm.com/bundle/publicresource/topics/80-6520-2/xbl.html).
3. **ABL (`abl.elf`, UEFI application).** Qualcomm's Android Bootloader: implements fastboot, selects the **active slot**, loads vbmeta and verifies the Android images with libavb, applies vbmeta flags (e.g. hashtree-disabled), assembles the kernel command line + bootconfig, loads `boot.img` (kernel+DTB), `vendor_boot` (vendor ramdisk+DTB+bootconfig) and `init_boot` (generic ramdisk) into memory, and jumps to the kernel. Open-source upstream: [CodeLinaro abl/tianocore](https://git.codelinaro.org/clo/la/abl/tianocore). On Xiaomi devices this is stock — Xiaomi-specific features (unlock state check, orange-state splash, `devinfo`/misc handling) live here. *(Xiaomi-specific ABL internals: NEEDS VERIFICATION.)*
4. **AVB verification (still in ABL).** For each slot, `avb_slot_verify()` loads the top-level `vbmeta`, follows chained descriptors (`vbmeta_system` chain for system/product/system_ext/system_dlkm), verifies hash descriptors for `boot`, `init_boot`, `vendor_boot`, `dtbo`, `recovery`, and hashtree descriptors for the dynamic partitions, and checks **rollback indexes** stored in tamper-evident storage ([AVB README](https://android.googlesource.com/platform/external/avb/+/refs/heads/main/README.md), [AOSP AVB](https://source.android.com/docs/security/features/verifiedboot/avb)).
   - In **LOCKED** state verification errors are fatal; in **UNLOCKED** state ABL passes `AVB_SLOT_VERIFY_FLAGS_ALLOW_VERIFICATION_ERROR` and treats public-key rejection / verification failure / rollback violation as non-fatal ([AVB README, "Locked and Unlocked mode"](https://android.googlesource.com/platform/external/avb/+/refs/heads/main/README.md)).
   - Unlocked devices show the orange boot-state warning — "If a device is UNLOCKED, the bootloader shows the user a warning and then proceeds to boot even if the loaded OS isn't signed by the root of trust" ([AOSP device-state](https://source.android.com/docs/security/features/verifiedboot/device-state)).
   - If the top-level vbmeta carries `AVB_VBMETA_IMAGE_FLAGS_HASHTREE_DISABLED`, first-stage init sets `androidboot.veritymode=disabled` and skips dm-verity setup ([AVB README](https://android.googlesource.com/platform/external/avb/+/refs/heads/main/README.md)); `--disable-verification` additionally sets `AVB_VBMETA_IMAGE_FLAGS_VERIFICATION_DISABLED` (flag bits 1 and 2 in `avbtool.py`, [source](https://android.googlesource.com/platform/external/avb/+/refs/heads/main/avbtool.py)).
5. **Kernel start (GKI).** The Linux 6.1 GKI kernel from `boot.img` starts with the concatenated cmdline/bootconfig. DTB from `boot.img` applies overlays from `dtbo`.
6. **First-stage init.** `init` comes from the **generic ramdisk in `init_boot`**; it mounts `/metadata`, processes `first_stage_mount` fstab entries ([fstab.qcom](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/rootdir/etc/fstab.qcom)): loads the first-stage kernel modules from the **vendor_boot ramdisk** (peridot: `modules.list.msm.pineapple` from the kernel tree + `hwid.ko`, ~110 modules — clocks, pinctrl, regulators, UFS/SCSI core, SMMU, etc.), mounts `modem`/`dsp` firmware mounts, then switches roots into `/system` (from `super`) and sets up **dm-verity** on the dynamic partitions unless vbmeta says disabled.
7. **Second-stage init.** SELinux policies load; `/vendor`, `/odm`, `/vendor_dlkm`, `/system_dlkm` are live; second-stage module set loads from `vendor_dlkm` (205 modules listed for second-stage + 77 vendor_dlkm in the Lineage tree); vold unwraps file-based encryption keys using `/metadata`; zygote → System Server → Android.
8. **Recovery path.** `misc` BCB tells ABL to boot the `recovery` partition instead; peridot's recovery is a standalone image sharing the same GKI kernel via `BOARD_EXCLUDE_KERNEL_FROM_RECOVERY_IMAGE` trickery plus its own modules ([BoardConfig.mk](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/BoardConfig.mk)).

*(Steps 5–8 follow the standard GKI boot flow described in [AOSP generic-boot](https://source.android.com/docs/core/architecture/partitions/generic-boot) and [AOSP kernel modules overview](https://source.android.com/docs/core/architecture/kernel/modules); peridot-specific module counts and configs are from the LineageOS tree.)*

### 3.3 AVB partition topology on peridot (as built by LineageOS)

| Chain element | Covers | Key / algorithm | Rollback index location | Source |
|---|---|---|---|---|
| `vbmeta` | vendor, odm, vendor_dlkm, system_dlkm(hashtree via vbmeta), boot, init_boot, vendor_boot, dtbo, recovery (hash) | SHA256_RSA2048 (test key in Lineage; OEM key on stock) | vbmeta itself + chains | [fstab.qcom](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/rootdir/etc/fstab.qcom), [BoardConfig.mk](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/BoardConfig.mk) |
| `vbmeta_system` (chain from vbmeta) | system, system_ext, product, system_dlkm | SHA256_RSA2048 | `ROLLBACK_INDEX_LOCATION := 2` | BoardConfig.mk |
| boot descriptor | `boot` | SHA256_RSA2048 | `ROLLBACK_INDEX_LOCATION := 3` | BoardConfig.mk |
| recovery descriptor | `recovery` | SHA256_RSA2048 | `ROLLBACK_INDEX_LOCATION := 1` | BoardConfig.mk |

LineageOS builds set `BOARD_AVB_MAKE_VBMETA_IMAGE_ARGS += --flags 3` — i.e. hashtree-disabled + verification-disabled — precisely because an unlocked device will refuse nothing but must also not dm-verity-verify a re-signed OS ([BoardConfig.mk](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/BoardConfig.mk)). GSI AVB public keys are moved into the vendor_boot ramdisk (`BOARD_MOVE_GSI_AVB_KEYS_TO_VENDOR_BOOT := true`; fstab `avb_keys=/avb/q-gsi.avbpubkey:...`), which is what allows a GSI to boot and verify itself against its own keys when inserted into the system slot ([BoardConfig.mk](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/BoardConfig.mk), [fstab.qcom](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/rootdir/etc/fstab.qcom)).

> Stock Xiaomi vbmeta values (key, rollback indexes, flags) differ from Lineage's test-key values; the topology above is verified for the Lineage build. **Stock rollback index locations: NEEDS VERIFICATION.**

### 3.4 Where a custom boot menu could hook in (analysis for this project)

Ranked by practicality on peridot:

| Hook point | What you control | Constraints / risks |
|---|---|---|
| **`boot.img` kernel replacement** | Full kernel (own menu via kexec-like loader, custom init, framebuffer UI) | Must be a **6.1 GKI kernel matching the android14-6.1 KMI** or you must also ship matching modules (see §5.4). DTB in boot.img is replaceable. 96 MiB is plenty. Verified boot must be disabled via unlocked state/vbmeta flags. |
| **`vendor_boot` vendor ramdisk** | Additional init `.rc` files, `first_stage_ramdisk` scripts, fstab tweaks, extra modules, recovery modules | 96 MiB budget shared with mandatory module set; module lists are enforced by `modules.load` semantics; cleanest "menu before mount" seam that doesn't touch the kernel ([AOSP vendor-boot doc](https://source.android.com/docs/core/architecture/partitions/vendor-boot-partitions)) |
| **`init_boot` generic ramdisk** | Replace/patch first-stage `init` (Magisk-style) | Only **8 MiB** — fine for a Magisk-like patch, too small for a full menu environment |
| **`recovery` partition** | Fully custom menu OS (own UI) | Only boots when BCB/keys request it; not part of normal boot; but it is a **separate non-A/B slot-independent** image that is trivially flashable after unlock |
| **Slot switching (`_a`/`_b`)** | Two boot configurations (e.g. OS A on slot a, OS B on slot b) with `fastboot set_active`/BCB-based switcher | super is shared and single — both slots' dynamic partitions come from the same super image, so true multi-OS needs super gymnastics (fastbootd) or data-partition images |
| **`misc` BCB** | Programmable one-shot redirection into recovery | Limited to "boot recovery once" semantics |

For a multiboot menu the most promising seam is **vendor_boot ramdisk init scripting** (runs before dynamic partitions are mounted, has framebuffer via kernel, can read key events) combined with either **two super layouts switched via fastbootd** or **per-OS images on userdata** mounted after selection. This mirrors what Magisk/KernelSU already prove possible: Magisk patches the **init_boot** ramdisk on Android 13+ devices and KernelSU ships as an LKM or full GKI kernel replacement ([Magisk install docs](https://github.com/topjohnwu/Magisk/blob/master/docs/install.md), [KernelSU installation docs](https://github.com/tiann/KernelSU/blob/main/website/docs/guide/installation.md)).

---

## 4. Bootloader unlock

### 4.1 Xiaomi procedure (Mi Unlock)

From the [official LineageOS install instructions for peridot](https://wiki.lineageos.org/devices/peridot/install/variant1):

1. Create a **Mi account** on Xiaomi's website and **add a phone number**; insert a SIM into the phone.
2. Enable Developer options (tap "MIUI Version"/"OS version" 7 times), then "Mi Unlock status" → link device to account.
3. Run the Windows-only **Mi Unlock tool** (`driver_install.exe` first so the device is detected). "The app may tell you that you have to wait up to 30 days. If it does so, please wait for the quoted amount of time before continuing! It is ideal to start this step at midnight (GMT+8), as Xiaomi only allows a limited number of devices to be unlocked each day."
4. Account limits: "one account can only unlock four unique devices every year (one HyperOS device, three MIUI devices), and even then only once every 30 days."
5. The unlock **wipes all user data** (factory reset), consistent with the AOSP requirement that LOCKED→UNLOCKED must clear `userdata` ([AOSP locking_unlocking](https://source.android.com/docs/core/architecture/bootloader/locking_unlocking)).

Community-reported granularity of the wait (72 h vs 168 h vs 30 days) varies by account/region/firmware and changes over time — **NEEDS VERIFICATION**; the only on-device-verified figure is the LOS-documented "up to 30 days". China-region accounts face additional HyperOS-era restrictions (community-points/quiz requirements) — **NEEDS VERIFICATION**.

Xiaomi devices do **not** respond to plain `fastboot flashing unlock`: unlocking is performed exclusively through the Mi Unlock tool's authenticated challenge flow (AOSP describes the standard `fastboot flashing unlock` path, which Xiaomi replaces ([AOSP locking_unlocking](https://source.android.com/docs/core/architecture/bootloader/locking_unlocking), [LOS install page](https://wiki.lineageos.org/devices/peridot/install/variant1))). *(Exact fastboot error text on peridot: NEEDS VERIFICATION.)*

### 4.2 What orange state (UNLOCKED) disables

- ABL still runs AVB but passes `AVB_SLOT_VERIFY_FLAGS_ALLOW_VERIFICATION_ERROR`; wrong signatures, wrong keys, rollback violations no longer block boot ([AVB README](https://android.googlesource.com/platform/external/avb/+/refs/heads/main/README.md)).
- The orange warning splash is shown at every boot ([AOSP device-state](https://source.android.com/docs/security/features/verifiedboot/device-state); LineageOS FAQ: "If your device is displaying a warning on every boot - there is nothing you can do about it but ignoring", [install page](https://wiki.lineageos.org/devices/peridot/install/variant1)).
- dm-verity on dynamic partitions effectively depends on vbmeta flags: after unlocking you typically flash vbmeta with `--disable-verity --disable-verification` (flags=3) so modified vendor/system images boot ([Magisk install docs — the standard command](https://github.com/topjohnwu/Magisk/blob/master/docs/install.md), [avbtool.py flag bits](https://android.googlesource.com/platform/external/avb/+/refs/heads/main/avbtool.py)).
- `fastboot flash` of boot/images becomes possible; state transitions still wipe data ([AOSP locking_unlocking](https://source.android.com/docs/core/architecture/bootloader/locking_unlocking)).

### 4.3 What stays verified — why XBL/ABL cannot be replaced

- The root of trust is a public key (or its hash) **fused into read-only storage** at manufacture; "The private part of the root of trust is known only to the device manufacturer" ([AOSP device-state](https://source.android.com/docs/security/features/verifiedboot/device-state)). On Qualcomm SoCs the ROM PBL verifies XBL against fused keys before anything else runs; unlocking the bootloader does **not** re-blow or disable these fuses, so `xbl`, `xbl_config`, `uefi`, `abl`, `tz`, `hyp`, `aop`, `devcfg`, `keymaster` etc. remain signature-checked by the PBL/XBL chain. Flashing corrupted/foreign firmware there simply fails verification (or bricks the boot chain) — the bootloader's own fastboot cannot write them successfully on a locked device, and on an unlocked device writing broken images renders the device unbootable because the *hardware* verification still applies.
- Recovery from such corruption requires **EDL (Emergency Download, 9008)** — but modern Qualcomm/Xiaomi devices authenticate EDL programmers: the Sahara/Firehose loaders are signed and restricted. The open-source [bkerler/edl](https://github.com/bkerler/edl) client documents that Xiaomi requires OEM-signed programmers ("EDL authentification" limitations listed in its README). Obtaining a usable firehose programmer for this device without an authorized Xiaomi service account is not realistic — **treat EDL as unavailable to end users** for peridot. *(Device-specific firehose availability for SM8635: NEEDS VERIFICATION.)*
- Consequently: **never flash `xbl`, `xbl_config`, `uefi`, `xbl_ramdump`, `abl`, `tz`, `hyp`, `aop`, `aop_config`, `cpucp`, `cpucp_dtb`, `shrm`, `devcfg`, `keymaster`, `uefisecapp`, `imagefv`** from unofficial sources. A multiboot design must treat the entire XBL/ABL/TZ chain as immutable and do everything from boot/init_boot/vendor_boot/recovery/vbmeta onward.

### 4.4 Fastboot commands used on this device

Boot into bootloader: `adb reboot bootloader`, or power off → hold **Volume Down + Power** until "FASTBOOT" appears ([LOS wiki](https://wiki.lineageos.org/devices/peridot/variant1/)). Verified commands from the LOS install guide ([source](https://wiki.lineageos.org/devices/peridot/install/variant1)):

```bash
fastboot flash boot boot.img
fastboot flash dtbo dtbo.img
fastboot flash init_boot init_boot.img
fastboot flash vendor_boot vendor_boot.img
fastboot flash recovery recovery.img
fastboot reboot bootloader
fastboot reboot recovery
```

Additional standard A/B fastboot (works on virtual-A/B Qualcomm devices; verify per-device output):

```bash
fastboot getvar current-slot          # active slot
fastboot set_active a|b               # switch + reset retry counters
fastboot reboot fastboot              # userspace fastbootd (needed for super/dynamic partitions)
fastboot flash vbmeta --disable-verity --disable-verification vbmeta.img   # flags=3 (data wipe possible)
fastboot flashing get_unlock_ability  # OEM-unlock setting state
```

*(The `vbmeta` command is the documented Magisk/LOS convention ([Magisk docs](https://github.com/topjohnwu/Magisk/blob/master/docs/install.md)); slot commands follow [AOSP A/B fastboot semantics](https://source.android.com/docs/core/architecture/bootloader/fastbootd). Output strings on peridot: NEEDS VERIFICATION.)*

### 4.5 Anti-rollback

- AVB rollback protection: "the device stores the last seen rollback index in tamper-evident storage … Rollback protection is having the device reject an image unless `rollback_index[n]` >= `stored_rollback_index[n]`" ([AVB README](https://android.googlesource.com/platform/external/avb/+/refs/heads/main/README.md)). peridot uses per-chain locations (boot=3, recovery=1, vbmeta_system=2 in the Lineage build, §3.3) and platform-security-patch-timestamp indexes ([BoardConfig.mk](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/BoardConfig.mk)).
- On the firmware side Xiaomi tracks an "ARB" (anti-rollback) counter carried by firmware/modem images; the XM Firmware Updater project even ships an "ARB Checker" tool ([xmfirmwareupdater.com](https://xmfirmwareupdater.com/firmware/peridot/)). Flashing very new firmware can raise stored rollback indexes such that older boot chains fail verification. *(Per-firmware ARB values for peridot: NEEDS VERIFICATION.)*
- Practical rule for a multiboot project: keep **boot/init_boot/vendor_boot/dtbo/vbmeta** images paired with a firmware generation ≥ the currently stored rollback indexes; don't downgrade firmware.

---

## 5. GKI status

### 5.1 Kernel / KMI

| Item | Value | Source |
|---|---|---|
| Kernel | Linux **6.1.174** (Lineage `lineage-23.2` branch of `android_kernel_xiaomi_sm8635`) | [kernel Makefile](https://github.com/LineageOS/android_kernel_xiaomi_sm8635/blob/lineage-23.2/Makefile), [LOS wiki kernel: 6.1](https://wiki.lineageos.org/devices/peridot/variant1/) |
| GKI branch | **android14-6.1** (KMI `6.1-android14`) — 6.1 GKI is the Android 14 KMI; the device remains on 6.1 even under Android 15/16 firmware | [AOSP GKI release builds index (android14-6.1 listed)](https://source.android.com/docs/core/architecture/kernel/generic-kernel-image); HyperOS 3 GKI kernel repo description ([harunaltair](https://github.com/harunaltair/poco-f6-gki-kernel)) |
| Kernel release format | `6.1.<sublevel>-android14-<kmi_generation>-<suffix>` | [AOSP GKI versioning scheme](https://source.android.com/docs/core/architecture/kernel/gki-versioning) |
| Configs | `gki_defconfig` + `vendor/pineapple_GKI.config` + `vendor/peridot_GKI.config` | [BoardConfig.mk](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/BoardConfig.mk), [peridot_GKI.config](https://github.com/LineageOS/android_kernel_xiaomi_sm8635/blob/HEAD/arch/arm64/configs/vendor/peridot_GKI.config) |
| Kernel source (Xiaomi OSS) | `MiCode/Xiaomi_Kernel_OpenSource` branch **`peridot-u-oss`** ("u" = Android 14 U release), Kleaf/Bazel-based build with clang (LLVM 17 in community manifests) | [MiCode branch](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/peridot-u-oss); [community Kleaf manifest](https://github.com/amackdev/peridot-kernel-manifest) |
| Ext modules | ~25 module projects: camera, display (msm drm), audio, wlan (qcacld-3.0/qca6750), datarmnet, securemsm, graphics (fgl), video, eva, touch (goodix/nxp), etc. | [BoardConfig.mk `TARGET_KERNEL_EXT_MODULES`](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/BoardConfig.mk) |

**KMI implications:** the KMI is the stable interface between the GKI kernel and loadable modules — "kernel versions with the same KMI are compatible… if the KMI is different, these kernels aren't compatible, and flashing a kernel image with a different KMI than your device may cause a bootloop" ([KernelSU KMI explainer](https://github.com/tiann/KernelSU/blob/main/website/docs/guide/installation.md); canonical definition: [AOSP GKI versioning](https://source.android.com/docs/core/architecture/kernel/gki-versioning)). GKI modules are "protected" (cannot be overridden) or "unprotected" (can be overridden by vendor modules) ([AOSP kernel modules overview](https://source.android.com/docs/core/architecture/kernel/modules)).

### 5.2 Where modules live on peridot (4 distinct sets)

| Set | Image | Count (Lineage tree) | Contents (examples) | Source |
|---|---|---|---|---|
| First-stage (boot-critical) | **vendor_boot ramdisk** | 109 from `modules.list.msm.pineapple` + `hwid.ko` | clocks (`gcc-cliffs.ko`), pinctrl, regulators, SMMU, UFS, PHYs, `qcom-scm`, WDT, RPMH | [modules.list.msm.pineapple](https://github.com/LineageOS/android_kernel_xiaomi_sm8635/blob/HEAD/modules.list.msm.pineapple), [device `modules/modules.list.first_stage`](https://github.com/LineageOS/android_device_xiaomi_peridot/tree/HEAD/modules) |
| Recovery extras | vendor_boot recovery ramdisk | first-stage + 205 second-stage modules | full boot set so recovery can mount everything | `BOARD_VENDOR_RAMDISK_RECOVERY_KERNEL_MODULES_LOAD` in [BoardConfig.mk](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/BoardConfig.mk) |
| Second-stage / vendor_dlkm | `vendor_dlkm` (in super) | 205 second-stage + 77 vendor_dlkm | display/msm_drm, touch, audio, wlan qcacld, camera, ipa | [modules.list.second_stage](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/modules/modules.list.second_stage), [modules.list.vendor_dlkm](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/modules/modules.list.vendor_dlkm) |
| GKI-owned modules | `system_dlkm` (in super) | 60 | framework-required GKI modules, loaded from system side | [modules.list.system_dlkm](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/modules/modules.list.system_dlkm) |

Per AOSP: "Modules required early in the boot process must be in `vendor_boot`" and GKI modules delivered by Google sit in `system_dlkm` on modern devices ([AOSP kernel modules overview](https://source.android.com/docs/core/architecture/kernel/modules)).

### 5.3 What replacing the kernel in `boot.img` means

- You are replacing the **GKI kernel only** — all hardware drivers are modules living in vendor_boot/vendor_dlkm. A custom kernel must either (a) keep the exact `android14-6.1` KMI (module-compatible, stock modules keep working), or (b) ship its own matching module set into vendor_boot + vendor_dlkm (+ system_dlkm if GKI modules change). Community kernel tooling for this device already produces the full image set: "Xiaomi peridot (SM8650) kernel build scripts for MiCode OSS — produces vendor_boot, vendor_dlkm, system_dlkm" ([ApexLegend007/sm8650-peridot-kernel](https://github.com/ApexLegend007/sm8650-peridot-kernel)) and GKI-module-specific builds such as "GKI 6.1 peridot msm_drm.ko build" ([hoshikv/peridot-kernel-build](https://github.com/hoshikv/peridot-kernel-build)).
- Because `boot.img` also carries the DTB, replacing the kernel can also replace/patch device trees (overlays for the panel etc. still come from `dtbo`).
- `system_dlkm` is mounted from the *system* side — a different OS image (e.g. GSI or custom ROM) brings its own `system_dlkm` contents, which is why cross-ROM kernel swaps must keep system_dlkm/vendor_dlkm coherence in mind.

---

## 6. Known community efforts on peridot

### 6.1 LineageOS (official)

- Official support, LineageOS **23.2** (Android 16), maintainer **adarshgrewal**; kernel 6.1 ([device page](https://wiki.lineageos.org/devices/peridot/variant1/)).
- Device tree: [LineageOS/android_device_xiaomi_peridot](https://github.com/LineageOS/android_device_xiaomi_peridot); kernel: [LineageOS/android_kernel_xiaomi_sm8635](https://github.com/LineageOS/android_kernel_xiaomi_sm8635) (+ [-devicetrees](https://github.com/LineageOS/android_kernel_xiaomi_sm8635-devicetrees), [-modules](https://github.com/LineageOS/android_kernel_xiaomi_sm8635-modules)); dependencies: `hardware/xiaomi` ([lineage.dependencies](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/lineage.dependencies)).
- How they build the kernel: the device tree points `TARGET_KERNEL_SOURCE` at `kernel/xiaomi/sm8635` with `gki_defconfig` + `pineapple_GKI.config` + `peridot_GKI.config`, plus the extensive `TARGET_KERNEL_EXT_MODULES` list compiled out-of-tree; modules are distributed to vendor_boot/vendor_dlkm/system_dlkm via the four module lists ([BoardConfig.mk](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/BoardConfig.mk)).
- Vendor tree: [TheMuppets/proprietary_vendor_xiaomi_peridot](https://github.com/TheMuppets/proprietary_vendor_xiaomi_peridot) (referenced via `proprietary-files*.txt` in the device tree).

### 6.2 Recoveries

- Lineage Recovery official (recovery.img, flashed per §4.4).
- TWRP trees: [Nomishaw21/twrp_peridot](https://github.com/Nomishaw21/twrp_peridot); unified SM8650-family TWRP tree covering "zorn chenfeng shennong **peridot** ruyi aurora houji" ([RWA82/android_device_xiaomi_sm8650-twrp](https://github.com/RWA82/android_device_xiaomi_sm8650-twrp)).

### 6.3 Custom kernels (repositories found; XDA thread names NEEDS VERIFICATION)

| Kernel / repo | Notes | Link |
|---|---|---|
| harunaltair/poco-f6-gki-kernel | "Custom GKI 6.1 Kernel for Poco F6 (peridot) — HyperOS 3 (Android 16)" | [GitHub](https://github.com/harunaltair/poco-f6-gki-kernel) |
| JuanArton/xiaomi_peridot_kernel | battery-focused; schedutil tuning | [GitHub](https://github.com/JuanArton/xiaomi_peridot_kernel) |
| kutemeikito/android_kernel_xiaomi_peridot | "Ryzen kernel source for POCO F6 / Peridot", based on `KERNEL.PLATFORM.3.0.r1-13300-kernel.0` | [GitHub](https://github.com/kutemeikito/android_kernel_xiaomi_peridot) |
| shyamaldasandhi/SAMXI_peridot_kernel / SamJ_Peridot_Kernel | "based on isis kernel base"; gaming kernel | [1](https://github.com/shyamaldasandhi/SAMXI_peridot_kernel), [2](https://github.com/shyamaldasandhi/SamJ_Peridot_Kernel) |
| shymax777/los_sm8635_peridot_kernel | Lineage-based SM8635 kernel work | [GitHub](https://github.com/shymax777/los_sm8635_peridot_kernel) |
| hoshikv/peridot-kernel-build | GKI 6.1 msm_drm.ko build automation (git workflow) | [GitHub](https://github.com/hoshikv/peridot-kernel-build) |
| ApexLegend007/sm8650-peridot-kernel (+ peridot-kernel-manifest) | Kleaf build environment for MiCode OSS peridot kernel; produces vendor_boot/vendor_dlkm/system_dlkm | [GitHub](https://github.com/ApexLegend007/sm8650-peridot-kernel) |
| Various ROM-org kernels (yaap, crdroid, Matrixx, RisingOS, AxionAOSP, VoltageOS, BlissROMs, PixelOS, Project-Flare, peridot-hyperos-2, sm8635-dev, PeridotLions, …) | ROM-specific kernel/module forks | found via `gh search repos peridot kernel` |

### 6.4 Existing multiboot attempts

- **No public multiboot or dual-boot project specific to peridot was found** (searches: GitHub repos/code for `peridot multiboot`, `peridot dualboot`, `peridot kexec` — zero relevant hits; XDA could not be scraped for this report — see §8).
- General-purpose tooling landscape: [DualBootPatcher](https://github.com/chenxiaolong/DualBootPatcher) ("Patches Android ROMs for dual boot support") is **archived since 2023** and predates GKI/A-B devices of this generation; KernelSU/Magisk provide root-only seams (LKM loading without kernel replacement, init_boot patching) that are the closest modern analogues ([KernelSU docs](https://github.com/tiann/KernelSU/blob/main/website/docs/guide/installation.md), [Magisk docs](https://github.com/topjohnwu/Magisk/blob/master/docs/install.md)).
- Design consequence: a peridot multiboot menu is **greenfield** — nothing to fork. The boot-slot duality (`boot_a/b`, `vbmeta_a/b`, …) plus fastbootd-accessible `super` are the only OS-switching primitives the stock chain offers.

---

## 7. Safety notes (what bricks, what's safe)

### 7.1 Hard-brick risk — do NOT touch after unlock

| Partitions | Why fatal |
|---|---|
| `xbl`, `xbl_config`, `uefi`, `xbl_ramdump` | First boot stages; corrupt = no fastboot, no charging UI, no USB handling. Recovery would need EDL (9008) with an authenticated programmer, which end users cannot obtain for Xiaomi (see §4.3, [bkerler/edl](https://github.com/bkerler/edl)) |
| `abl` | Corrupt ABL = no fastboot/no kernel start; EDL-only recovery |
| `tz`, `hyp`, `aop`, `aop_config`, `cpucp`, `cpucp_dtb`, `shrm`, `devcfg`, `keymaster`, `uefisecapp`, `imagefv`, `qupfw` | Signature-verified early firmware; wrong images fail verification or crash the boot chain; `keymaster`/`persist` damage additionally destroys keystore/encryption state |
| `persist` | Not firmware, but sensors/RIL calibration; formatting degrades functionality (mountpoint documented in [fstab.qcom](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/rootdir/etc/fstab.qcom)) |
| `fsc`/`fsg`/`modemst1`/`modemst2`, `logfs` (expected on device) | Modem NV/EFS — loss = lost IMU of radio config; listed in §8 as unverified for peridot |

General rule: the signature-verified, fuse-anchored chain (PBL → XBL → ABL → TZ) is **immutable for practical purposes** — see §4.3 for the cryptographic reason ([AOSP device-state root-of-trust](https://source.android.com/docs/security/features/verifiedboot/device-state), [AVB README](https://android.googlesource.com/platform/external/avb/+/refs/heads/main/README.md)).

### 7.2 Safely flashable after unlock (and recoverable)

| Partition | Risk profile | Notes |
|---|---|---|
| `boot` | Low | Worst case: no boot → flash stock again or use fastboot; boot-loop recoverable from fastboot |
| `init_boot` | Low | Same; Magisk uses it as root seam ([Magisk docs](https://github.com/topjohnwu/Magisk/blob/master/docs/install.md)) |
| `vendor_boot` | Low-medium | Wrong module set → bootloop or early panic; recoverable via fastboot; keep module lists consistent (§5.2) |
| `dtbo` | Low | Wrong overlays → display issues; recoverable |
| `recovery` | Low | Independent, non-A/B; flash back stock/Lineage recovery any time |
| `vbmeta` (+ `vbmeta_system`) | Low with flags | Flash with `--disable-verity --disable-verification` after unlock; wrong signed vbmeta on LOCKED device = brick, so only on UNLOCKED ([avbtool flags](https://android.googlesource.com/platform/external/avb/+/refs/heads/main/avbtool.py)) |
| `super` contents (system/vendor/product/odm/system_ext/system_dlkm/vendor_dlkm) | Low-medium | Flash via **fastbootd**; both slots share one super, so slot A/B tests are not independent (§2.4, [AOSP fastbootd](https://source.android.com/docs/core/architecture/bootloader/fastbootd)) |
| `userdata`, `metadata` | Data-loss only | Wipeable/formatable; `metadata` holds FBE keys — wiping = losing data access, not bricking ([fstab.qcom](https://github.com/LineageOS/android_device_xiaomi_peridot/blob/HEAD/rootdir/etc/fstab.qcom)) |
| `misc` | Low | BCB corruption can confuse slot/recovery selection; rewriteable from fastboot/OS |

Also note: the LOS install flow itself flashes boot/dtbo/init_boot/vendor_boot **before** recovery ("boot_stack"), because Lineage recovery requires the matching boot images ([wiki metadata](https://github.com/LineageOS/lineage_wiki/blob/main/_data/devices/peridot_variant1.yml), [install page](https://wiki.lineageos.org/devices/peridot/install/variant1)) — a good template for multiboot installers: always keep the boot-stack quartet coherent.

### 7.3 Firmware ↔ boot-image coherence

Stock firmware generations (Android 14 → 15 → 16 HyperOS) change vendor/odm/firmware expectations; LineageOS requires a specific firmware generation before flashing and warns that mismatched firmware can "permanently damage" the device experience ([install page](https://wiki.lineageos.org/devices/peridot/install/variant1)). A multiboot design must therefore pin or verify firmware compatibility per OS slot, and respect rollback indexes (§4.5).

---

## 8. NEEDS VERIFICATION

1. **Exact shipped firmware versions** for global POCO F6 and Chinese Redmi Turbo 3 (HyperOS 1.0.x strings, e.g. `OS1.0.x.y`) and the full HyperOS 2/3 version history per region. (Launch dates and generations verified; strings not.)
2. **Stock (Xiaomi) AVB rollback index locations and values** per partition (Lineage test-key values are verified; stock values are not). Also whether stock uses `vbmeta_vendor` as a separate partition — it is *not* in the Lineage `AB_OTA_PARTITIONS`/firmware lists, but a stock GPT dump would settle it.
3. **Full on-disk GPT partition list** from a peridot stock dump (e.g. presence/names of `frp`, `devinfo`, `fsc`, `fsg`, `modemst1/2`, `logfs`, `apdp`, `msadp`, `config`, `dip`, `limits`, `storsec`, `rescue`, `storage`, `miscta`-style Xiaomi partitions). Currently only build-config + fstab-verified partitions are listed (§2).
4. **Xiaomi-specific ABL behavior**: whether `devinfo` holds the unlock flag, exact orange-state splash text/behavior, whether `fastboot flashing unlock`/`oem unlock` replies with an error, fastboot variable names/output (`getvar current-slot`), and fastboot `oem` command surface.
5. **Mi Unlock waiting-time specifics** for HyperOS-era global units (72 h vs 168 h vs 30-day pool) and current China-region unlock restrictions (community-points/quiz requirements). LOS-documented: "up to 30 days" + 4 devices/year/account ([source](https://wiki.lineageos.org/devices/peridot/install/variant1)).
6. **XDA community landscape** for peridot (thread names for custom kernels like "Ryzen", "ISIS", recovery threads, any multiboot experiments): XDA forum pages were not fetchable during research (HTTP 403), so the kernel list in §6.3 is GitHub-sourced and thread-level claims are omitted.
7. **EDL/firehose programmer availability for SM8635/peridot**: whether an authenticated programmer has leaked/been shared in the wild; current bkerler/edl support status for this platform.
8. **Stock kernel build provenance details**: exact clang/Kleaf version used by Xiaomi for `peridot-u-oss` (community manifests use clang-r487747c / LLVM 17 — [amackdev manifest](https://github.com/amackdev/peridot-kernel-manifest)); whether stock vbmeta `--flags` are 0 and stock boot AVB uses Xiaomi's internal signing key.
9. **`recovery` partition A/B semantics in OTA packages** (Lineage lists `recovery` in `AB_OTA_PARTITIONS` although the GPT partition is single-slot; how stock OTA updates it needs confirmation).
10. **Exact KMI generation `k`** currently shipped by stock HyperOS 3 (Android 16) builds — needed to guarantee module compatibility for kernel swaps on latest firmware.
11. **postmarketOS peridot page** — the device is listed in the SM8635 page ([devices table](https://wiki.postmarketos.org/wiki/Qualcomm_Snapdragon_8s_Gen_3/7%2B_Gen_3_(Palawan/Lamma))) but no dedicated `xiaomi-peridot` wiki page exists yet; check again for mainline bring-up progress (codeberg `palawan-mainline`).

---

## Appendix A: Source index

Primary (device-specific):

- LineageOS wiki — device: https://wiki.lineageos.org/devices/peridot/variant1/ ; variant selector: https://wiki.lineageos.org/devices/peridot/ ; install: https://wiki.lineageos.org/devices/peridot/install/variant1
- LineageOS wiki data: https://github.com/LineageOS/lineage_wiki/blob/main/_data/devices/peridot_variant1.yml , peridot_variant2.yml
- LineageOS device tree: https://github.com/LineageOS/android_device_xiaomi_peridot (BoardConfig.mk, fstab.qcom, proprietary-firmware.txt, modules/*)
- Kernel & DT: https://github.com/LineageOS/android_kernel_xiaomi_sm8635 , https://github.com/LineageOS/android_kernel_xiaomi_sm8635-devicetrees , https://github.com/LineageOS/android_kernel_xiaomi_sm8635-modules
- Xiaomi OSS kernel: https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/peridot-u-oss
- postmarketOS SoC page: https://wiki.postmarketos.org/wiki/Qualcomm_Snapdragon_8s_Gen_3/7%2B_Gen_3_(Palawan/Lamma) (SM8650 page: https://wiki.postmarketos.org/wiki/Qualcomm_Snapdragon_8_Gen_3_(SM8650))
- POCO official specs (archived): https://www.po.co/global/product/poco-f6/specs (Wayback: 2024-05-23)
- TWRP trees: https://github.com/Nomishaw21/twrp_peridot , https://github.com/RWA82/android_device_xiaomi_sm8650-twrp
- XM Firmware Updater (peridot page incl. ARB checker): https://xmfirmwareupdater.com/firmware/peridot/

Secondary (platform/boot chain):

- AOSP generic boot partition: https://source.android.com/docs/core/architecture/partitions/generic-boot
- AOSP vendor boot partitions: https://source.android.com/docs/core/architecture/partitions/vendor-boot-partitions
- AOSP boot image header: https://source.android.com/docs/core/architecture/bootloader/boot-image-header
- AOSP bootconfig: https://source.android.com/docs/core/architecture/bootloader/implementing-bootconfig
- AOSP fastbootd: https://source.android.com/docs/core/architecture/bootloader/fastbootd
- AOSP lock/unlock: https://source.android.com/docs/core/architecture/bootloader/locking_unlocking
- AOSP device state: https://source.android.com/docs/security/features/verifiedboot/device-state
- AOSP AVB: https://source.android.com/docs/security/features/verifiedboot/avb
- AOSP AVB README: https://android.googlesource.com/platform/external/avb/+/refs/heads/main/README.md ; avbtool.py: https://android.googlesource.com/platform/external/avb/+/refs/heads/main/avbtool.py
- AOSP AVB version info: https://source.android.com/docs/core/architecture/bootloader/version-info-avb
- AOSP GKI project: https://source.android.com/docs/core/architecture/kernel/generic-kernel-image ; versioning: https://source.android.com/docs/core/architecture/kernel/gki-versioning ; modules: https://source.android.com/docs/core/architecture/kernel/modules
- Qualcomm XBL docs: https://docs.qualcomm.com/bundle/publicresource/topics/80-6520-2/xbl.html ; Qualcomm ABL (UEFI) source: https://git.codelinaro.org/clo/la/abl/tianocore
- Magisk install docs: https://github.com/topjohnwu/Magisk/blob/master/docs/install.md ; KernelSU installation: https://github.com/tiann/KernelSU/blob/main/website/docs/guide/installation.md
- EDL tooling: https://github.com/bkerler/edl ; DualBootPatcher (archived): https://github.com/chenxiaolong/DualBootPatcher
