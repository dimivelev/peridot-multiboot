# Implementation Roadmap

## Phase 0 — research & CI (current)
- [x] Kernel tree cloned & analyzed (docs/04) — relocated to /mnt/storagebox (local disk)
- [x] Architecture designed (docs/02)
- [x] GitHub Actions build pipelines for kernel + bootmenu (workflows/)
- [x] Subagent research: device/bootchain, multiboot survey, CI patterns (docs/01,03,05)
- [x] bootmenu builds in CI ✅ (static aarch64 ELF, 39 KB, zig/musl, DRM backend compiles)
- [x] CI tree repair: MiCode peridot-u-oss is incomplete (hwid driver lives in a separate
      Xiaomi repo) — repair-kernel-tree.sh fetches it + stubs any other missing Kconfig sources
- [ ] First successful CI kernel build (v3 pipeline running: clang r530567 + pineapple/
      peridot/multiboot fragment merge) — iterate on runner until Image.gz pops out

## Phase 1 — read-only validation on device
- [x] `boot-menu-test.img`: fastboot-bootable menu trial — **no extraction, no flashing**
      (`fastboot boot boot-menu-test.img`; CI artifact)
- [ ] Extract + inspect stock `init_boot`/`vendor_boot` from the device's current ROM
      (`scripts/extract-rom-images.sh` on the HyperOS fastboot tgz) — needed only for the
      permanent install (proprietary, build-pinned images we minimally repack)
- [ ] Dump device tree at runtime: confirm ABL simple-framebuffer node (docs/04 §7)
- [ ] Enumerate evdev nodes: `adb shell getevent -pl` → volume/power keycodes
- [ ] Static-test bootmenu binary on-device via `adb push` + chroot (no flashing yet)

## Phase 2 — boot menu MVP (Android only)
- [ ] Repack init_boot with menu (scripts/repack-initboot.sh) from stock images
- [ ] Flash boot.img (multiboot variant) + init_boot — menu appears, default boots Android
- [ ] Choice persistence via /metadata verified across reboots
- [ ] Watchdog safety verified (menu timeout can't hang boot)

## Phase 3 — multiple Android ROMs
- [ ] Build ROM image files (system.img etc.) from two ROM fastboot payloads
- [ ] multiboot-mount helper + .rc triggers; boot ROM2 via menu
- [ ] OTA re-application script (repack against new stock images automatically)
- [ ] Optional: avbroot integration for clean AVB re-signing instead of disable-verification

## Phase 4 — non-Android OSes
- [ ] Linux rootfs (Halium-style) image; boot via menu entry "linux-hal" (Android-kernel path)
- [ ] kexec smoke test: kexec -l same kernel + tiny initramfs → kexec -e (proves syscall path)
- [ ] kexec to mainline: needs SM8635 mainline DTS (start from sm8550/sm8650 upstream DTS),
      UART console first, display later; fallback `fastboot boot` path remains the default
- [ ] postmarketOS / Ubuntu Touch packaging once DTS reaches basic boot

## Phase 5 — polish
- [ ] Menu: smooth UI, status line (battery? no — keep dumb), multilingual labels
- [ ] Choice storage hardening (atomic write, checksum)
- [ ] CI: kernel release automation with changelog + boot.img/init_boot/vendor_boot bundle
- [ ] Documentation: user-facing install guide + screenshots

## Risk register (top 5)
1. `peridot_GKI.config` alone may not produce a bootable device kernel (missing platform
   pieces) → mitigate: compare with community peridot kernel trees if CI kernel misbehaves
2. Module signing/KMI edge cases → stock-variant kernel artifact as escape hatch
3. simpledrm not bound (ABL DT node missing) → menu falls back to key-only blind mode (timeout default)
4. kexec handoff hangs (Qualcomm security) → keep kexec experimental; fastboot boot fallback
5. OTA clobbers multiboot → document re-apply flow; later automate via avbroot
