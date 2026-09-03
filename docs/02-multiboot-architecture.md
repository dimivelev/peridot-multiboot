# Multi-OS Boot Architecture for POCO F6 (peridot)

**TL;DR:** We cannot touch the signed bootloader (XBL/ABL), so the boot menu is implemented as
a **static aarch64 binary placed as `/init` inside a repacked `init_boot` (generic ramdisk)**.
It draws a framebuffer menu, stores the selection on the unencrypted `/metadata` partition,
then execs the real Android `init`. Second-stage switching (multiple Android ROMs) happens via
loop-mounted per-ROM images; booting non-Android OSes uses **kexec** from an early point, or
`fastboot boot` of a mainline image as a robust fallback. The GKI kernel is rebuilt from
`peridot-u-oss` with minimal, KMI-safe config changes (KEXEC enabled) and is built/kept by
GitHub Actions.

## 0. Constraints (why this design)

| Constraint | Consequence |
|---|---|
| XBL/ABL are signature-verified (fused) | No custom bootloader menu. Menu must run *after* ABL loads images. |
| `boot`/`init_boot`/`vendor_boot` are freely flashable once unlocked (orange state) | These are our injection points. |
| GKI: kernel in `boot` must stay KMI-compatible with `vendor_boot`/`vendor_dlkm` modules | Only additive config changes (e.g. `CONFIG_KEXEC=y`) or we ship module-load fixes. |
| `/data` is FBE-encrypted and mounted late | Pre-init menu cannot read `/data` → store choice on **`/metadata`** (unencrypted) or pass via kernel cmdline. |
| OTA updates overwrite `boot`/`init_boot` | Multiboot must be re-applied after every OTA (automate later via `avbroot`-style OTA patching). |

## 1. Boot flow overview (stock → modified)

```
STOCK:
XBL → ABL(verify) → boot.img(kernel 6.1 GKI) + init_boot(generic rd: /init)
                    + vendor_boot(vendor rd: fstab, .rc, modules) + DTB
   → first-stage init (mounts system/vendor from super, loads vendor modules)
   → switch_root → second-stage init → Android

MODIFIED (v1):
XBL → ABL(verify, orange state OK) → same images, but init_boot ramdisk contains:
        /init          -> our bootmenu binary (static musl)
        /init.real     -> original Android init binary
   → kernel unpacks all ramdisks, runs /init = bootmenu:
        1. mount devtmpfs/sysfs/proc (menu runs very early, /dev may be empty)
        2. draw menu on /dev/graphics/fb0 (DRM dumb-buffer fallback)
        3. read volume keys from /dev/input/eventX (Vol- = down, Vol+ = up, Power = select)
        4. 5 s timeout → default entry (last choice, stored in /metadata)
        5. write selection → /metadata/multiboot/active  (mount metadata if needed)
        6. exec /init.real  → Android boots; later stages read the choice
```

Nothing upstream (XBL/ABL/vbmeta) changes, so there is **no brick risk beyond normal
custom-ROM flashing** — and `fastboot flash boot boot.stock.img` always restores stock.

## 2. The three OS classes and how each boots

### 2.1 Additional Android ROMs (HyperOS/LineageOS/…)

Modern dynamic partitions make chenxiaolong-style "directory-per-ROM on one `/data`" hard —
each ROM owns logical partitions inside `super`. Two workable schemes:

**Scheme A — per-ROM image files + loop mounts (recommended)**
- Each ROM's system/product/vendor/system_ext images live as files:
  `/data/multiboot/romN/system.img` (erofs or ext4, no dm-verity), plus
  `/data/multiboot/romN/data/` (its own f2fs/ext4 image file or real directory + own FBE policy).
- A small `multiboot.rc` injected into the **vendor_boot vendor ramdisk** (or a
  `first-stage` exec'd helper) reads `/metadata/multiboot/active`, `losetup`s the chosen
  images and mount-binds them over `/system` etc. before `post-fs`.
- GKI kernel already has `CONFIG_BLK_DEV_LOOP=y` (verify in 04-kernel-analysis.md).
- Pros: super stays stock for the "primary" ROM; each ROM image is a plain file, trivially
  backed up; Cons: needs ~8–16 GB free per extra ROM; loop+erofs support must exist (6.1 has it).

**Scheme B — second super / resized logical partitions**
- Shrink `userdata`, create `super_mb`, and put ROM N's logical partitions there. Cleaner
  mounts but requires re-partitioning (high risk, not v1).

### 2.2 Linux distros that reuse the Android kernel (Halium-style, quickest path)

- A rootfs (Ubuntu Touch / postmarketOS-alien / plain systemd rootfs) packaged as an ext4
  image file, booted with the **same peridot kernel** through a minimal "Linux-on-Android-kernel"
  init (mount rootfs, `switch_root`). Menu entry type: `linux-halium`.
- This boots *a non-Android OS* today with zero mainline work — display/touch/wifi come from
  the Android kernel drivers + userspace glue.
- kexec not required if we boot it as an init-variant: menu writes choice, `init.real` is
  replaced for that boot by `init.linux` which loop-mounts the rootfs and switch_roots into it.

### 2.3 True mainline Linux (kernel 6.x mainline, SM8635 DTS)

- Needs a mainline DTB for peridot: start from `sm8550-mtp.dts`/`sm8650` mainline DTS (8s Gen 3
  is pin/clk close to those), enable UART/console first. Track postmarketOS & Linaro qcomlt work.
- Two launch paths:
  1. **kexec (fast path, experimental):** menu binary calls `kexec_file_load()` on our
     KEXEC-enabled GKI kernel *before* vendor modules load, jumps to mainline kernel+DTB+initrd.
     Risk: Qualcomm hyp/rpmh/smem handoff state; historically needs a "hardboot" piggyback.
     Mark as experimental until proven on SM8635.
  2. **fastboot path (robust):** menu → `reboot bootloader` → user/automation runs
     `fastboot boot mainline-peridot.img`; later: ABL can't be scripted, so wrap with a helper
     PC script or a small Android app that reboots with `fastboot` intent. This always works.

## 3. Components in this repo

| Component | Location | Notes |
|---|---|---|
| `bootmenu` | `bootmenu/` | static C binary, fbdev + evdev, ~60 KB musl-static |
| kernel config patch | `kernel-patches/0001-enable-kexec-multiboot.patch` | additive defconfig deltas |
| init_boot repacker | `scripts/repack-initboot.sh` | cpio: insert `init`+`init.real`, new `bootconfig`? keep header v4 |
| vendor_boot repacker | `scripts/repack-vendorboot.sh` | adds `multiboot.rc`, later: ramdisk modules untouched |
| image builder | `scripts/make-images.sh` | builds per-ROM `system.img` from ROM zips (fastboot payload extraction) |
| CI | `.github/workflows/build-kernel.yml`, `build-bootmenu.yml` | GitHub Actions, artifacts + releases |
| flash/rollback | `scripts/flash.sh` | with `--stock-restore` |

## 4. Boot menu spec (v1)

- Entries: `Android (default ROM)`, `ROM 2`, `Linux rootfs`, `Mainline (kexec, exp.)`,
  `Recovery`, `Fastboot`, `Reboot`.
- Navigation: Vol± (evdev `KEY_VOLUMEUP=115`, `KEY_VOLUMEDOWN=114`), select `KEY_POWER=116`.
- Choice persistence: `/metadata/multiboot/active` (plain text; metadata is unencrypted ext4/f2fs).
- Draw: **DRM dumb buffer on `/dev/dri/card0`** is the real GKI path — `CONFIG_FB` is not
  set in `gki_defconfig` (verified, docs/04 §4), so `/dev/graphics/fb0` does not exist.
  Our kernel fragment enables `CONFIG_DRM_SIMPLEDRM=y` so simpledrm binds the
  ABL-provided framebuffer before vendor display modules load. Fallbacks: fbdev backend
  (non-GKI custom kernels), then blind mode (timeout + key-only selection; choice still
  persisted to /metadata).
- Watchdog safety: pet `watchdogd`-style `/dev/watchdog` every loop or set timeout ≥ 60 s so
  a hang can't brick (reboot fallback is fine — nothing is half-written yet).

## 5. Failure modes & mitigations

| Failure | Mitigation |
|---|---|
| Menu binary crashes pre-init | `init.real` exec wrapped: menu always execs even on error path; unit-test in CI (qemu-aarch64 smoke test) |
| KMI breakage → vendor modules refuse to load | Config delta kept additive; CI diff `abi_gki_aarch64.stg` symbol list; if broken, ship `CONFIG_MODULE_SIG_FORCE=n` variant + document `enforce` symlink trick |
| OTA wipes multiboot | Flash script re-applies after OTA; later: avbroot OTA integration |
| kexec handoff hangs | experimental only; default path = fastboot boot |
| /metadata missing/corrupt | default entry = last known good = Android |

## 6. What is explicitly OUT of scope (v1)

- Touchscreen in menu (keys only), animations, encrypted choice storage
- Modifying XBL/ABL (impossible), re-partitioning (v2 Scheme B)
- Windows-on-ARM / other non-Linux OSes
