# peridot-multiboot

Multi-OS boot menu for the Xiaomi **POCO F6 / Redmi Turbo 3** (`peridot`,
Snapdragon 8s Gen 3 / SM8635, GKI kernel 6.1). Boots several Android ROMs **and**
non-Android Linux from one device — no bootloader modification (impossible: signed XBL/ABL),
only repacked `boot` / `init_boot` / `vendor_boot` images on an unlocked device.

```
menu (pre-init /init in init_boot ramdisk)
 ├─ Android primary / ROM2 / ROM3  → per-ROM loop-mounted images (chenxiaolong-style, modernized)
 ├─ Linux rootfs (Halium-style)    → android-kernel rootfs switch
 ├─ Mainline Linux (experimental)  → kexec (KEXEC enabled in our GKI kernel) / fastboot boot fallback
 └─ Recovery / Fastboot            → reboot-restart2 targets
```

## Repo contents

| Path | What |
|---|---|
| `docs/` | Knowledge base — start at `docs/00-overview.md` |
| `bootmenu/` | Pre-init framebuffer menu (static aarch64, DRM/simpledrm display, evdev keys) |
| `kernel-patches/` | Kconfig fragment enabling KEXEC + simpledrm (KMI-safe) |
| `scripts/` | repack/flash helpers, subagent prompts |
| `workflows/` | GitHub Actions pipelines (mirrored to `.github/workflows/`) |
| `kernel/peridot-u/` | local (gitignored) shallow clone of `MiCode/Xiaomi_Kernel_OpenSource@peridot-u-oss` |

## CI (GitHub Actions)

Two workflows build everything on GitHub-hosted runners:

- **build-kernel** — clones the Xiaomi kernel branch, applies the multiboot fragment,
  builds with AOSP clang (`LLVM=1`), packages a header-v4 kernel-only `boot.img`
  (+ both Image.gz/Image.lz4), uploads artifacts, releases on tags.
- **build-bootmenu** — cross-compiles the static menu binary with zig/musl, smoke-tests
  under qemu-aarch64, uploads the artifact.

Run them from the *Actions* tab (workflow_dispatch) or push to `main`.

## Warning

This is advanced-device-modification territory. Read `docs/06-flash-and-rollback.md`
before flashing anything. Signed bootloader partitions must never be flashed.
