# Kernel Tree Analysis: MiCode `peridot-u-oss` (kernel 6.1)

Status: ✅ verified against the actual tree (cloned 2026-09, commit peridot-u-oss HEAD).

## 1. Tree identity & structure

| Fact | Value |
|---|---|
| Repo | `MiCode/Xiaomi_Kernel_OpenSource`, branch `peridot-u-oss` |
| Kernel | Linux **6.1** (ACK `android14-6.1` lineage) |
| Layout | Full ACK common tree + Xiaomi/QCOM additions; **single-tree**, but it *references* multi-repo platform pieces that are NOT included |
| Missing pieces | `msm-kernel/` (build configs incl. `build.config.msm.pineapple`), `build/` (ACK build.sh), `vendor/qcom` DTS repo, `techpack/` (camera/audio external modules) |
| `build.config.msm.peridot` | 2 lines: `. ${ROOT_DIR}/msm-kernel/build.config.msm.pineapple` → **breaks without the platform manifest** |

**Consequence:** Xiaomi's own build path needs the full `kernel_platform` manifest
(KERNEL.PLATFORM r1 for SM8635). For THIS project we bypass it: a GKI device kernel is
`gki_defconfig` + device fragment — plain `make` is enough (see §3). Kleaf/Bazel and
Xiaomi build.configs are not required for a kernel-only `boot.img`.

## 2. What the device provides vs what the tree builds

| Component | Where it lives on device | In our tree? |
|---|---|---|
| GKI kernel | `boot.img` | ✅ this tree |
| peridot DTB | `vendor_boot.img` | ❌ (built from `vendor/qcom` dts repo — NOT in tree; `DTB_DIR=vendor/qcom` in build.config.msm.common) |
| DTBO overlays | `dtbo.img` | ❌ same |
| vendor kernel modules | `vendor_boot` (1st stage) + `vendor_dlkm` (2nd stage) | ❌ (techpack + vendor modules) |

So: **our rebuilt `boot.img` must not contain a DTB** (ABL picks DTB from `vendor_boot`) —
kernel-only header-v4 boot image is correct for GKI. ✔ matches stock layout.

## 3. Config system (verified)

- Base: `arch/arm64/configs/gki_defconfig`
- Device fragment: `arch/arm64/configs/vendor/peridot_GKI.config` (Xiaomi device bits:
  MI hardware IDs, fingerprint, zram, haptics, …) — used when building the device-flavored GKI
- `peridot_consolidate.config` = same idea for the "consolidate" (non-GKI, full-featured) variant
- Kbuild: `scripts/kconfig/merge_config.sh` available for fragment merging ✔

Build recipe used by this project:
```
make O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 gki_defconfig
scripts/kconfig/merge_config.sh -m out/.config \
    arch/arm64/configs/vendor/peridot_GKI.config \
    arch/arm64/configs/vendor/multiboot.fragment
make O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 olddefconfig
make O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 -j$(nproc) Image.gz Image.lz4
```
(`build.config.gki.aarch64` lists both `Image.lz4` and `Image.gz` as GKI outputs — we ship both;
default `boot.img` uses `Image.gz` for maximum ABL compatibility.)

## 4. Multiboot-relevant config state (verified by grep)

| Option | State in `gki_defconfig` | Impact |
|---|---|---|
| `CONFIG_KEXEC` | **not set** | old `kexec_load` syscall unavailable → our fragment enables it |
| `CONFIG_KEXEC_FILE` | **not set** | `kexec_file_load()` unavailable → enabled by fragment |
| `CONFIG_KEXEC_SIG` | (depends on KEXEC_FILE) | verify-if-signed semantics; no FORCE → unsigned mainline kernels acceptable |
| `CONFIG_BOOT_CONFIG` | **y** | bootconfig supported (menu can consume/extend cmdline) |
| `CONFIG_BLK_DEV_LOOP` | **y** (+16 devices) | per-ROM image loop mounts work out of the box ✔ |
| `CONFIG_MODVERSIONS` | y | KMI symbol versioning on |
| `CONFIG_MODULE_SIG` / `_PROTECT` | y / **y** | ⚠ see §5 |
| `CONFIG_DRM` | y | DRM core built-in |
| `CONFIG_FB` | **not set** | **no fbdev!** `/dev/graphics/fb0` does NOT exist on GKI → menu must use DRM |
| `CONFIG_DRM_SIMPLEDRM` | **not set** | enabled by our fragment so the menu has a display *before* vendor modules load |
| `CONFIG_DRM_MSM` | m (ACK defconfig) | display comes later from `vendor_dlkm` modules — after our menu |

⚠ **Display timing finding:** at pre-init menu time, the qcom display stack (DRM_MSM, panel
drivers, PMIC regulators) is NOT loaded yet — they're vendor modules loaded by first-stage init.
The only display available is the **ABL-provided framebuffer** exposed by `simpledrm`
(`CONFIG_DRM_SIMPLEDRM=y` in our fragment). This is exactly how it works on mainline — ABL
publishes the framebuffer in the DT (`/reserved-memory` + `simple-framebuffer`).
NEEDS VERIFICATION on peridot: confirm ABL fills the simple-framebuffer node (check
`/proc/device-tree/reserved-memory/framebuffer*` on the running device).

## 5. Module signing / KMI risk matrix

**Vermagic insight (R3, doc 05 §2.4/§5):** stock peridot `system_dlkm` modules carry
`vermagic=6.1.75-android14-11-g16c5f6cd5e9b-ab12268515 … modversions aarch64`. With
`CONFIG_MODVERSIONS=y` the kernel **relaxes the vermagic comparison** (release string may
differ; only symbol CRCs must match) — that is exactly how community peridot kernels boot
with stock vendor modules. Two consequences:
1. Our rebuilt kernel does NOT need to reproduce the stock release string.
2. Symbol CRCs must not change → clang version matters (Xiaomi pinned `clang-r487747c`;
   it is not downloadable, community uses `clang-r530567` successfully — NEEDS VERIFICATION
   on device that stock modules load), and config changes must stay additive.

Also note `gki_defconfig` facts verified by R3: `CONFIG_MODULE_SIG_PROTECT=y`,
`CONFIG_MODULE_SCMVERSION=y` (module scmversion stamping — shallow CI clone is fine),
`CONFIG_CFI_CLANG=y`, `CONFIG_DEBUG_INFO_BTF=y`, `CONFIG_MODULE_ALLOW_BTF_MISMATCH=y`,
stock `CONFIG_CMDLINE` contains `bootconfig`.

| Scenario | Result |
|---|---|
| Our kernel + stock vendor modules, `MODULE_SIG_PROTECT=y` | modules signed with Xiaomi key ≠ our trusted key → **load rejected** (EKEYREJECTED) |
| Our kernel, `MODULE_SIG_PROTECT=n` (our fragment) | modules load (sig optional) ✔ — trade-off: no enforced module signature policy |
| KMI breakage (removed/changed exported symbols) | `MODVERSIONS=y` CRC mismatch → module load failure regardless of signing → keep changes **additive only** (enabling KEXEC/SIMPLEDRM adds config, exports nothing removed) ✔ |

Our fragment therefore unsets `CONFIG_MODULE_SIG_PROTECT` (additive-ish, config-only) and
changes nothing else in the ABI surface.

## 6. Bootimage format facts

- `boot.img` header version 4, pagesize 4096, kernel `Image.gz` (GKI also emits lz4)
- No DTB in boot (GKI), no ramdisk in `boot` on Android 13+ (generic ramdisk lives in `init_boot`)
- `init_boot.img`: header v4, **ramdisk only** (contains `/init`) → our injection point
- `vendor_boot.img`: vendor ramdisk + `dtb` + `vendor_ramdisk_table` + bootconfig → our `.rc` injection point
- vbmeta: after unlock, `fastboot flash vbmeta --disable-verity --disable-verification` is the
  standard orange-state procedure

## 7. Open items

- [ ] Verify ABL exposes simple-framebuffer DT node on peridot (needs a live device / dumped DTB)
- [ ] ~~Verify whether peridot stock `boot.img` embeds a DTB~~ **RESOLVED by R3 (doc 05 §4):**
      peridot boot.img is kernel-only (no DTB — DTB lives in vendor_boot; pagesize 4096
      verified from shipped dtbo.img header bytes). CI packaging (kernel-only, v4) is correct.
- [ ] Confirm whether Xiaomi `peridot_GKI.config` alone yields a bootable device kernel without
      extra QCOM fragments from the missing platform repos (first CI run will tell)
- [ ] Volume-key evdev node naming on SM8635 (`gpio-keys`) — enumerate on device
- [ ] kexec handoff behavior on Qualcomm (hyp/policy) — experimental track
