# Survey: Existing Multi-Boot / Boot-Menu Approaches on Android, and What Is Viable on POCO F6 (peridot)

*Research doc R2 — survey of real projects, evaluated against the peridot constraint set:
GKI 2.0 kernel (android14-6.1), dynamic partitions (super), A/B slots, AVB, unlocked-but-signed
bootloader (XBL/ABL cannot be replaced). Every factual claim carries a URL or a
"verified in source" note. Unverified items are flagged **NEEDS VERIFICATION**.*

---

## TL;DR

1. **No existing multi-boot project works on peridot unmodified.** DualBootPatcher is
   archived and died precisely on the two features peridot has (system-as-root + dynamic
   partitions). MultiROM's kexec-hardboot is a 32-bit-arm kernel patch from 2014. DSU and
   A/B slots give partial multi-OS today but are too limited for our goal.
2. **The strongest reusable pieces are chenxiaolong's:** DualBootPatcher's *init-replacement
   + bind-mount* model (still conceptually valid) and avbroot's *boot-image repack + AVB
   re-sign* pipeline (directly reusable to inject our menu into `init_boot` and keep it
   alive across OTAs).
3. **Stock peridot kernel has NO kexec.** Verified: `android14-6.1` `gki_defconfig` contains
   no `CONFIG_KEXEC` line, and the peridot vendor fragment doesn't add one. A rebuilt GKI
   kernel with additive config (`CONFIG_KEXEC=y`, `CONFIG_PROC_KCORE=y`) is mandatory for
   the Linux path — which our repo already plans.
4. **Cold kexec on Qualcomm is historically unreliable** (hypervisor/rpmh/smem handoff);
   a real 2020s attempt on a Qualcomm phone crashed. kexec on peridot must be treated as
   experimental; `fastboot boot` of a mainline image is the robust fallback.
5. **Mainline Linux for SM8635 does not exist yet.** Mainline has SM8550 and SM8650 DTSs
   (including commercial devices: Samsung Q5Q, Sony Xperia, Ayaneo Pocket S2) but zero
   SM8635. postmarketOS lists peridot as "Mainline WIP (just started)". A peridot DTS will
   be a derivative of sm8550/sm8650 work.
6. **The pre-init display problem is real:** GKI has `CONFIG_DRM=y` but no fbdev, and the
   peridot display driver (`msm_drm`) is a loadable module that ships in `vendor_dlkm` —
   it is NOT loaded when our menu would run. Menu design must handle a headless early
   stage (module pre-load, late menu hook, or vibration/fallback UI).

---

## 1. chenxiaolong's projects (the closest relatives)

### 1.1 DualBootPatcher (the "MultiBoot" of the prompt; real repo name is DualBootPatcher)

- Repo: https://github.com/chenxiaolong/DualBootPatcher — **archived**, last push
  2023-04-20 (verified via GitHub API `repos/chenxiaolong/DualBootPatcher`).
  The prompt's "chenxiaolong/MultiBoot" does not exist; a repo search of all
  chenxiaolong repositories confirms `DualBootPatcher` + `DualBootZips` are the only
  multi-boot ones (verified via GitHub API `users/chenxiaolong/repos`).
- Status banner (README): development stopped because of Android P (mandatory
  system-as-root) and Android Q (dynamic partitions / dm-linear). Quote: *"With devices
  that ship with Android 9.0+, the system-as-root partition layout is mandatory … DBP
  relies on being able to modify the ramdisk … With the Android Q preview builds … a
  single GPT partition is split up and is mapped to device-mapper block devices … DBP
  would have to implement something equivalent [to liblp] to be able to mount the Android
  partitions."* https://github.com/chenxiaolong/DualBootPatcher#readme
  A `10.0.0-staging` branch exists with partial work.

**How it patched the boot ramdisk (verified in source, `mbtool/src/boot/init.cpp` and
`mbtool/include/util/multiboot.h`):**

- Patched ramdisk contains: `/init` → symlink to `/mbtool`; real Android init moved to
  `/init.orig`; a `/romid` file (one-line ROM ID) and `/device.json` are added.
- Kernel runs `mbtool` as PID 1. mbtool: mounts devtmpfs/proc/sys, runs its own ueventd
  thread to create device nodes, mounts the fstab, launches the boot UI, patches SELinux
  policy, **bind-mounts the chosen ROM's directories**, then does
  `rename("/init.orig", "/init"); execlp("/init", ...)` and execs real Android init.
  Source: https://github.com/chenxiaolong/DualBootPatcher/blob/master/mbtool/src/boot/init.cpp
  (functions `launch_boot_menu()`, `mount_rom()`, and the final `execlp("/init")`).
- ROM ID is baked into the ramdisk per flash; switching ROMs = flashing that ROM's patched
  `boot.img` (README, quoted above). No interactive picker *between boots* unless the boot
  UI runs (see below) — the /romid in the image is the primary selector.

**Boot menu UI (mbbootui) — how it draws + reads keys pre-init:**

- `mbbootui` is a stripped-down port of **TWRP's graphics stack** ("mbbootui is based on
  the graphical code from TWRP … the entire graphics system is configured at runtime").
  Source: https://github.com/chenxiaolong/DualBootPatcher/blob/master/mbbootui/README.md
  (minuitwrp/ tree: framebuffer + evdev input + themes).
- It is *not* part of the boot ramdisk; mbtool's `launch_boot_menu()` reads a signed zip
  from `/raw/cache/multiboot/bootui.zip`, verifies it, extracts to `/mbbootui`, then
  executes `/mbbootui/exec` **after** /data has been mounted by mbtool — i.e. it runs
  pre-real-init but post-data-mount, with display already available because those devices
  had fbdev built into the kernel. Paths verified in
  `mbtool/include/util/multiboot.h` (`BOOT_UI_ZIP_PATH`, `BOOT_UI_PATH`,
  `BOOT_UI_SKIP_PATH`) and `mbtool/src/boot/init.cpp`.
- Keys: TWRP-style evdev handling; the UI waits on `KEY_POWER` etc.
  (e.g. `while(wait_for_key() != KEY_POWER)` in the error path of
  https://github.com/Tasssadar/multirom/blob/master/multirom_ui.c — same design family).

**ROM layout on userdata (verified in `mbtool/src/util/roms.cpp` and
`mbtool/src/boot/mount_fstab.cpp`):**

- Everything lives under the shared `/data` (internal storage):
  - `/data/media/0/MultiBoot/<romid>/` — per-ROM metadata: `boot.img`, `config.json`,
    `thumbnail.webp` (`Rom::boot_image_path()`, `Rom::config_path()` in
    https://github.com/chenxiaolong/DualBootPatcher/blob/master/mbtool/src/util/roms.cpp).
  - Two mount strategies per ROM (same file, `roms.cpp` lines ~124–177):
    1. **Directory-ROM**: `…/multiboot/<romid>/system`, `/cache`, `/data` directories on
       the real `/data` filesystem, bind-mounted by mbtool over the mounts.
    2. **Image-ROM**: `…/multiboot/<romid>/system.img` loop-mounted to
       `/raw/images/<romid>/system` (`mount_all_system_images()` in
       https://github.com/chenxiaolong/DualBootPatcher/blob/master/mbtool/src/boot/mount_fstab.cpp),
       with per-ROM cache/data directories bind-mounted from `/data`.
  - mbtool mounts the *real* partitions at `/raw/system`, `/raw/cache`, `/raw/data`
    (mount_fstab.cpp top defines), then overlays the chosen ROM.
- Extras that made it usable: per-App data sharing / individual app sharing
  (`RomConfig`, app sync daemons — `mbtool/src/boot/appsync.cpp`), SELinux policy patching
  per boot (`sepolpatch.cpp`), shared vs private `/data` (README of GUI).

**Why it can't run on peridot as-is:**
- Needs fbdev + a data-mounted pre-init stage (peridot: data is FBE-encrypted, mounts late;
  display driver is a module).
- Assumes non-dynamic partitions or the ability to loop-mount system over a classic block
  device; peridot's system/vendor/product live inside `super` via dm-linear
  (https://source.android.com/docs/core/architecture/partitions — partition docs; DBP README
  quote above).
- Assumes ROM switch = re-flash boot image, i.e. no AVB, no A/B slot metadata, no
  `init_boot` (peridot: `init_boot` holds the generic ramdisk with `/init`).
- Kernel-level assumptions: the pre-init exec of `/init` symlink-to-binary works, but on
  Android 14 first-stage init reads fstab/bootconfig itself; DBP predates `init_boot`
  (introduced Android 13) entirely.

### 1.2 avbroot (actively maintained; our key tooling donor)

- Repo: https://github.com/chenxiaolong/avbroot (not archived; active in 2026 — verified
  via GitHub API). Tool to modify A/B OTA images reproducibly and re-sign AVB + OTA with
  custom keys: https://github.com/chenxiaolong/avbroot#readme
- What it patches (README "Patches" section):
  - `boot` or **`init_boot`** (depending on device) for root (Magisk / KernelSU) — proof
    that patching `init_boot` ramdisks on modern GKI devices is a solved problem;
  - `boot`/`recovery`/`vendor_boot` OTA-cert replacement;
  - `system` OTA-cert replacement; then re-signs all vbmeta images and the OTA payload/zip.
- **AVB re-signing flow** (README "Generating keys", "Initial setup"): generate AVB key →
  `avbroot key encode-avb` → flash `fastboot flash avb_custom_key avb_pkmd.bin` → relock
  bootloader with your own root of trust. Device compatibility list for
  `avb_custom_key`: https://github.com/chenxiaolong/avbroot/issues/299 — "Google is the only
  OEM to officially document that avb_custom_key is supported". Xiaomi: one user reports
  Xiaomi 14 accepts `avb_custom_key` (issue #299 comments). Peridot: **NEEDS VERIFICATION**.
- **How the ramdisk injection actually works** (verified in source
  https://github.com/chenxiaolong/avbroot/blob/main/avbroot/src/patch/boot.rs):
  `load_ramdisk()` / `save_ramdisk()` decompress the boot-image ramdisk cpio, mutate
  entries (Magisk injection: `MagiskRootPatcher`, including Magisk-preinit device
  handling), re-compress, repack the boot image
  (`avbroot/src/format/bootimage.rs`, `format/cpio.rs`), then re-sign AVB hash tree +
  vbmeta (`format/avb.rs`). This is exactly the machinery we need to swap `/init` for our
  menu binary + `/init.real` inside `init_boot`, with valid signatures so the device
  could even stay **relocked** (if peridot accepts `avb_custom_key`).
- Magisk-style patching alternative: our menu injection can follow either avbroot's
  own cpio editing, or Magisk's `magiskboot` repack
  (https://github.com/topjohnwu/Magisk — `native/src/boot`). avbroot additionally supports
  `--prepatched` images so CI can inject without avbroot doing Magisk logic.
- Bootconfig: avbroot does not need to touch bootconfig for root; but `mkbootimg.py`
  supports `--vendor_bootconfig` to append a bootconfig section to `vendor_boot` v4
  (verified in https://android.googlesource.com/platform/system/tools/mkbootimg/+/refs/heads/main/mkbootimg.py,
  `args.vendor_bootconfig`). AOSP documents bootconfig as the modern replacement for
  `androidboot.*` cmdline propagation: https://source.android.com/docs/core/architecture/bootloader/bootconfig
  (page 404s for raw curl — **NEEDS VERIFICATION for exact URL**; the mkbootimg flag is the
  hard evidence).

---

## 2. Legacy approaches and why they died

### 2.1 MultiROM (Tasssadar) + kexec-hardboot

- Repo: https://github.com/Tasssadar/multirom (last meaningful push 2019; README lists
  Nexus 7 grouper/flo, mako, hammerhead — all 2012–2013 devices).
- Architecture: patched kernel per device + `trampoline` (pre-init stub), a framebuffer
  boot UI (verified: `multirom_ui.c` uses `lib/framebuffer.h`, `lib/input.h`, waits on
  `KEY_POWER`), ROMs kept in `/sdcard/multirom/roms/...` and booted either directly
  (Android-kernel reuse) or via **kexec-hardboot** into another Linux kernel
  (https://github.com/Tasssadar/multirom/blob/master/kexec.c — comments show the
  `kexec --load-hardboot ./zImage --command-line="$(cat /proc/cmdline)" --mem-min=...`
  invocation and memory-placement caveats).
- **kexec-hardboot kernel patch**: adds a "hardboot" kexec variant where the *outgoing*
  kernel stays resident in RAM to re-initialize hardware (display clocks, PMIC rails,
  regulators) before jumping. Verified in real device kernels, all 32-bit ARM:
  - `arch/arm/mach-msm/restart.c` with `KEXEC_HARDBOOT`:
    https://github.com/faux123/Nexus_5 (hammerhead, mach-msm),
    https://github.com/CyanogenMod/android_kernel_samsung_jf
  - `arch/arm/kernel/machine_kexec.c` + `relocate_kernel.S` hardboot variants:
    https://github.com/faux123/Nexus_5
  - Redmi 1S kexec-hardboot kernel: https://github.com/multirom-armani/android_kernel_xiaomi_armani
  - **No arm64 / GKI-era kexec-hardboot patch exists** in any of these trees; the concept
    predates arm64 Android. (Searched GitHub code for `KEXEC_HARDBOOT` — all hits are
    `arch/arm`, mach-msm/mach-tegra.)
- Death causes (same families as DBP, verified from README dates + supported devices):
  requires heavily patched per-device kernels (impossible under GKI/KMI), per-device
  trampolines/ROM layouts, non-dynamic partitions, and it predates AVB enforcement,
  A/B and system-as-root. Last supported devices shipped Android 4.x–6.x.
- Project page (archived): https://github.com/Tasssadar/MultiRom

### 2.2 Dual Boot Patcher — death recap (see 1.1)

Summarizing the author's own README: system-as-root removed the boot-ramdisk hook surface;
bootloaders ignoring ramdisk/cmdline (Pixels needed kernel patches to ignore
`skip_initramfs`; some Samsungs always ignore ramdisk) removed the ROM-ID channel; dynamic
partitions required reimplementing liblp in userspace. All three are exactly peridot's
constraints → **DBP is a design reference, not a runnable codebase, for us.**

### 2.3 Other historical notes

- `phhusson/phh-loader` — "kexec-based Android bootloader" (used kexec to chain-load
  Android from Android on early devices; RPi-focused rewrite later):
  https://github.com/phhusson/phh-loader
- `zagto/dtbootmenu` — kexec-hardboot boot menu for ASUS TF300T (Tegra 3, 32-bit):
  https://github.com/zagto/dtbootmenu
- `mkasick/external_kexec-tools` — old Android port of kexec-tools:
  https://github.com/mkasick/external_kexec-tools
- Lesson from all of them: **boot menus that survived did their selection in the boot
  image ramdisk with a kernel they controlled**, and all needed per-device kernel work.

---

## 3. kexec on Android / GKI

### 3.1 Does the GKI kernel have KEXEC? (verified)

- Fetched `arch/arm64/configs/gki_defconfig` from the `android14-6.1` branch of
  https://android.googlesource.com/kernel/common/ (+refs/heads/android14-6.1):
  **no `CONFIG_KEXEC` line at all** (neither `=y` nor commented-out), i.e. kexec syscall
  is disabled in the default GKI build. Other verified relevant lines:
  `CONFIG_HIBERNATION=y`, `CONFIG_BLK_DEV_LOOP=y`, `CONFIG_DRM=y` (no `CONFIG_FB`),
  `CONFIG_INPUT_EVDEV=y`, `CONFIG_KALLSYMS_ALL=y`, `CONFIG_EXT4/F2FS/EROFS_FS=y`,
  `CONFIG_BLK_DEV_DM=y`, no `CONFIG_PROC_KCORE`.
- Xiaomi peridot kernel tree https://github.com/MiCode/Xiaomi_Kernel_OpenSource branch
  `peridot-u-oss` (verified to exist; tree at commit 062233df735d): builds with
  `arch/arm64/configs/gki_defconfig` + fragment `arch/arm64/configs/vendor/peridot_GKI.config`
  (file fetched; it contains only Xiaomi device bits — haptics, fingerprint, zram… — and
  no KEXEC). GitHub code search for `CONFIG_KEXEC` in the MiCode repo returns nothing.
- Kconfig constraints (verified in v6.1 source,
  https://github.com/torvalds/linux/blob/v6.1/arch/arm64/Kconfig and
  `init/Kconfig`): arm64 `config KEXEC` **depends on `PM_SLEEP_SMP`** and selects
  `KEXEC_CORE`; `config KEXEC_FILE` is the fd-based syscall; `config KEXEC_SIG`
  optionally enforces signatures for `kexec_file_load()` (v6.1 arm64 Kconfig lines
  1400–1410). GKI already has suspend infra (`HIBERNATION=y`), so `KEXEC=y` should be an
  additive, KMI-safe change — final KMI impact check in doc 04.
- Consequence: **stock peridot kernel cannot kexec; our rebuilt kernel must add
  `CONFIG_KEXEC=y` (and ideally `CONFIG_PROC_KCORE=y`, `CONFIG_KEXEC_FILE=y`)**.

### 3.2 kexec -l vs kexec-hardboot

- `kexec -l` + `kexec -e`: the running kernel shuts down devices itself, then jumps.
  Works when hardware can be re-driven cold by the new kernel (PCs, mainline qcom boards).
- **kexec-hardboot** (MultiROM patch, 32-bit era): on phones, full cold re-init of
  regulators/clocks/display from a fresh kernel was unreliable, so the patch kept the
  outgoing kernel resident to re-setup hardware before the jump (patch refs in 2.1;
  MultiROM kexec.c comments on `--load-hardboot` and memory placement).
- On arm64 Android there is **no mainline equivalent of hardboot**; mainline arm64 kexec
  is expected to work only where the outgoing kernel can quiesce hardware properly — on
  Qualcomm SoCs the firmware handoff state (XBL-loaded hypervisor, `smem`, rpmh sleep
  votes, subsys restart) is not designed for a second cold kernel bring-up.
- Real-world data point (the only modern one found): evdenis/kexec issue #2 — a user
  enabled `CONFIG_KEXEC=y` + `PROC_KCORE` + `KALLSYMS` on a Qualcomm phone kernel
  (`kexec -l Image --append=<stock cmdline> --initrd=...` succeeded, then `kexec -e`
  black-screened and the phone crashed/rebooted):
  https://github.com/evdenis/kexec/issues/2 — consistent with the handoff problem.
  **Cold kexec on SM8635: experimental, must be validated. NEEDS VERIFICATION.**
- Qualcomm-specific security context: XBL/ABL boot chain and firmware (hyp, smem, rpmh,
  tee) run *below* Linux; kexec bypasses them entirely — this is why the "reboot to
  bootloader, then `fastboot boot`" path (which re-runs XBL/ABL properly) is the reliable
  alternative for loading an arbitrary kernel. ABL *does* accept images via fastboot on
  unlocked devices by design (Android bootloader docs:
  https://source.android.com/docs/core/architecture/bootloader — see also A/B docs
  https://source.android.com/docs/core/ota/ab). Whether peridot's ABL accepts
  `fastboot boot <img>`: **NEEDS VERIFICATION** (Xiaomi has a history of broken/restricted
  `fastboot boot` on some devices — e.g. MTK bug report
  https://github.com/MiCode/Xiaomi_Kernel_OpenSource/issues/2356; TWRP `fastboot boot`
  issues on Xiaomi https://github.com/TeamWin/Team-Win-Recovery-Project/issues/1203).

### 3.3 kexec tooling for Android

- **evdenis/kexec** — Magisk module shipping static `kexec` binaries (arm64/arm/x86_64/x86),
  actively maintained (pushed 2026): https://github.com/evdenis/kexec — this saves us
  building kexec-tools for aarch64-Android; upstream is
  https://git.kernel.org/pub/scm/utils/kernel/kexec/kexec-tools.git/
- It requires a kernel with `CONFIG_KEXEC` (author's own words in issue #2, above) — the
  module does not patch kernels.
- Our own use: the boot-menu binary can call the `kexec_file_load()` / `kexec_load()`
  syscalls directly (no userspace dependency) from the GKI kernel we build; kexec tools
  are needed only for interactive debugging from Android.

---

## 4. Booting non-Android OSes on SM8635/SM8650-class hardware

### 4.1 Mainline Linux status (verified against torvalds tree)

- SM8550 (kernel 6.x mainline): `sm8550-mtp.dtb`, `sm8550-qrd.dtb`, `sm8550-hdk.dtb`,
  plus real products **`sm8550-samsung-q5q.dtb`** (Samsung Galaxy S23 family SoC) and
  **`sm8550-sony-xperia-yodo-pdx234.dtb`** (Xperia 10 V). Verified in
  https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/Makefile
- SM8650: `sm8650-mtp/qrd/hdk` DTBs + **`sm8650-ayaneo-pocket-s2.dtb`** (commercial
  Ayaneo handheld) and full platform plumbing (`gcc-sm8650.c`, `dpu_10_0_sm8650.h`,
  interconnect, pinctrl…). Same Makefile.
- SM8635 (Snapdragon 8s Gen 3): **nothing in mainline**. GitHub code search for `sm8635`
  returns only Android-kernel device trees (e.g.
  https://github.com/kmiit/android_kernel_xiaomi_sm8650-devicetrees `qcom/peridot-sm8635.dtsi`
  — Xiaomi's own downstream DTS naming proves SM8635 is treated as an SM8650-family
  ("pineapple") sibling with its own overlays) and trivia files. → A peridot mainline DTS
  must be written, starting from `sm8550-mtp.dts` / `sm8650-mtp.dts` and the downstream
  peridot DTSI; expected to be *close to* sm8650 (same gen) with differences in PMIC,
  display panel (peridot uses a specific DSI panel), and missing periphery
  (e.g. no UFS-less wifi combos differ). **Effort: weeks-months; NEEDS VERIFICATION on
  exact delta.**
- Robotics/dev-boards in mainline prove the SoC-gen bring-up path:
  `qcs8550-rb5gen2.dtb` (Thundercomm RB5 Gen 2, QCS8550 = SM8550-class) — same Makefile.

### 4.2 postmarketOS

- pmaports device tree: no `device-*-sm8550/-sm8650/-sm8635` package exists (verified by
  listing `device/community` and `device/testing` via GitLab API
  https://gitlab.postmarketos.org/api/v4/projects/postmarketOS%2Fpmaports/repository/tree?path=device).
  Qualcomm support is concentrated on msm8916/8953/8974, sdm660/845, sc7180/7280, sm6350,
  qcs6490 (Fairphone-class) etc.
- **peridot-specific pmOS page exists** (via Wayback snapshot of
  https://wiki.postmarketos.org/wiki/Xiaomi_POCO_F6_/_Redmi_Turbo_3_(xiaomi-peridot)):
  status **"not booting / Mainline WIP (just started)"** — a port attempt exists but has
  produced no working build yet.
- pmOS's own boot flow (`postmarketos-mkinitfs`, now a Go rewrite at
  https://gitlab.postmarketos.org/postmarketOS/postmarketos-mkinitfs) provides an
  initramfs with device-specific hooks and a rescue/debug interface, **not** a general
  multi-OS menu. The claim "multi-profile boot via pmbootstrap initramfs menu" from the
  task brief: **NEEDS VERIFICATION — no evidence found**; treat pmOS as *target distro*,
  not *boot-menu mechanism*.

### 4.3 Halium / Ubuntu Touch (Android-kernel reuse)

- Halium = shared hardware-adaptation layer to run GNU/Linux userspaces (Ubuntu Touch etc.)
  **on the vendor Android kernel + Android userspace HALs via libhybris**:
  https://halium.org/ ("Linux kernel (source provided by device vendor); Android services
  required to talk with hardware; Libhybris").
- For peridot this is the *fastest* non-Android path because it reuses the stock GKI+Xiaomi
  kernel exactly as Android uses it (display/touch/wifi/modem come up like Android).
- No peridot/POCO F6 port exists in the UBports installer configs (verified: full listing
  of https://github.com/ubports/installer-configs `v2/devices/` — 77 devices, none
  peridot; the `turbo.yml` there is the Meizu Pro 5). Latest Xiaomi Halium ports are
  old (beryllium = POCO F1). → A peridot Halium port is a full adaptation project
  (rootfs image, libhybris for android14, RIL, bt…), significant but well-documented work:
  https://docs.halium.org/
- This matches our architecture doc's "linux-halium" menu entry type: boot the Android
  kernel, but exec a Linux init on a loop-mounted rootfs image instead of Android.

---

## 5. Boot-menu implementation techniques pre-init (what actually works in 2024+)

### 5.1 Injection points on a GKI A/B device (peridot)

| Point | Contents | How to inject | Reboot-survival |
|---|---|---|---|
| `init_boot` | generic ramdisk, contains `/init` binary (Android 13+) | repack cpio: `/init` = our menu, `/init.real` = real init (avbroot/magiskboot-style; avbroot does this class of patch for Magisk: https://github.com/chenxiaolong/avbroot/blob/main/avbroot/src/patch/boot.rs) | lost on OTA; re-apply via patched-OTA flow |
| `vendor_boot` | vendor ramdisk (fstab, .rc, first-stage modules), DTB, **bootconfig section** | repack vendor ramdisk; add `multiboot.rc`; `--vendor_bootconfig` for extra `androidboot.*` (https://android.googlesource.com/platform/system/tools/mkbootimg/+/refs/heads/main/mkbootimg.py) | lost on OTA |
| `boot` | GKI kernel | our rebuilt kernel with `CONFIG_KEXEC` (+ optionally our menu piggybacked — Magisk precedent patches boot.img the same way) | lost on OTA |
| `vbmeta*` | hashes | must be re-signed after any of the above (avbroot does this automatically) | — |

`init_boot` exists since Android 13 for GKI devices
(https://source.android.com/docs/core/architecture/partitions — partitions page covers
`init_boot`; peridot ships it, verified in project doc 01).

### 5.2 Drawing to the screen (the hard part)

- GKI 6.1 `gki_defconfig` has `CONFIG_DRM=y` but **no fbdev** (no `CONFIG_FB`,
  no `CONFIG_FB_SIMPLE`) — verified in the fetched defconfig. `/dev/graphics/fb0`
  likely does not exist on peridot. MultiROM/DBP/TWRP-era framebuffer code assumed fbdev.
- The peridot display pipeline is **`msm_drm.ko` as a loadable module** in `vendor_dlkm`
  (verified: peridot kernel builders build msm_drm.ko separately —
  https://github.com/hoshikv/peridot-kernel-build "GKI 6.1 peridot msm_drm.ko build";
  module target `obj-$(CONFIG_DRM_MSM) += msm.o` in
  https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/peridot-u-oss/drivers/gpu/drm/msm/Makefile).
  At menu time (init_boot ramdisk, before vendor init), **no display driver is bound**,
  so a DRM dumb-buffer menu would need the module loaded first.
- Practical options, in order of preference:
  1. **Pre-load modules from the menu binary**: run `modprobe`-equivalent (manual
     `finit_module()` on the vendor_boot-ramdisk copies of msm_drm + panel module) —
     vendor_boot's `lib/modules` carries some first-stage modules
     (verify which; **NEEDS VERIFICATION** whether msm_drm.ko is among first-stage
     modules on peridot or only in vendor_dlkm).
  2. **Run the menu later**: hook `on init`/`on fs` in an injected .rc so the menu runs
     after first-stage mounts have provided vendor modules (DBP-style: menu before
     real init's second stage, but post-modprobe). Costs: switching Android ROMs by
     bind-mount becomes second-stage only (see §6).
  3. **Headless menu**: selection purely via key-timing / vibration patterns / LED —
     last-resort fallback (no display dependency at all).
- ABL "continuous splash": Qualcomm ABL draws the boot logo and the display keeps that
  image until the kernel display driver takes over. Whether peridot's kernel/panel
  supports keeping ABL splash alive while our menu writes via the *event* path only
  (no display writes) — visually the user sees the splash while pressing keys — is a
  pragmatic hack. **NEEDS VERIFICATION.**

### 5.3 Reading keys

- `CONFIG_INPUT_EVDEV=y` in GKI (verified) → `/dev/input/event*` works pre-init once
  devtmpfs is mounted (menu mounts it itself, DBP/mbtool precedent).
- Keycodes (verified in
  https://github.com/torvalds/linux/blob/master/include/uapi/linux/input-event-codes.h):
  `KEY_VOLUMEDOWN=114`, `KEY_VOLUMEUP=115`, `KEY_POWER=116`. Volume keys generate
  press/release via the pmic gpio/keypad driver that Android uses for boot selection —
  on Xiaomi devices volume keys are read by ABL itself for menu entry (e.g. recovery),
  so they are wired to a pre-boot key path by design. Which evdev node carries them
  pre-init: **NEEDS VERIFICATION** (grep of input devices at menu time).
- Timeout: fixed-countdown default entry; DBP boot UI had the same "skip file" override
  pattern (`BOOT_UI_SKIP_PATH`), which we mirror via `/metadata/multiboot/active`.

### 5.4 Persisting the choice

- **`/metadata` partition**: unencrypted ext4/f2fs, mounted by first-stage init from
  vendor fstab; our menu can mount it read-write and store
  `/metadata/multiboot/active` (architecture doc §4). Also used by Magisk preinit logic
  conventions (avbroot README documents Magisk preinit partition dependency:
  https://github.com/chenxiaolong/avbroot#magisk-preinit-device).
- **`/misc` BCB (bootloader control block)**: canonical AOSP mechanism for Linux↔bootloader
  communication — `bootloader_message` struct with `command` field "used by linux when it
  wants to reboot into recovery" (verified:
  https://android.googlesource.com/platform/bootable/recovery/+/refs/heads/main/bootloader_message/include/bootloader_message/bootloader_message.h).
  We can reuse BCB to implement `reboot-recovery` menu entries (write `boot-recovery`
  command + reboot) — safer than relying on Android init for the recovery entry.
- **bootconfig/cmdline**: `androidboot.slot_suffix` comes from ABL per active slot; the
  menu cannot override slot selection for the *running* boot (the images are already
  loaded), but `--vendor_bootconfig` can add extra androidboot flags
  (mkbootimg flag, verified above) — e.g. `androidboot.multiboot.active=rom2` to hand the
  choice to init without touching `/metadata`. Whether init honours bootconfig-provided
  slot_suffix (it does *not* pick the slot — that's ABL's job) — do not rely on it;
  use our own flag. **NEEDS VERIFICATION** for exact merge semantics of duplicate
  androidboot keys between cmdline and bootconfig
  (https://source.android.com/docs/core/architecture/bootloader/bootconfig).

### 5.5 Exec-switching after the menu

- **(a) Continue to Android, chosen ROM**: DBP model (§1.1) — but on peridot the
  "choose ROM" effect must happen via bind-mounts/images orchestrated by our injected
  .rc + helper (see §6), not by mbtool's first-stage mounts.
- **(b) kexec into mainline/Linux kernel**: menu binary calls `kexec_load()`
  (kernel + DTB + initramfs from /metadata or a small partition), then `reboot exec`.
  Requires our rebuilt kernel (§3.1). Handoff risk: §3.2 — experimental.
- **(c) fastboot boot fallback**: menu writes a marker and reboots to bootloader
  (`reboot bootloader`); ABL then loads a mainline image via `fastboot boot`. Robust
  because XBL/ABL re-run their own bring-up. **NEEDS VERIFICATION**: peridot `fastboot boot`
  support (Xiaomi historically flaky, §3.2 refs).
- **(d) reboot-recovery**: write BCB `boot-recovery` (§5.4) + reboot; ABL boots the
  recovery slot image. Canonical, zero risk.
- Watchdog hygiene: pet or disable `/dev/watchdog` before any long menu wait
  (GKI `CONFIG_WATCHDOG=y` verified; Qualcomm watchdog will reset an idle pre-init
  system — DBP had an "emergency reboot" path for exactly this class of failure,
  `mbtool/src/boot/init.cpp` `emergency_reboot()`).

---

## 6. ROM layout schemes for multiple ANDROID ROMs on one userdata

### 6.1 The classical scheme (DBP): directory-per-ROM on /data + bind mounts

Detailed in §1.1. Recap of what is *conceptually transferable* to peridot:

- Per-ROM directory on shared storage: `/data/multiboot/romN/{system.img, vendor.img,
  product.img, system_ext.img, data/}` (images preferred — DBP's `system_is_image` path
  — because Android 14 ROM contents are erofs; GKI has `CONFIG_EROFS_FS=y` + LZMA, and
  `CONFIG_BLK_DEV_LOOP=y`, verified in gki_defconfig → loop-mounting erofs images works).
- Selector file readable early (`/romid` in DBP = ramdisk-baked; ours = `/metadata/...`
  or bootconfig flag).
- A pre-init (or early-second-stage) helper performing mounts + SELinux patching + then
  exec of real init.

### 6.2 What is different on peridot (dynamic partitions, system-as-root, AVB, A/B)

- **system-as-root**: there is no classic `/system` block device; first-stage init
  mounts `system` from a dm-linear logical partition (inside `super`) *as `/`*.
  https://source.android.com/docs/core/architecture/partitions/system-as-root
  Bind-mounting a directory "over /system" is meaningless; you must either (i) overlay
  after the root is assembled (second-stage; Magisk does its systemless magic this way
  and even DBP carried a workaround for Magisk re-binding /system — verified in DBP
  `init.cpp` comment referencing Magisk PR 387) or (ii) provide alternate block devices
  in first stage.
- **dm-linear / dynamic partitions**: to mount ROM B's partitions *as real partitions*,
  first-stage init would need different `super` metadata — i.e. a second logical
  partition set. Options: shrink `userdata`, create `super_mb` with per-ROM logical
  partitions (architecture doc Scheme B; requires re-partitioning = highest risk,
  destroys nothing in super if done carefully but is v2-only), or stay with
  **loop-mounted image files on /data** (Scheme A) which sidesteps liblp entirely.
- **First-stage vs second-stage bind mounts**: first-stage init on Android 14 reads
  fstab from vendor_boot ramdisk and does dm-linear + `/` mount + FBE key derivation
  start; injecting *mounts* there means shipping our own fstab entries + a helper
  binary in vendor_boot (kernel `CONFIG_BLK_DEV_LOOP=y` is in GKI, verified).
  Second-stage mounts (via `on post-fs` in injected .rc) are simpler but later: some
  apex/vendor services snapshot state early; zygote isn't started yet, so it is still
  safe for system/vendor/product substitution in practice — this is exactly where
  Magisk mounts systemless overlays (https://github.com/topjohnwu/Magisk).
- **erofs images on loop**: fine read-only; but ROM data dirs need ext4/f2fs — use
  per-ROM `data.img` f2fs files or shared-data with FBE policy implications (below).
- **FBE / encryption**: `/data` is FBE-encrypted (peridot); per-ROM private data dirs
  need their own fscrypt policies; sharing app data across ROMs breaks policy and is a
  known pain point even in DBP (its "share data" was best-effort). Safest v1: separate
  `data.img` per ROM, no sharing.
- **vbmeta per-ROM**: AVB hash-tree descriptors for system/vendor live in `vbmeta` +
  partition footers. In orange (unlocked) state, verification is skipped, so per-ROM
  images without valid hashtrees boot fine unlocked. If we ever re-lock with
  `avb_custom_key` (§1.2), every ROM image must be signed per-ROM — a hard requirement
  that pushes toward avbroot-style signing for *every* secondary ROM.
  https://source.android.com/docs/core/architecture/kernel/generic-kernel-image (GKI doc),
  https://source.android.com/docs/core/architecture/bootloader (verified boot context).
- **apex**: active apexes are mounted from `/system` (or `/data/apex` for updates);
  loop-mounted per-ROM system brings its own apex set — fine — but a *shared*
  `/data/apex` (staging of updated apexes) belongs to the *host* ROM and can
  cross-contaminate; v1 should refuse shared-data layouts for ROMs whose apexes differ.
  (**NEEDS VERIFICATION** for exact mount namespace handling on Android 14.)

### 6.3 Schemes that already work today on peridot (baselines to beat)

- **A/B slots as poor-man's dual-boot**: each slot holds a complete OS; `fastboot set_active b`
  (or app) switches. Verified concept: A/B docs
  https://source.android.com/docs/core/ota/ab. Works today, zero custom code; limits: 2 OSes,
  both must fit partition sizes, no menu at boot (selection is a fastboot command), shared
  /data per... actually /data is per-device not per-slot → ROMs fight over data formats
  (fine for GSI-style tests, painful for daily dual-ROM). Super partitions are shared
  between slots (`system_a` vs `system_b` inside one super).
- **GSI via DSU (Dynamic System Updates)**: official mechanism to install & boot a
  different system image on a dynamic-partition device, "switch between the current system
  image and the GSI"; Android 11+ has a **DSU Loader** in developer settings
  (verified text from https://developer.android.com/topic/dsu). Requires Google/OEM-signed
  GSI per docs (in practice with unlocked BL, `adb shell gsi_tool`/webusb `fastboot flash
  dsu` flows vary). **NEEDS VERIFICATION** for DSU behaviour on HyperOS 14 (MIUI-specific
  restrictions on gsi_tool).
- **Custom recovery as de-facto boot menu** (OrangeFox/TWRP on `vendor_boot`/`recovery`
  for peridot — OrangeFox device tree exists:
  https://github.com/AzzyC/ofox_device_xiaomi_peridot; crDroid/LineageOS install guides
  for peridot flash recovery images — https://github.com/crdroidandroid/android_device_xiaomi_peridot).
  This is how peridot users "switch ROMs" today: recovery → format data → flash ROM zip.
  It's a boot menu of last resort (manual, destructive-ish), not multi-boot.
- **LineageOS officially supports peridot** (verified:
  https://github.com/LineageOS/lineage_wiki `_data/devices/peridot_variant1.yml` —
  kernel repo `android_kernel_xiaomi_sm8635`, version 6.1, branch 23.2) — meaning a
  second, non-HyperOS ROM with known-good flashing path exists.

---

## 7. Comparison matrix

Legend: invasiveness = how much signed/blessed content changes; "OTA" = survives stock OTA
without re-work; brick = risk of unrecoverable state (assumes unlocked BL + backups).

| Approach | Invasiveness | Kernel mods | Android+Android | Android+Linux | Survives OTA | Brick risk | Works on peridot? |
|---|---|---|---|---|---|---|---|
| MultiROM (kexec-hardboot, patched kernel + trampoline) | Very high (per-device kernel patch) | Yes — hardboot patch (arm32 only) | Yes | Yes | No (kernel re-patch each OTA) | Low (stock flash recovers) | **No** — arm32-only patch, no GKI port, dead upstream |
| DualBootPatcher (init-replace + bind mounts) | High (boot image + system bits + recovery) | No | Yes (Android ≤ ~9/10) | Partial (via kexec on patched kernels) | No | Low | **No as-is** — archived; needs rewrite for init_boot/super/FBE; UI needs display at early boot |
| Menu in `init_boot` ramdisk + per-ROM images on /data (our v1) | Medium (init_boot + vendor_boot + vbmeta only) | Optional (`CONFIG_KEXEC`) | Yes (with re-signing/OTA pipeline) | Halium-style yes; mainline via kexp/fastboot | With avbroot-style OTA repatch | Low (all partitions re-flashable; stock images restore) | **Yes** — designed for it |
| A/B slot dual-ROM | None | No | 2 OSes | No | Yes (per-slot) | Minimal | **Yes** (baseline) |
| DSU (GSI loader) | None (dynamic partitions by design) | No | GSI only | No | Yes (unless updated over) | Minimal | Yes-ish; **NEEDS VERIFICATION** on HyperOS |
| kexec from Android into mainline (GKI kernel w/ KEXEC) | Medium (rebuild GKI kernel) | Yes (additive) | n/a | Yes (if handoff works) | Kernel re-flash each OTA (we ship CI kernel) | Low | Experimental — handoff risk (§3.2) |
| `fastboot boot` mainline image | None | No (mainline kernel image) | n/a | Yes | Yes | Low | **NEEDS VERIFICATION** (ABL must support `fastboot boot`) |
| Second `super` / re-partition (`super_mb`) | Very high (GPT edit) | No | Yes | Yes | Risky (updater expects layout) | **High** (partition table surgery) | Deferred to v2 |

---

## 8. Recommended architecture for peridot (synthesis)

1. **Menu location**: static aarch64 binary as `/init` in a repacked `init_boot`
   (with `/init.real`), injected via an avbroot-style pipeline
   (cpio edit → repack → re-sign vbmeta). Precedent: DBP `/init`→`/mbtool` symlink
   scheme (§1.1) and avbroot's `init_boot` Magisk patching (§1.2).
2. **Fallback/persistence**: choice stored in `/metadata/multiboot/active`; BCB in
   `/misc` used for reboot-recovery; bootconfig flag as metadata-free channel.
3. **Display strategy** (in order): try loading msm_drm + panel modules from
   vendor_boot ramdisk copies → DRM dumb-buffer menu; if that fails, headless
   key-count menu with vibration feedback; ABL-splash retention hack as bonus
   (all NEEDS VERIFICATION, §5.2). Keys: Vol± = 115/114, Power = 116.
4. **Android+Android**: per-ROM image files on `/data` (erofs system images from ROM
   payloads, ext4/f2fs per-ROM data images), loop-mounted and bind-mounted early in
   second stage by our injected .rc + helper (Scheme A of architecture doc). The DBP
   code remains the reference for the *semantics* (mount targets, SELinux patching,
   per-ROM config), not for the code itself (its liblp/dm-linear assumptions
   (§6.2) don't apply to loop images).
5. **Android+Linux (fast)**: Halium-style rootfs image loop-booted on the stock peridot
   kernel — same boot-image machinery, menu entry swaps init; no kexec needed
   (§4.3). This is the lowest-risk "non-Android OS" deliverable.
6. **Android+Linux (mainline)**: rebuilt GKI kernel with additive
   `CONFIG_KEXEC=y`/`PROC_KCORE=y` (+ `KEXEC_FILE` if signature-free loading is kept
   without `KEXEC_SIG_FORCE`), menu calls `kexec_file_load()` with mainline
   kernel+peridot DTS+initramfs. Treat as experimental (§3.2); keep
   **fastboot boot** (§5.5c) and a PC-side script as the robust path while a mainline
   SM8635 DTS (from sm8550/sm8650, §4.1) matures in postmarketOS/mainline.
7. **OTA strategy**: integrate with an avbroot-based OTA patch step so
   boot/init_boot/vendor_boot multiboot bits are re-applied on every OTA
   (avbroot proves this class of pipeline works locked & unlocked, §1.2).

### Open risks (honest list)

- Display pre-module-load is unproven on peridot; headless UX may be the only v1 reality.
- `fastboot boot` on peridot ABL unverified; without it, mainline path loses its fallback.
- Cold kexec handoff on Qualcomm: one documented modern attempt failed; nothing shows it
  working on SM8x50-class Android kernels.
- Multi-Android bind-mount scheme interacts with FBE, apex, and GMS integrity checks;
  per-ROM GMS registration/device attestation may break in secondary ROMs.
- KMI: `CONFIG_KEXEC=y` is believed KMI-safe (additive) but must be checked against
  `abi_gki_aarch64` symbol lists (doc 04).
- Xiaomi has signalled tightening bootloader unlocking (avbroot issue #299 comment);
  long-term viability of re-flashing multiboot images depends on unlock remaining possible.

---

## Primary sources

| Claim | URL |
|---|---|
| DualBootPatcher repo, status, death reasons | https://github.com/chenxiaolong/DualBootPatcher |
| DBP init replacement (`/init`→`/mbtool`, `/init.orig`, mount_rom, boot UI launch) | https://github.com/chenxiaolong/DualBootPatcher/blob/master/mbtool/src/boot/init.cpp |
| DBP per-ROM layout & paths | https://github.com/chenxiaolong/DualBootPatcher/blob/master/mbtool/src/util/roms.cpp ; https://github.com/chenxiaolong/DualBootPatcher/blob/master/mbtool/src/util/multiboot.h |
| DBP image mounting / raw mount points | https://github.com/chenxiaolong/DualBootPatcher/blob/master/mbtool/src/boot/mount_fstab.cpp |
| mbbootui = TWRP graphics | https://github.com/chenxiaolong/DualBootPatcher/blob/master/mbbootui/README.md |
| avbroot repo & patch list | https://github.com/chenxiaolong/avbroot |
| avbroot boot/ramdisk patcher | https://github.com/chenxiaolong/avbroot/blob/main/avbroot/src/patch/boot.rs |
| avbroot Magisk preinit partition requirement | https://github.com/chenxiaolong/avbroot#magisk-preinit-device |
| avb_custom_key device list (Xiaomi note) | https://github.com/chenxiaolong/avbroot/issues/299 |
| MultiROM repo + kexec.c hardboot invocation | https://github.com/Tasssadar/multirom ; https://github.com/Tasssadar/multirom/blob/master/kexec.c |
| KEXEC_HARDBOOT arm32 kernel patches | https://github.com/faux123/Nexus_5 ; https://github.com/CyanogenMod/android_kernel_samsung_jf ; https://github.com/multirom-armani/android_kernel_xiaomi_armani |
| android14-6.1 gki_defconfig (no KEXEC) | https://android.googlesource.com/kernel/common/+/refs/heads/android14-6.1/arch/arm64/configs/gki_defconfig |
| arm64 KEXEC/KEXEC_FILE/KEXEC_SIG Kconfig (v6.1) | https://github.com/torvalds/linux/blob/v6.1/arch/arm64/Kconfig |
| peridot kernel tree + vendor config fragment | https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/peridot-u-oss ; .../arch/arm64/configs/vendor/peridot_GKI.config |
| msm_drm as module for peridot | https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/peridot-u-oss/drivers/gpu/drm/msm/Makefile ; https://github.com/hoshikv/peridot-kernel-build |
| kexec-tools Magisk module (arm64) | https://github.com/evdenis/kexec |
| Real Qualcomm kexec failure report | https://github.com/evdenis/kexec/issues/2 |
| Mainline qcom DTS list (sm8550/sm8650/products, no sm8635) | https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/Makefile |
| Downstream peridot SM8635 device tree naming | https://github.com/kmiit/android_kernel_xiaomi_sm8650-devicetrees (qcom/peridot-sm8635.dtsi) |
| postmarketOS pmaports (no sm8635/8550/8650 device) | https://gitlab.postmarketos.org/postmarketOS/pmaports |
| postmarketOS peridot page (Mainline WIP) | https://wiki.postmarketos.org/wiki/Xiaomi_POCO_F6_/_Redmi_Turbo_3_(xiaomi-peridot) |
| postmarketos-mkinitfs | https://gitlab.postmarketos.org/postmarketOS/postmarketos-mkinitfs |
| Halium definition | https://halium.org/ ; https://docs.halium.org/ |
| No peridot UBports port | https://github.com/ubports/installer-configs (v2/devices/) |
| Keycodes 114/115/116 | https://github.com/torvalds/linux/blob/master/include/uapi/linux/input-event-codes.h |
| mkbootimg --vendor_bootconfig | https://android.googlesource.com/platform/system/tools/mkbootimg/+/refs/heads/main/mkbootimg.py |
| system-as-root / partitions / A-B / GKI docs | https://source.android.com/docs/core/architecture/partitions/system-as-root ; https://source.android.com/docs/core/architecture/partitions ; https://source.android.com/docs/core/ota/ab ; https://source.android.com/docs/core/architecture/kernel/generic-kernel-image |
| DSU / DSU Loader | https://developer.android.com/topic/dsu |
| /misc BCB (bootloader_message) | https://android.googlesource.com/platform/bootable/recovery/+/refs/heads/main/bootloader_message/include/bootloader_message/bootloader_message.h |
| LineageOS supports peridot | https://github.com/LineageOS/lineage_wiki ( _data/devices/peridot_variant1.yml ) |
| OrangeFox recovery device tree for peridot | https://github.com/AzzyC/ofox_device_xiaomi_peridot |
| crDroid peridot device tree | https://github.com/crdroidandroid/android_device_xiaomi_peridot |
| Xiaomi fastboot boot broken on MTK (context) | https://github.com/MiCode/Xiaomi_Kernel_OpenSource/issues/2356 |

## NEEDS VERIFICATION

1. **`fastboot boot` on peridot ABL** — decisive for the mainline fallback path (§3.2, §5.5).
2. **`avb_custom_key` on peridot** (Xiaomi 14 reportedly works) — enables locked-bootloader
   multiboot; not required for v1 (§1.2).
3. **Display bring-up pre-init**: is `msm_drm.ko` (+panel backlight module) available in
   vendor_boot first-stage `lib/modules`, or only in vendor_dlkm? Can our menu
   `finit_module()` it cleanly before DRM dumb-buffer use? (§5.2) — cross-check doc 04.
4. **ABL splash retention** ("continuous splash") on peridot — could allow a visually
   static menu screen without full display stack (§5.2).
5. **bootconfig vs cmdline androidboot merge semantics** (duplicate keys, slot_suffix
   handling) — affects our metadata-free selection channel (§5.4).
6. **Cold kexec on SM8635** — only a failure report on an older low-end Qualcomm exists;
   nothing proves or disproves it for SM8635 (§3.2).
7. **postmarketOS initramfs "boot menu"** mentioned in the task brief — no evidence found
   of a multi-OS menu in postmarketos-mkinitfs; pmOS is treated as target distro only (§4.2).
8. **Exact SM8635↔SM8650/SM8550 hardware deltas** for the future mainline DTS (PMIC, panel,
   USB/DP mux, thermal) (§4.1).
9. **DSU on HyperOS 14** (gsi_tool availability, vendor restrictions) (§6.3).
10. **KMI impact of CONFIG_KEXEC=y** — confirm via `abi_gki_aarch64` symbol-list diff in
    doc 04 (§3.1, §8 risks).
