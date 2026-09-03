# 05 — GitHub Actions CI pipeline for peridot (POCO F6 / Redmi Turbo 3) GKI 6.1 kernel builds

Research subagent R3 deliverable. Everything marked ✅ was verified against a live source
(fetched raw file, GitHub API response, or binary header) during this research session.
Uncertain items are tagged **NEEDS VERIFICATION** and collected in §9.

## TL;DR

1. **Runner:** `ubuntu-24.04`, x64, on a **public** repo (4 vCPU / 16 GB RAM / 14 GB root SSD).
   On private repos the same label is only 2 vCPU / 8 GB — build times roughly double.
   Do **not** use `ubuntu-22.04`: GitHub announced its deprecation (brownouts from
   2026-03, retirement 2027-04). The runner already keeps a 4 GB swapfile on `/mnt`
   (75 GB disk); add a swapfile or zram on top for LTO/KASAN-heavy links.
2. **Toolchain:** AOSP/Google clang prebuilt tarball. Xiaomi's peridot-u-oss pins
   `clang-r487747c` (android14-6.1, KMI generation 11); community peridot builds happily
   use newer AOSP clang (`r530567` verified downloadable from `android.googlesource.com`
   `main`) — but note that **clang version can shift MODVERSIONS symbol CRCs**, so for
   module compatibility with stock vendor modules the *stock-consistent* choice is the
   pinned r487747c.
3. **Build style:** Xiaomi's peridot tree supports both the legacy `build/build.sh` +
   `build.config.*` flow and **Kleaf (Bazel)**. Neither is convenient on Actions
   (both want a full multi-repo `repo` workspace with prebuilts). Every real-world
   peridot/GKI Actions pipeline we inspected (§3) instead does a **standalone
   `make LLVM=1 LLVM_IAS=1 ARCH=arm64 O=out`** build with a merged defconfig:
   `gki_defconfig` + `vendor/pineapple_GKI.config` + `vendor/peridot_GKI.config`
   (+ our multiboot fragment).
4. **boot.img:** peridot is Android-14 GKI: `boot.img` = header v4, page size 4096,
   base `0x80000000`, kernel = `Image.gz` (no DTB inside!), empty/no ramdisk (generic
   ramdisk lives in `init_boot`); DTB is in `vendor_boot`, DTBO separate (verified
   page_size=4096, 2 entries, from the shipped `dtbo.img` header bytes). Best practice
   used by peridot custom kernels: ship an **AnyKernel3** zip that flashes `boot` and
   lets magiskbot repack on-device (auto-compression, auto vbmeta flag patch).
5. **KMI / modules:** stock peridot system_dlkm modules carry
   `vermagic=6.1.75-android14-11-g16c5f6cd5e9b-ab12268515 … modversions aarch64`
   (verified from a shipped `zram.ko`). Our rebuilt kernel must reproduce this exact
   `UTS_RELEASE` and must not change module-facing symbol CRCs, or stock
   system_dlkm/vendor_dlkm modules (zram, msm display stack, etc.) will refuse to load.
   Enabling `CONFIG_KEXEC` via a config fragment is exactly the kind of change that
   *can* be KMI-safe; see §5 for the rules.
6. **Pipeline:** one `build` job (matrix: `stock` / `multiboot` defconfig) + one
   least-privilege `release` job; cache clang tarball + ccache; artifacts
   `Image`, `Image.gz`, `boot.img`, AnyKernel3 zip, `.config`; release via
   `softprops/action-gh-release`. Expected 40–90 min cold, 15–30 min warm ccache.

---

## 1. Runner choice

### 1.1 GitHub-hosted specs (verified)

From the GitHub docs source (`github/docs`,
`data/reusables/actions/supported-github-runners.md`, fetched 2026-02) and
`actions/runner-images` README:

| Label | Public repo | Private repo | Storage |
|---|---|---|---|
| `ubuntu-latest` / `ubuntu-24.04` (x64) | 4 vCPU / 16 GB RAM | 2 vCPU / 8 GB RAM | 14 GB free root SSD |
| `ubuntu-24.04-arm` | 4 vCPU / 16 GB | 2 vCPU / 8 GB | 14 GB |
| `ubuntu-22.04` (x64) | 4 vCPU / 16 GB | 2 vCPU / 8 GB | 14 GB |
| `ubuntu-slim` | 4 vCPU / 16 GB (container, no `sudo mount`, unprivileged) | n/a | 14 GB |

- Docs: https://docs.github.com/en/actions/reference/runners/github-hosted-runners
- Image definitions: https://github.com/actions/runner-images (VM size for Ubuntu images
  is `Standard_D4s_v4`, OS disk 75 GB — verified in
  `images/ubuntu/templates/variable.ubuntu.pkr.hcl` and `locals.ubuntu.pkr.hcl`).

**Critical for this project:** if the multiboot repo is private, `ubuntu-24.04` is a
**2-core/8 GB** box. Kernel builds with GKI's default `KASAN=y` + `DEBUG_INFO` configs
are RAM-hungry (LTO-ish link phases can OOM at 8 GB). Options: keep the repo public,
use a paid larger runner, or trim debug configs in the multiboot fragment.

### 1.2 Do not use ubuntu-22.04 anymore

`actions/runner-images` issue #14254 (fetched): *"Deprecation will begin on September
17th 2026 and the images will be fully unsupported by April 17th 2027"* for
`ubuntu-22.04`/`ubuntu-22.04-arm`, with brownouts (jobs temporarily fail) starting
March 2026. The folklore "use 22.04 for clang-17-era kernels" is now obsolete:

- AOSP clang prebuilts are fully self-contained (bundled glibc/`libc++`/binutils),
  so `clang-r487747c`/`r530567` run fine on 24.04 (both used on 24.04 runners in the
  wild — see §3.1). **NEEDS VERIFICATION**: run one build to confirm no
  `libtinfo5`/`libncurses5`-type runtime gap for r487747c specifically.
- Only truly ancient toolchains (GCC 4.9-era, old `mkbootimg`, some GCC builds needing
  `libncurses5`) ever justified 22.04.

Recommendation: **`runs-on: ubuntu-24.04`**, pinned (never `ubuntu-latest`) so image
drift can't break a working pipeline.

### 1.3 Disk space & swap

- Root FS is 84 GB total / ~14 GB free after preinstalled software (verified via
  `jlumbroso/free-disk-space` README `df` capture: `/dev/root 84G 53G 31G avail` on
  20.04; current 24.04 images leave ~14–21 GB).
- `/mnt` is a separate **resource disk (~65–75 GB)** on which the image configures a
  **4096 MB swapfile** (verified in `actions/runner-images`
  `images/ubuntu/scripts/build/configure-environment.sh`: waagent
  `ResourceDisk.EnableSwap=y`, `SwapSizeMB=4096`). Use `/mnt` for anything big
  (ccache, out dirs) — it is *not* persisted across jobs.
- Space reclaim (community standard):
  - `jlumbroso/free-disk-space@main` (~31 GB reclaimed in ~3 min, verified README):
    `android: true, dotnet: true, haskell: true, large-packages: true,
    docker-images: true, tool-cache: false, swap-storage: false` (keep swap-storage,
    you want the swapfile).
  - Or `easimon/maximize-build-space@master` (LVM-joins root+`/mnt`) — used by
    Xiaomichael/OnePlus-Actions for 5.10/5.15 builds (§3.3).
  - Or hand-rolled `rm -rf /usr/local/lib/android /usr/share/dotnet /opt/ghc … &&
    apt-get clean` — the xiaoleGun template does the action-based version.
- Swap for the final link: real peridot pipelines do either
  - an extra 8 GB **swapfile on /mnt** (xiaoleGun template: `fallocate -l 8G
    /mnt/swapfile; mkswap; swapon`), or
  - a **24 GB zram** swap with `vm.swappiness=180` (Lu5ck peridot workflow, §3.2) —
    faster than disk when the box is mostly swapping compiler metadata.

### 1.4 Self-hosted / larger runners

Not needed for a single-device kernel: public-repo 4-core is enough with ccache.
If the repo must be private: GitHub **larger runners** (Team/Enterprise plans,
`ubuntu-24.04` 8/16/32/64-core labels) or a self-hosted box. Self-hosted caveats
(noted by every template maintainer): untrusted workflow code + `sudo` = fine for
your own repo, but you must handle ccache/toolchain persistence yourself. None of
the inspected kernel pipelines use self-hosted runners.

---

## 2. Toolchains for GKI 6.1 (android14-6.1, SM8635)

### 2.1 What Xiaomi's peridot-u-oss actually pins ✅

Fetched from `MiCode/Xiaomi_Kernel_OpenSource` branch `peridot-u-oss`
(commit `062233df735dd3db2e20aea2f7d3f87c0b1ffde2`, "Kernel: Xiaomi kernel changes for
Redmi Turbo 3 Android U"):

`build.config.constants` (repo root):
```
BRANCH=android14-6.1
CLANG_VERSION=r487747c
AARCH64_NDK_TRIPLE=aarch64-linux-android31
```
`build.config.common`:
```
KMI_GENERATION=11
LLVM=1
DEPMOD=depmod
CLANG_PREBUILT_BIN=prebuilts/clang/host/linux-x86/clang-${CLANG_VERSION}/bin
KCFLAGS="${KCFLAGS} -D__ANDROID_COMMON_KERNEL__"
HERMETIC_TOOLCHAIN=${HERMETIC_TOOLCHAIN:-1}
```
So Xiaomi builds peridot with **AOSP clang r487747c, LLVM=1, KMI android14-11** —
matching the stock firmware `vermagic=6.1.75-android14-11-g…` we verified (§5.2).

### 2.2 How Xiaomi builds it (build.config + Kleaf both present) ✅

Repo layout at `peridot-u-oss` root (verified via git trees API):
- Legacy flow: `build.config.*` (root, ACK-style) + `build.config.msm.<target>` +
  `build_with_bazel.py`; build entry is the AOSP `build/build.sh`
  (`BUILD_CONFIG=build.config.msm.peridot build/build.sh` from a full kernel_platform
  workspace — Xiaomi does **not** ship `build.sh` in this repo; the build repo is a
  separate `build/kernel` project, see the community manifest in §3.4).
- Kleaf flow: `BUILD.bazel`, `peridot.bzl`, `xiaomi_sm8650_common.bzl`,
  `msm_kernel_la.bzl`, `image_opts.bzl`, … `peridot.bzl` defines the `peridot` target
  with **in-tree modules** `zram.ko`, `zsmalloc.ko`, `mi_fp.ko`, `fsa4480-i2c.ko`,
  `aw8697-haptic.ko`, `si_haptic.ko` (+ xiaomi-common `hwid.ko`, `ir-spi.ko`,
  `dump_display.ko`, mtd modules, `sched-walt-debug.ko`).
- A community repo manifest reconstructs the full workspace
  (`amackdev/peridot-kernel-manifest`, `default.xml`, fetched): projects
  `sm8650-peridot-kernel` (root scripts), `peridot-msm-kernel` (msm-kernel,
  `peridot-u-oss`), devicetree at `msm-kernel/arch/arm64/boot/dts/vendor`,
  `build/kernel` Kleaf at `android14-release`, `prebuilts/kernel-build-tools`
  (avbtool/mkdtimg/depmod), AOSP `mkbootimg` at `tools/mkbootimg`, and
  `kernel_xiaomi_sm8650-modules` (out-of-tree vendor modules: audio, camera, display,
  wlan/cnss2/qcacld, touch, video, …). It also documents that
  `prebuilts/clang/host/linux-x86/clang-r487747c` "downloaded from AOSP" is set up by
  `build.sh` (not by repo).

Chain of build.configs (all fetched, verified):
```
build.config.msm.peridot
  └─ . build.config.msm.pineapple
        ├─ BOOT_IMAGE_HEADER_VERSION=4
        ├─ PAGE_SIZE=4096
        ├─ BASE_ADDRESS=0x80000000
        ├─ BUILD_VENDOR_DLKM=1  PREPARE_SYSTEM_DLKM=1  BUILD_INIT_BOOT_IMG=1
        ├─ DT_OVERLAY_SUPPORT=1
        ├─ KERNEL_VENDOR_CMDLINE+=' bootconfig '
        ├─ MSM_ARCH=pineapple  VARIANTS=(consolidate gki)   # gki is the device variant
        ├─ . msm-kernel/build.config.common   (LLVM=1, KMI_GENERATION=11, clang-r487747c)
        ├─ . msm-kernel/build.config.aarch64
        ├─ . build.config.msm.common
        └─ . build.config.msm.gki
```
`build.config.msm.gki` (fetched from `crdroidandroid/android_kernel_xiaomi_sm8635`
branch `16.0`, which is the peridot/SM8635 kernel tree LineageOS uses):
```
DEFCONFIG="gki_defconfig"
BUILD_BOOT_IMG=1
BUILD_INITRAMFS=1
KMI_SYMBOL_LIST=android/abi_gki_aarch64_qcom
KMI_SYMBOL_LIST_STRICT_MODE=1
GKI_KMI_ENFORCED=1  KMI_ENFORCED=1
MAKE_GOALS="modules dtbs"
# gki variant:
GKI_TRIM_NONLISTED_KMI=1
apply_defconfig_fragment arch/arm64/configs/vendor/${MSM_ARCH}_GKI.config \
                          vendor/${MSM_ARCH}-gki_defconfig
```
The peridot device fragment is `arch/arm64/configs/vendor/peridot_GKI.config`
(verified present; first lines set `CONFIG_MI_HARDWARE_ID=m`, `CONFIG_ZRAM=m`,
`CONFIG_ZSMALLOC=m`, `CONFIG_INPUT_AW86927_HAPTIC=m`, …). The MiCode commit message
for `peridot-u-oss` states *"The kernel config file used is peridot_defconfig"*.
**NEEDS VERIFICATION** (cosmetic): whether the *device* build merges
`peridot_GKI.config` on top of `pineapple_GKI.config` via
`build.config.msm.peridot` overrides in the private msm-kernel repo, or whether the
Kleaf `peridot.bzl` target is authoritative. Community builds (§3) merge both.

`build.config.msm.common` (fetched, crdroid sm8635 16.0):
```
KERNEL_BINARY=Image
MAKE_GOALS+=" dtbs"
install_dtbs: make INSTALL_DTBS_PATH=... dtbs_install   # flat staging of *.dtb/*.dtbo
make_dtbo_img: mkdtboimg create dtbo.img --page_size=${PAGE_SIZE} ${DIST_DIR}/*.dtbo
```

### 2.3 The AOSP clang prebuilts — verified URLs

- Google clang **r530567** from `main` (used by a real peridot build, §3.1):
  `https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/refs/heads/main/clang-r530567.tar.gz` → HTTP 200 ✅
- **r487747c is NOT on `main`** (only the newest few clangs are; `+archive` for it
  returns 400 ✅ tested). Ways to get the Xiaomi-pinned r487747c:
  - `ci.android.com` submitted-build artifact, pattern
    `https://ci.android.com/builds/submitted/<BUILD_ID>/linux/latest/clang-<BUILD_ID>-linux.tar.gz`
    (verified 200 for build 9978441 ✅; need the build id that shipped r487747c —
    **NEEDS VERIFICATION**).
  - Community mirrors: `ZyCromerZ/Clang` GitHub releases (used by Lu5ck's peridot
    pipeline ✅) and `Neutron-Toolchains/antman` (referenced but commented out in the
    same script). Both ship *newer* clang than r487747c.
  - Mirrors of the whole `prebuilts/clang/host/linux-x86` tree with historical
    versions exist (e.g. search `clang-r487747c`); **NEEDS VERIFICATION** for a
    specific long-lived mirror URL.
- ci.android.com branch-name guesses (`aosp-kernel-build-tools` etc.) returned 404 ✅
  tested — the `submitted/<id>` shape above is the reliable one.

**Toolchain choice for this project:** default to the pinned `clang-r487747c`
(firmware-CRC-fidelity, see §5) with a fallback option to `r530567` for pure-testing
builds. Both ~1.2–2 GB tarballs; cache them with `actions/cache` (keyed by version).

### 2.4 Build env vars & the make invocation

Community-standard invocation for peridot GKI (from §3.1/§3.2, both real peridot
pipelines):

```bash
export PATH="<clang>/bin:$PATH"
export ARCH=arm64 SUBARCH=arm64
export LLVM=1 LLVM_IAS=1
# optional explicit tool selection (hoshikv build.sh):
export LD=ld.lld AR=llvm-ar NM=llvm-nm STRIP=llvm-strip \
       OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump READELF=llvm-readelf

# defconfig: kconfig multi-fragment syntax (Lu5ck):
make O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 \
     gki_defconfig vendor/pineapple_GKI.config vendor/peridot_GKI.config
# …or merge_config.sh (hoshikv):
scripts/kconfig/merge_config.sh -m -r arch/arm64/configs/gki_defconfig \
    arch/arm64/configs/vendor/pineapple_GKI.config \
    arch/arm64/configs/vendor/peridot_GKI.config
make O=out ARCH=arm64 olddefconfig

make O=out -j"$(nproc)" LLVM=1 LLVM_IAS=1 ARCH=arm64 Image dtbs
# GKI boot kernel = gzip of Image (hoshikv does exactly this):
gzip -9 -k out/arch/arm64/boot/Image   # → Image.gz
```
ccache env (verbatim from Xiaomichael/OnePlus-Actions):
```
CCACHE_COMPILERCHECK="%compiler% -dumpmachine; %compiler% -dumpversion"
CCACHE_NOHASHDIR="true"
CCACHE_HARDLINK="true"
CCACHE_MAXSIZE=8G
CC="ccache clang"
```
Xiaomi's own `LLVM=1` + hermetic toolchain is honored by the same env; `LLVM_IAS=1`
matters for 6.1 (GNU as fallback otherwise). `CROSS_COMPILE=aarch64-linux-gnu-` is
unnecessary under `LLVM=1` but harmless and still added by some workflows (§3.3).

Facts about the config we inherit (verified from
`arch/arm64/configs/gki_defconfig`, MiCode peridot-u-oss = ACK android14-6.1):
`CONFIG_MODULES=y`, `CONFIG_MODULE_UNLOAD=y`, `CONFIG_MODULE_SIG=y`,
`CONFIG_MODULE_SIG_PROTECT=y`, `CONFIG_MODVERSIONS=y`, `CONFIG_CFI_CLANG=y`,
`CONFIG_DEBUG_INFO_BTF=y`, `CONFIG_MODULE_ALLOW_BTF_MISMATCH=y`,
`CONFIG_KASAN=y`, `CONFIG_KASAN_HW_TAGS=y`, `CONFIG_MODULE_SCMVERSION=y`,
`CONFIG_CMDLINE="console=ttynull … bootconfig ioremap_guard"`.
**No `CONFIG_KEXEC`** (also verified against ACK `android14-6.1` `gki_defconfig`
directly on android.googlesource.com) — see §5.4.

---

## 3. Real-world workflow files inspected

Each row = a workflow I actually fetched and read during this session.

| # | Repo | Path | Device/SoC | Notable |
|---|---|---|---|---|
| 3.1 | `hoshikv/peridot-kernel-build` | `.github/workflows/build.yml` | **peridot (SM8635), GKI 6.1 msm_drm.ko** | closest analogue to this project |
| 3.2 | `Lu5ck/android_custom_kernel_builds` | `.github/workflows/peridot.yml` + `actions/peridot/{action.yml,entrypoint.sh,DockerFile}` | **peridot LOS kernel** | docker build, zram swap |
| 3.3 | `Xiaomichael/OnePlus-Actions` | `.github/workflows/Build Kernel Only.yml` | OnePlus GKI 5.10–6.6 incl. **sm8635** option | most complete GKI-only template (580★) |
| 3.4 | `xiaoleGun/KernelSU_Action` | `.github/workflows/build-kernel.yml` + `scripts/*.sh` | generic (config.env profiles) | 718★ template, modern hygiene |
| 3.5 | `dabao1955/kernel_build_action` | `action.yml` (composite) + `.github/workflows/main.yml` | generic | classic "one input = one build" action (180★) |
| 3.6 | `jiganomegsdfdf/ubuntu-oneplus-aston` | `aston-kernel_build.sh` | SM8550 mainline (non-GKI) | manual `mkbootimg --header_version 4` |

### 3.1 `hoshikv/peridot-kernel-build` — peridot GKI 6.1 on Actions ✅

`ubuntu-24.04`, `timeout-minutes: 360`. Steps:
1. `actions/checkout@v4` with `submodules: recursive` (kernel = crdroid
   `android_kernel_xiaomi_sm8635` branch `16.0`, display-drivers = peridot-u-oss).
2. `actions/cache@v4` for `out/` (incremental, keyed on `hashFiles('display-drivers/**')`)
   and for the clang tarball (`path: toolchain/clang-r530567`, `key: clang-r530567`);
   fetch step: `curl -L` the googlesource `+archive` tarball → untar.
3. `hendrikmuhs/ccache-action@v1.2`, `max-size: 4G`, `CCACHE_DIR` inside workspace.
4. `build.sh` (all logic here):
   - merges `gki_defconfig` + `vendor/pineapple_GKI.config` +
     `vendor/peridot_GKI.config` via `scripts/kconfig/merge_config.sh -m -r`;
   - `scripts/config -d WERROR -d DEBUG_INFO_BTF` (speed),
     patches `certs/extract-cert.c` (USE_PKCS11_ENGINE) and `Makefile.extrawarn`;
   - `make O=out ARCH=arm64 vmlinux` → `modules` → `Image dtbs`;
   - out-of-tree companion modules (mm-drivers sync/hw_fence/ext_display, mmrm,
     securemsm) built with `KBUILD_EXTRA_SYMBOLS`, then msm_drm with doze patch;
   - `gzip -9 -k Image` → `Image.gz`;
   - collects `.ko` → `vendor_dlkm/lib/modules/<kver>/`, writes `modules.load`,
     `modules.dep`, packs `vendor_dlkm.img` with `mkfs.erofs -z lz4 -b 4096`.
5. Uploads: `kernel-Image` (Image + Image.gz), `msm_drm-patched`, `vendor_dlkm`,
   `mmrm-log` (debug); prints `vermagic=` of built module into the job summary.
6. Release: `gh release create "build-$(date +%Y%m%d-%H%M)" … --clobber` with
   `GH_TOKEN`.

Its README documents the KMI rule we need: *"Device kernel `6.1.138-android14-11` is
KMI `android14-6.1` … on a `version magic`/`modversions` mismatch during `modprobe`,
pin the kernel submodule to a commit matching your device's exact KMI."*

### 3.2 `Lu5ck/android_custom_kernel_builds` — peridot LOS kernel ✅

Workflow: `ubuntu-latest`; step "Setup zram as swap" (swapon zram 24 GB lz4,
`vm.swappiness=180`, `vm.page-cluster=0`); checkout builder + kernel source
(`lineage-23.2-ksun_susfs` ref); build inside an ArchLinux docker (`actions/peridot`
composite with `entrypoint.sh`):
```bash
KERNEL_DEFCONFIG="gki_defconfig vendor/pineapple_GKI.config vendor/peridot_GKI.config"
KERNEL_CMDLINE="ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=out LLVM=1 LLVM_IAS=1"
make $KERNEL_CMDLINE $KERNEL_DEFCONFIG
make $KERNEL_CMDLINE -j$(nproc --all)
cp out/arch/arm64/boot/Image ../builder/actions/peridot/AnyKernel3
```
Toolchain: `ZyCromerZ/Clang` release tarball (Clang 20 git). Config toggles done by
`set_config_flag CONFIG_KSU… peridot_GKI.config` (sed/append into the fragment —
the same pattern our multiboot fragment will use). Artifact: the AnyKernel3 tree;
upload `peridot_ksun_susfs_<datetime>`.

### 3.3 `Xiaomichael/OnePlus-Actions` "Build Kernel Only" (GKI 5.10–6.6, sm8635 listed) ✅

The most feature-complete GKI-only pipeline found (580★). Pattern:
1. `workflow_dispatch` inputs: SoC (`sm8635` is an option!), device, KMI
   (`android14`), kernel version (`6.1`), feature toggles.
2. `easimon/maximize-build-space` for old kernels; apt packages via
   `awalsh128/cache-apt-pkgs-action`; `CCACHE_*` env exactly as §2.4; ccache dir
   per-device under `$HOME`, cached with `actions/cache@v5`.
3. `repo init -u …/kernel_manifest.git -m <device>.xml && repo sync` — full
   kernel_platform workspace (common + msm-kernel + prebuilts).
4. Config mutations *by appending to `gki_defconfig`*:
   `CONFIG_BBG=y`, `CONFIG_LSM=…`, `CONFIG_TMPFS_XATTR=y`, `CONFIG_TMPFS_POSIX_ACL=y`,
   ipset/netfilter sets, `CONFIG_CC_OPTIMIZE_FOR_PERFORMANCE=y`,
   `CONFIG_HEADERS_INSTALL=n`; and — important for us —
   ```bash
   rm kernel_platform/common/android/abi_gki_protected_exports_* || echo "No protected exports!"
   sed -i 's/ -dirty//g' kernel_platform/common/scripts/setlocalversion
   sed -i '$s|echo "$res"|echo "-android14-oki-xiaoxiaow"|' …/setlocalversion
   ```
   i.e. they **remove the protected-exports list** and **rewrite setlocalversion** to
   control `UTS_RELEASE` exactly.
5. Build (6.1 path, verbatim):
   ```bash
   export PATH="…/prebuilts/clang/host/linux-x86/clang-r487747c/bin:$PATH"
   cd kernel_platform/common
   make -j$(nproc --all) LLVM=1 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
        CC="ccache clang" RUSTC=../../prebuilts/rust/linux-x86/1.73.0b/bin/rustc \
        PAHOLE=../../prebuilts/kernel-build-tools/linux-x86/bin/pahole \
        LD=ld.lld HOSTLD=ld.lld O=out KCFLAGS+=-O2 gki_defconfig all
   ```
   (yes — **the same clang-r487747c as Xiaomi's peridot build.config**).
6. Packaging: clone AnyKernel3, `cp …/out/arch/arm64/boot/Image AnyKernel3/Image`,
   zip → `upload-artifact`; extra artifacts: built `zram.ko`/`crypto_zstdn.ko` when
   the zram patch is enabled (modules *do* get shipped for patched kernels).

### 3.4 `xiaoleGun/KernelSU_Action` (template, 718★) ✅

Structure worth copying regardless of KernelSU:
- Inputs via `config.env` profiles + `workflow_dispatch` overrides; `concurrency:
  group: build-kernel-${{ github.ref }}-${{ inputs.config }}, cancel-in-progress: true`.
- `timeout-minutes: 120`.
- **Least-privilege split**: `build` job runs with `permissions: contents: read`;
  a separate `release` job gated on an input gets `contents: write` and does
  `softprops/action-gh-release@<pinned-sha>` with a markdown changelog table built
  from job `outputs`.
- `jlumbroso/free-disk-space` (dotnet+haskell+large-packages+docker-images,
  `android: false`, `swap-storage: false`) then a self-written 8 GB `/mnt/swapfile`.
- Lean apt list (bc bison build-essential ccache cpio curl device-tree-compiler flex
  git libelf-dev libssl-dev lz4 make python3 rsync unzip zip zstd libncurses-dev).
- `hendrikmuhs/ccache-action@v1.2` (2G), artifacts: kernel image, **AnyKernel3**,
  `dtbo.img`, `boot.img` (each with `if-no-files-found: error`).

### 3.5 `dabao1955/kernel_build_action` (composite action, 180★) ✅

The "classic" single-step experience: `uses: dabao1955/kernel_build_action@main` with
inputs `kernel-url/branch/config/arch/aosp-clang/aosp-clang-version/android-version/
anykernel3/release`. Internally: clone kernel, download AOSP clang from googlesource,
`make defconfig`, `LLVM=1`, KernelSU integration, AnyKernel3 zip, GH release. Useful
as a fallback skeleton but its inputs don't model GKI multi-fragment configs well;
we'll write our own steps instead (and it pins old clang by default).

### 3.6 Manual `mkbootimg` example ✅

`jiganomegsdfdf/ubuntu-oneplus-aston` (`aston-kernel_build.sh`):
```bash
cat Image sm8550-oneplus-aston.dtb > Image_w_dtb && gzip Image_w_dtb
mkbootimg --header_version 4 --base 0x0 --os_version 15.0.0 --os_patch_level 2025-02 \
          --kernel Image_w_dtb.gz -o boot16G.img
```
Note: that's a **non-GKI mainline** kernel (DTB concatenated into the boot kernel).
Peridot GKI must **not** do this — its DTB lives in `vendor_boot` (§4).

### 3.7 Common pattern summary across all inspected pipelines

```
checkout (repo + submodules / repo sync)
→ free disk space (+ swap or zram)
→ apt install lean build deps
→ download & cache clang prebuilt
→ ccache restore (actions/cache or ccache-action)
→ (patch source / append config fragments)
→ make defconfig merge → make -j$(nproc) LLVM=1 (vmlinux|all|Image dtbs)
→ gzip Image → Image.gz
→ package: AnyKernel3 zip (+ optional boot.img via mkbootimg, dtbo.img, modules)
→ upload-artifact@v4 (versioned names)
→ (optional) release job: download-artifact → zip → softprops/action-gh-release
```

---

## 4. boot.img / init_boot / vendor_boot / dtbo assembly for peridot ✅

### 4.1 What the device expects (verified from Xiaomi build.configs + real image bytes)

| Property | Value | Source |
|---|---|---|
| Boot header version | **4** | `build.config.msm.pineapple`: `BOOT_IMAGE_HEADER_VERSION=4` |
| Page size | **4096** | same (`PAGE_SIZE=4096`) + verified in the shipped `dtbo.img` header (page_size field = `0x00001000` big-endian) |
| Base address | **0x80000000** | `build.config.msm.pineapple`: `BASE_ADDRESS=0x80000000` |
| Kernel binary | `Image` (arm64, uncompressed, MZ-magic EFI-stub header) | `build.config.msm.common`: `KERNEL_BINARY=Image`; PixelOS peridot prebuilt `images/kernel` starts `4d 5a` (verified) |
| Boot kernel payload | `Image.gz` (gzip) | `build.config.gki.aarch64` `FILES="… Image.lz4 Image.gz"`, GKI boot sizes `BUILD_GKI_BOOT_IMG_GZ_SIZE=47185920` / `…LZ4_SIZE=53477376`; hoshikv does `gzip -9 Image` |
| DTB in boot? | **No** — GKI: DTB goes to `vendor_boot` (`--dtb` lands there in AOSP `build_boot_images`); boot.img has kernel only | AOSP `build/kernel/build_utils.sh` (mirror `aosp-riscv/kernel-build@riscv64-common-android-mainline`, fetched): `MKBOOTIMG_ARGS+=("--dtb" dtb.img)` + `--vendor_boot vendor_boot.img` |
| DTBO | separate `dtbo.img`, `mkdtboimg create --page_size=4096`, **2 entries** ("Qualcomm Technologies, Inc. **Cliffs** SoC" + "Cliffs 7 SoC" — SM8635 = Cliffs) | PixelOS-Devices-old `device_xiaomi_peridot-kernel@fifteen` `images/dtbo.img` header parsed: magic `d7b7ab1e`, entry_count=2, page_size=0x1000; file padded to 20 MiB |
| Generic ramdisk | in **init_boot** (not in boot) | `build.config.msm.pineapple`: `BUILD_INIT_BOOT_IMG=1`; AnyKernel3 peridot scripts branch on `init_boot_a` presence |
| vendor_dlkm / system_dlkm | built separately (erofs), **not** touched by boot flash | `BUILD_VENDOR_DLKM=1`, `PREPARE_SYSTEM_DLKM=1` |

DTB inventory on device (from the same PixelOS repo): peridot ships the "Cliffs" /
"Cliffs 7" SoC dtbs in `vendor_boot` and the two-overlay `dtbo.img`.

### 4.2 mkbootimg invocation (what AOSP does for header v3/v4 GKI)

From AOSP `build_utils.sh::build_boot_images` (fetched mirror, §4.1) — the canonical
arg set, matching `BOOT_IMAGE_HEADER_VERSION=4`/`PAGE_SIZE=4096`/`BASE_ADDRESS=…`:
```bash
mkbootimg \
  --header_version 4 \
  --base 0x80000000 \
  --pagesize 4096 \
  --kernel out/arch/arm64/boot/Image.gz \
  --cmdline "$(KERNEL_CMDLINE)" \
  [--os_version 0 --os_patch_level 0]        # defaults; stock value NEEDS VERIFICATION
  --vendor_boot out/vendor_boot.img \
  --vendor_cmdline "$(KERNEL_VENDOR_CMDLINE … bootconfig)" \
  --vendor_ramdisk ramdisk.lz4 \
  --dtb out/dtb.img                          # DTB is packed into vendor_boot, NOT boot
  -o out/boot.img
```
`mkbootimg.py` (AOSP `platform/system/tools/mkbootimg@android14-release`, fetched):
supports `--header_version > 3`, `--vendor_boot`, `--vendor_ramdisk`,
`--vendor_ramdisk_fragment`, `--pagesize` (legacy alias of `--page_size`),
`os_version<<11|os_patch_level` field.

**Practical recommendation:** don't hand-assemble `boot.img` from scratch (you'd have
to reproduce cmdline/os_version/vendor_boot bytes). Use **AnyKernel3 on-device
repack** (§4.3) as the primary flashable, and additionally emit a "best-effort"
`boot.img` artifact (mkbootimg with the stock-matching args above, or unpack stock
boot.img and swap the kernel with `magiskboot`) for direct-fastboot users.

### 4.3 AnyKernel3 for peridot (verified from two shipping custom kernels)

Both `GuidixX/kernel_xiaomi_sm8635@16.2` (`anykernel/anykernel.sh`, Chidori-Kernel)
and `Krtonia/AnyKernel3@rd-peridot` (Radioactive-Kernel) use the identical peridot
stanzas:
```bash
block=boot
is_slot_device=auto
ramdisk_compression=auto
patch_vbmeta_flag=auto
no_magisk_check=1

if [ -L "/dev/block/bootdevice/by-name/init_boot_a" -o -L "/dev/block/by-name/init_boot_a" ]; then
    split_boot   # devices with init_boot ramdisk: no ramdisk unpack
    flash_boot
else
    dump_boot
    write_boot
fi
```
Key points:
- `split_boot`/`flash_boot` = magiskboot repack of the existing boot image with only
  the kernel replaced — original cmdline/header fields/os_version preserved,
  kernel compression re-applied automatically (`ramdisk_compression=auto`).
- `patch_vbmeta_flag=auto` → AnyKernel3 patches the vbmeta flags
  (`AVB_VBMETA_IMAGE_FLAGS_VERIFICATION_DISABLED | HASHTREE_DISABLED`) so the
  custom kernel passes AVB on an unlocked bootloader — no manual vbmeta flash needed.
- Ship the **uncompressed `Image`** in the AK3 zip (both kernels do); magiskboot
  compresses to whatever the device's boot.img uses. Optionally also ship `Image.gz`.

### 4.4 vbmeta / avb signing paths

1. **AnyKernel3 `patch_vbmeta_flag=auto`** (default choice; works on unlocked
   devices, verified used by peridot kernels above).
2. Manual fastboot: `fastboot flash vbmeta vbmeta.img --disable-verification
   --disable-verity` (standard GKI-device practice; vbmeta.img = stock one or
   `avbtool make_vbmeta_image`).
3. **avbroot** (`chenxiaolong/avbroot`, 950★): re-signs boot/vendor_boot/init_boot and
   OTA images with your own key while keeping AVB *enabled* — the "proper" path if we
   ever want verified boot intact. Overkill for multiboot testing.

---

## 5. KMI / ABI stability when modifying the kernel config

### 5.1 The KMI concept & peridot's KMI

- KMI = "Kernel Module Interface": the set of exported symbols + in-kernel type
  layouts that vendor (`vendor_dlkm`/`system_dlkm`) modules are built against.
  GKI guarantees: a stock vendor module binary keeps loading on any kernel image
  with the **same KMI**.
- peridot KMI = **android14-6.1, generation 11** (`KMI_GENERATION=11` in
  `build.config.common` ✅) → release string `6.1.x-android14-11` ✅ (verified in the
  shipped module vermagic below). ACK's android14-6.1 symbol lists for peridot live
  in the kernel tree under `android/`: `abi_gki_aarch64_qcom`,
  `abi_gki_aarch64_xiaomi`, `abi_gki_aarch64` (base), and
  `abi_gki_protected_exports_aarch64` (all verified present; `abi_gki_aarch64_xiaomi`
  has 350 lines, qcom list has thousands).
- Xiaomi builds the device GKI variant with `KMI_SYMBOL_LIST_STRICT_MODE=1`,
  `KMI_ENFORCED=1`, and **`GKI_TRIM_NONLISTED_KMI=1`** — i.e. symbols not on the qcom
  list are *removed* from the image. That trimming is implemented in
  `build/build.sh`/Kleaf tooling, **not** in plain `make`; a standalone `make LLVM=1`
  build (our pipeline) does no trimming and no `abidiff` — which is exactly why the
  *module-load-time* consequences (§5.3) are the ones we must manage ourselves.

### 5.2 The hard facts about stock peridot modules (verified from a real module) ✅

Downloaded `modules/system_dlkm/6.1.75-android14-11-g16c5f6cd5e9b-ab12268515/kernel/
drivers/block/zram/zram.ko` from `PixelOS-Devices-old/device_xiaomi_peridot-kernel`
(stock-derived prebuilt modules) and read it:
```
vermagic=6.1.75-android14-11-g16c5f6cd5e9b-ab12268515 SMP preempt mod_unload modversions aarch64
```
Consequences for any rebuilt peridot kernel:
1. **`UTS_RELEASE` must match byte-for-byte** for stock modules to load (vermagic is
   compared verbatim). That includes `6.1.75` (LTS sublevel of that firmware),
   `-android14-11` (KMI), `-g16c5f6cd5e9b` (git hash Xiaomi stamped) and
   `-ab12268515` (Xiaomi build id). Since we can't reproduce Xiaomi's git hash from
   our tree, set it explicitly:
   ```bash
   # LOCALVERSION_AUTO off; append the full suffix:
   scripts/config -d LOCALVERSION_AUTO -e LOCALVERSION \
     --set-str LOCALVERSION "-android14-11-g16c5f6cd5e9b-ab12268515"
   ```
   (or rewrite `scripts/setlocalversion` like §3.3 does). The exact string differs
   **per firmware build** — read it on-device from `/proc/version` or
   `strings <any stock .ko> | grep vermagic` before flashing.
   **NEEDS VERIFICATION**: confirm which firmware the multiboot project targets and
   freeze its vermagic; also confirm `CONFIG_LOCALVERSION`/`SCMVERSION` interplay
   (gki_defconfig has `CONFIG_MODULE_SCMVERSION=y`, which stamps SCM info into
   modules — may need `-d MODULE_SCMVERSION` to avoid `-dirty`/hash drift).
2. **`modversions`** (CONFIG_MODVERSIONS) means symbol **CRCs** are also checked per
   symbol. CRCs are computed from source-level type signatures (genksyms); they stay
   stable when (a) the source that defines the exported symbols is unchanged,
   (b) headers/structs they reference are unchanged, and (c) the compiler
   preprocessor output is equivalent. Practical rules:
   - Keep the kernel source = Xiaomi `peridot-u-oss` (or the crdroid/Lineage tree that
     matches your firmware's sublevel). Changing sublevels (6.1.57 MiCode tag vs
     6.1.75 firmware!) changes CRCs wholesale — **NEEDS VERIFICATION** which base the
     project uses; if CRC equality with stock modules is required, the build tree must
     be at the *same* sublevel as the firmware, with LTS patches applied.
   - Newer clang *can* perturb CRCs (preprocessor/builtin differences); Xiaomi's
     pinned `r487747c` minimizes drift (§2.3).
   - `CONFIG_KEXEC=y` does not change any module-visible type layout or exported
     symbol → CRCs of existing symbols stay identical. This is the *safe* class of
     config change.
3. In-tree peridot modules (`zram`, `zsmalloc`, `mi_fp`, `fsa4480`, haptics, mtd…) are
   `=m` and live in the device's `system_dlkm`/vendor partitions. **Flashing boot.img
   does not replace them** — our kernel must load the *stock* copies, so config
   changes that alter their source-visible types (e.g. zram config knobs!) will break
   them even with matching vermagic.

### 5.3 MODULE_SIG / MODULE_SIG_PROTECT (verified Kconfig text)

`gki_defconfig`: `CONFIG_MODULE_SIG=y`, `CONFIG_MODULE_SIG_PROTECT=y` (no
`MODULE_SIG_FORCE`). Semantics, quoted from the ACK Kconfig
(`config MODULE_SIG_PROTECT`, "Android GKI module protection"):
> *Allows other modules to load if they don't violate the access to Android GKI
> protected symbols and do not export the symbols already exported by the Android GKI
> modules. Loading will fail and return **-EACCES** if symbol access conditions are
> not met.*

Practical meaning for us: stock vendor modules load as long as they don't touch
*protected* symbols in a violating way; unsigned/foreign-signed modules taint instead
of being rejected (no FORCE). The community workaround used by GKI custom-kernel
pipelines (Xiaomichael, §3.3, verified) is simply to delete
`android/abi_gki_protected_exports_*` before building, which relaxes protection to
plain module signing. We should **not** need this for a KEXEC-only change, but it is
the documented escape hatch if `-EACCES` shows up in dmesg.

### 5.4 Enabling KEXEC for multiboot — what breaks, what doesn't

- ACK android14-6.1 `gki_defconfig` contains **no `CONFIG_KEXEC`** (verified against
  android.googlesource.com). So multiboot adds a config fragment, e.g.:
  ```
  CONFIG_KEXEC=y
  # optional, only if crash-dump-style kexec is desired:
  # CONFIG_KEXEC_FILE=y / CONFIG_CRASH_DUMP=y
  ```
- What KEXEC *does*: adds `kernel/kexec*.o`, `arch/arm64/kernel/machine_kexec.o`,
  relocation code, and a small set of internal symbols; **does not add EXPORT_SYMBOLs**
  used by vendor modules and does not change struct layouts that vendor modules
  consume. Hence: no CRC change, no KMI break, stock modules keep loading. This
  matches the empirical fact that peridot custom kernels (Chidori, Radioactive,
  crdroid's own tree) ship modified configs without rebuilding vendor modules.
- What *would* break the KMI (avoid): enabling/disabling configs that change shared
  types or add/remove exports — e.g. `CONFIG_RANDOMIZE_BASE` toggles, struct-random
  options (`RANDSTRUCT`), debug options that alter `struct page`-adjacent layouts,
  adding new `EXPORT_SYMBOL`s, or re-enabling GMI-trimmed symbols. Rule of thumb:
  **config delta = additive feature flags only; keep everything else identical to the
  stock `.config`** (produce it once with `make gki_defconfig
  vendor/pineapple_GKI.config vendor/peridot_GKI.config` + `olddefconfig`, check the
  diff, and gate CI on that diff being only the multiboot lines).
- Detection if something *did* break: module load fails with
  `version magic mismatch` (vermagic) or `Unknown symbol`/`-EACCES` (CRC/protected
  exports). Test path: after build, `modprobe` each stock system_dlkm module against
  our `vmlinux`'s `Module.symvers`… practically, `modinfo -F vermagic` on stock .ko vs
  `make kernelrelease`, plus a CRC spot check with
  `scripts/module.lds`/`modpost` output or boot-testing.

### 5.5 GKI kernel vs "device kernel" difference

peridot's product kernel is a **GKI image built by Xiaomi with device trimmings**
(in-tree modules list from `peridot.bzl`, qcom symbol list, `__ANDROID_COMMON_KERNEL__`
kcflag, `CONFIG_MODULE_SCMVERSION`, zram/zsmalloc as modules). It is *not* a pure ACK
GKI: a pure-ACK build would miss the mi/qcom in-tree module set and device cmdline.
Our pipeline therefore builds **from Xiaomi's tree** with Xiaomi's defconfig merge
(§2.2), not from ACK `android14-6.1` directly.

---

## 6. Recommended pipeline for THIS project

### 6.1 Job design (pseudocode; the real YAML is written by the main agent)

```yaml
name: build-peridot-kernel
on:
  workflow_dispatch:
    inputs: { variant: {…}, publish: {…} }
  push: { branches: [main] }

concurrency:
  group: build-${{ github.ref }}
  cancel-in-progress: true

permissions: { contents: read }          # build job is read-only (xiaoleGun pattern)

jobs:
  build:
    runs-on: ubuntu-24.04                # pin; public repo => 4 vCPU / 16 GB
    timeout-minutes: 180
    strategy:
      fail-fast: false
      matrix:
        variant: [stock, multiboot]      # stock = exact Xiaomi defconfig merge;
                                         # multiboot = stock + multiboot.fragment
    steps:
      - uses: actions/checkout@v4        # our repo: kernel tree (submodule or
        with: { submodules: recursive }  # fetched tarball of peridot-u-oss), patches,
                                         # defconfig fragments, packaging assets
      - name: Free disk space
        uses: jlumbroso/free-disk-space@v1.3.1
        with: { tool-cache: false, android: true, dotnet: true, haskell: true,
                large-packages: true, docker-images: true, swap-storage: false }
      - name: Add swap (on /mnt, keeps the image's 4G swap too)
        run: |
          sudo fallocate -l 12G /mnt/swapfile && sudo chmod 600 /mnt/swapfile
          sudo mkswap /mnt/swapfile && sudo swapon /mnt/swapfile && free -h
      - name: Install build deps
        run: |
          sudo apt-get update -qq
          sudo apt-get install -y --no-install-recommends \
            bc bison build-essential ccache cpio curl device-tree-compiler flex \
            git libelf-dev libssl-dev lz4 make python3 rsync unzip zip zstd \
            libncurses-dev
      - name: Cache clang
        uses: actions/cache@v4
        with:
          path: toolchain/clang-r487747c
          key: clang-r487747c
      - name: Fetch clang if missed
        if: steps.clang.outputs.cache-hit != 'true'
        run: |
          mkdir -p toolchain && cd toolchain
          # pinned AOSP clang (Xiaomi CLANG_VERSION) — see §2.3 for URL options
          curl -fL <CLANG_R487747C_URL> | tar xzf - -C clang-r487747c
      - name: ccache
        uses: hendrikmuhs/ccache-action@v1.2
        with: { key: peridot-${{ matrix.variant }}, max-size: 4G }
      - name: Configure
        run: |
          export PATH="$PWD/toolchain/clang-r487747c/bin:$PATH"
          make O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 \
            gki_defconfig vendor/pineapple_GKI.config vendor/peridot_GKI.config \
            ${RUNNER_OS:+} fragments/${{ matrix.variant }}.fragment
          make O=out ARCH=arm64 olddefconfig
          # guardrail: stock variant must produce a config identical to Xiaomi's
          # (diff against a committed reference .config)
      - name: Build
        run: |
          export PATH="/usr/lib/ccache:$PWD/toolchain/clang-r487747c/bin:$PATH"
          export CCACHE_COMPILERCHECK="%compiler% -dumpmachine; %compiler% -dumpversion"
          export CCACHE_NOHASHDIR=true CCACHE_HARDLINK=true
          export KBUILD_BUILD_USER=peridot-multiboot KBUILD_BUILD_HOST=gha
          make O=out -j"$(nproc)" ARCH=arm64 LLVM=1 LLVM_IAS=1 \
               CC="ccache clang" LD=ld.lld all dtbs
          gzip -9 -k out/arch/arm64/boot/Image        # -> Image.gz
          make -s O=out ARCH=arm64 kernelrelease > kernelrelease.txt
      - name: Package boot.img (best-effort direct-flash image)
        run: |
          # unpack stock boot.img (committed, from the target firmware), swap kernel,
          # repack — preserves cmdline/os_version/flags exactly (magiskboot path);
          # OR mkbootimg --header_version 4 --pagesize 4096 --base 0x80000000
          #    --kernel out/arch/arm64/boot/Image.gz -o out/boot.img
      - name: Assemble AnyKernel3 zip
        run: |
          git clone --depth=1 https://github.com/osm0sis/AnyKernel3 ak3
          rm -rf ak3/.git
          # apply our peridot anykernel.sh (block=boot, is_slot_device=auto,
          # patch_vbmeta_flag=auto, split_boot/flash_boot — see §4.3)
          cp out/arch/arm64/boot/Image ak3/Image
          zip -qr9 peridot-multiboot-${{ matrix.variant }}.zip ak3/*
      - name: Upload artifacts
        uses: actions/upload-artifact@v4
        with:
          name: peridot-${{ matrix.variant }}-${{ github.sha }}
          path: |
            out/arch/arm64/boot/Image
            out/arch/arm64/boot/Image.gz
            out/boot.img
            peridot-multiboot-*.zip
            out/.config
            kernelrelease.txt
          if-no-files-found: error

  release:
    needs: build
    if: inputs.publish || github.ref == 'refs/heads/main'
    runs-on: ubuntu-24.04
    permissions: { contents: write }     # only this job can write
    steps:
      - uses: actions/download-artifact@v4
        with: { path: artifacts, merge-multiple: false }
      - uses: softprops/action-gh-release@v2   # pin SHA like xiaoleGun does
        with:
          tag_name: peridot-${{ github.run_number }}
          files: artifacts/**/*
          body_path: changelog.md           # generated from git log + vermagic
```

### 6.2 Caching

| Cache | Tool | Key | Size |
|---|---|---|---|
| clang tarball | `actions/cache@v4` | `clang-r487747c` | ~1.5 GB (saves 1–3 min DL + flake risk) |
| ccache | `hendrikmuhs/ccache-action@v1.2` or `actions/cache` on `~/.ccache` | `peridot-<variant>-<week>` + `restore-keys` | 4 GB; warm builds drop to ~15–25 min |
| `out/` | skip | — | keep builds hermetic; ccache covers the speed win (hoshikv caches `out/` but it bloats cache quota and can mask config errors) |

ccache env (from §2.4): `CCACHE_COMPILERCHECK`, `CCACHE_NOHASHDIR`, `CCACHE_HARDLINK`.

### 6.3 Artifacts & naming

- `peridot-<variant>-<shortsha>` artifact containing: `Image`, `Image.gz`,
  `boot.img`, `AnyKernel3 zip`, `out/.config`, `kernelrelease.txt` (the vermagic
  source of truth!), and the built in-tree `.ko` files (zram etc. — for CRC/vermagic
  inspection, not for flashing).
- Release assets: same, zipped per variant; body = git log + config delta +
  `make kernelrelease` + a big **"check your firmware vermagic"** warning (§5.2).

### 6.4 Matrix & variants

- `stock`: verbatim Xiaomi defconfig merge — CI guardrail that the pipeline itself is
  faithful (and produces a boot.img with known-good KMI).
- `multiboot`: stock + `fragments/multiboot.fragment` containing (at minimum)
  `CONFIG_KEXEC=y` (+ whatever the multiboot design needs; keep the diff ≤ ~10 lines).
  The Configure step diffs `.config` vs the stock variant and fails loudly on
  unexpected changes (protects the KMI rules of §5.4).

### 6.5 Expected duration (estimate; **NEEDS VERIFICATION**)

- Cold (no ccache), 4 vCPU: **~60–90 min** for `all dtbs` on GKI 6.1 with KASAN+debug
  info compiled in (GKI defconfig); KASAN/BTF trimming would cut this but is left as
  an explicit, reviewed choice (§5.2 CRC note suggests keeping debug-only options
  unchanged).
- Warm ccache (same toolchain, small patch): **~15–25 min**.
- Xlipped links are the memory peak: keep the 12 GB `/mnt` swapfile; 16 GB RAM is
  fine, 8 GB (private-repo runners) is risky without the swap.
- Observed reference points: xiaoleGun template default `timeout-minutes: 120`;
  hoshikv sets 360 (conservative, includes module building + erofs packaging).

---

## 7. Checklist distilled from the real pipelines

- [x] Pin runner image and action SHAs (`xiaoleGun` pins `jlumbroso` and `softprops` by SHA).
- [x] `concurrency` group + `cancel-in-progress` (xiaoleGun).
- [x] `timeout-minutes` on every job.
- [x] Build job read-only permissions; release job write-only-on-demand.
- [x] `if-no-files-found: error` on artifact upload.
- [x] Ship **uncompressed `Image`** in AnyKernel3; ship `Image.gz` + `boot.img` for fastboot users.
- [x] Record `make kernelrelease` + built-module `vermagic` in the job summary (hoshikv).
- [x] Keep the stock-variant build as a KMI canary.
- [x] Never concatenate dtb into the boot kernel (GKI! dtb → vendor_boot).
- [x] Do not regenerate `dtbo.img`/`vendor_boot`/`init_boot` — reuse stock partitions.

## 8. References (all fetched during this session)

Xiaomi / upstream:
- https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/peridot-u-oss (commit `062233df…`):
  `build.config.msm.peridot`, `build.config.msm.pineapple`, `build.config.common`,
  `build.config.constants`, `build.config.gki.aarch64`, `peridot.bzl`,
  `xiaomi_sm8650_common.bzl`, `arch/arm64/configs/gki_defconfig`,
  `arch/arm64/configs/vendor/{pineapple,peridot}_GKI.config`, `android/abi_gki_aarch64_qcom`,
  `android/abi_gki_aarch64_xiaomi`, `android/abi_gki_protected_exports_aarch64`
- https://github.com/crdroidandroid/android_kernel_xiaomi_sm8635 (branch `16.0`):
  `build.config.msm.common`, `build.config.msm.gki`
- ACK android14-6.1 gki_defconfig: https://android.googlesource.com/kernel/common/+/refs/heads/android14-6.1/arch/arm64/configs/gki_defconfig
- AOSP build_utils.sh (boot image assembly): https://github.com/aosp-riscv/kernel-build/blob/riscv64-common-android-mainline/build_utils.sh
- mkbootimg.py: https://android.googlesource.com/platform/system/tools/mkbootimg/+/refs/heads/android14-release/mkbootimg.py
- clang prebuilt: https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/refs/heads/main/clang-r530567.tar.gz (HTTP 200 verified)

Runner:
- https://docs.github.com/en/actions/reference/runners/github-hosted-runners
- https://github.com/actions/runner-images (+ `images/ubuntu/templates/*.pkr.hcl`,
  `images/ubuntu/scripts/build/configure-environment.sh`)
- https://github.com/actions/runner-images/issues/14254 (ubuntu-22.04 deprecation)
- https://github.com/jlumbroso/free-disk-space

Workflows / templates:
- https://github.com/hoshikv/peridot-kernel-build (`build.sh`, `.github/workflows/build.yml`)
- https://github.com/Lu5ck/android_custom_kernel_builds (`.github/workflows/peridot.yml`, `actions/peridot/entrypoint.sh`)
- https://github.com/Xiaomichael/OnePlus-Actions (`.github/workflows/Build Kernel Only.yml`)
- https://github.com/xiaoleGun/KernelSU_Action (`.github/workflows/build-kernel.yml`)
- https://github.com/dabao1955/kernel_build_action
- https://github.com/jiganomegsdfdf/ubuntu-oneplus-aston (`aston-kernel_build.sh`)
- https://github.com/amackdev/peridot-kernel-manifest (`default.xml`)

Packaging / device specifics:
- https://github.com/PixelOS-Devices-old/device_xiaomi_peridot-kernel (branch `fifteen`:
  `images/kernel`, `images/dtbo.img`, `modules/system_dlkm/…/zram.ko` — vermagic + dtbo header verified)
- https://github.com/osm0sis/AnyKernel3
- https://github.com/GuidixX/kernel_xiaomi_sm8635 (branch `16.2`, `anykernel/anykernel.sh`)
- https://github.com/Krtonia/AnyKernel3 (branch `rd-peridot`, `anykernel.sh`)
- https://github.com/chenxiaolong/avbroot
- https://github.com/ZyCromerZ/Clang (community clang tarballs)

## 9. NEEDS VERIFICATION (open items for the main agent)

1. **Stock boot.img kernel compression** (gzip vs lz4) for the exact peridot firmware
   targeted — unpack the target boot.img once (`magiskboot unpack`) and record it.
   AnyKernel3 handles it automatically, but the direct-fastboot `boot.img` artifact
   should match.
2. **Stock boot.img `os_version`/`os_patch_level`/header flags** — read from the
   unpacked boot header before deciding the `mkbootimg` args for the direct-flash
   artifact (default 0/0 may be fine; unverified).
3. **Exact stock `UTS_RELEASE`/vermagic for the target firmware** (verified example:
   `6.1.75-android14-11-g16c5f6cd5e9b-ab12268515` from a PixelOS module dump) — must be
   read from the actual device build the multiboot kernel ships for.
4. **Kernel source sublevel fidelity**: MiCode `peridot-u-oss` Makefile says
   `6.1.57`; the verified firmware module vermagic is `6.1.75`; crdroid's tree claims
   `6.1.138`. Decide which tree + LTS state reproduces the target firmware's
   MODVERSIONS CRCs; if exact CRC equality matters, the build tree must match the
   firmware's LTS level.
5. **Canonical URL for clang-r487747c** (not on googlesource `main`; 400 verified).
   Candidates: the ci.android.com submitted-build that produced it, a
   prebuilts-clang mirror repo, or accepting newer clang after a CRC-compat check.
6. **ci.android.com branch names for kernel toolchains** (my `aosp-kernel-build-tools`
   guess 404'd); the `builds/submitted/<id>` pattern works (200 verified for 9978441)
   but needs the right build id.
7. **ubuntu-24.04 runtime deps for r487747c** (libtinfo5-class gaps) — one dry-run
   build settles it.
8. **Whether MODULE_SIG_PROTECT/-EACCES actually triggers for stock peridot modules
   on our rebuilt kernel** — dmesg check on first device boot; escape hatch
   (remove `abi_gki_protected_exports_*`, as Xiaomichael's pipeline does) documented.
9. **Cold/warm build minutes on 4 vCPU with KASAN enabled** — real numbers after the
   first CI run; estimates in §6.5 are extrapolated from template timeouts.
10. **CONFIG_KEXEC arm64 dependencies on 6.1** (e.g. whether `CONFIG_KEXEC_FILE`,
    `CONFIG_CRASH_DUMP`, or MTE-adjacent deps are pulled in, and whether
    `CONFIG_PKVM_MODULE_PATH`/protected exports interact with kexec-loading of an
    unsigned secondary kernel) — validate in the first multiboot variant build.
