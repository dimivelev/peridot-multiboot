# POCO F6 (peridot) Multi-OS Boot Project

> Goal: boot multiple operating systems (several Android ROMs **and** non-Android Linux) on a
> Xiaomi POCO F6 (`peridot`, Redmi Turbo 3, Snapdragon 8s Gen 3 / SM8635) from a boot-time menu,
> without touching the signed bootloader.

## Document index

| File | Content | Status |
|---|---|---|
| `00-overview.md` | This file — index, glossary, quick facts | ✅ |
| `01-device-and-bootchain.md` | Device, partitions, boot chain, unlock, GKI status | ✅ subagent R1 (471 lines) |
| `02-multiboot-architecture.md` | **The concrete design** — how the menu + multi-OS switching works | ✅ |
| `03-multiboot-approaches.md` | Survey of existing multiboot projects & what's reusable | ✅ |
| `04-kernel-analysis.md` | Analysis of the actual `peridot-u-oss` kernel tree (defconfigs, KEXEC, KMI) | ✅ verified greps |
| `05-github-actions-build.md` | Research: kernel CI on GitHub Actions + recommended pipeline | ✅ subagent R3 (914 lines) |
| `06-flash-and-rollback.md` | Flashing procedure, backups, unbrick limits | ✅ |
| `07-roadmap.md` | Phased implementation plan | ✅ |
| `08-ci-verification.md` | CI verification results — first successful kernel build | ✅ |
| `09-full-functionality-plan.md` | **Everything needed for full functionality + how to get it** | ✅ |

## Quick facts

- Device: POCO F6 / Redmi Turbo 3 — codename **peridot**
- SoC: Qualcomm **SM8635** "Snapdragon 8s Gen 3" (pineapple-family platform, sibling of SM8650)
- Kernel: Linux **6.1** GKI 2.0 (`android14-6.1`), official source:
  `MiCode/Xiaomi_Kernel_OpenSource`, branch **`peridot-u-oss`**
- Boot images: `boot` (GKI kernel), `init_boot` (generic ramdisk w/ `init`),
  `vendor_boot` (vendor ramdisk + DTB), `dtbo`, `vbmeta*` — all A/B
- Bootloader unlockable (Xiaomi unlock, orange state). XBL/ABL are signed → **cannot** be
  replaced → the menu must live in boot images (ramdisk/kernel), not the bootloader.

## Repository layout

```
peridot-multiboot/
├── docs/                 # knowledge base (this folder)
├── bootmenu/             # pre-init framebuffer boot menu (static aarch64 binary)
├── kernel-patches/       # kernel config/source patches (KEXEC etc.)
├── scripts/              # build/flash helper scripts, subagent prompts
├── workflows/            # source of truth for .github/workflows
├── logs/                 # subagent / build logs (not committed)
└── kernel/peridot-u/     # shallow clone of the kernel source (not committed)
```

## Glossary

- **GKI** — Generic Kernel Image: Google's split where the kernel in `boot` is generic and
  device specifics live in vendor ramdisk modules + DTB. Constrains how much we may change
  the kernel config without breaking vendor module loading (KMI).
- **KMI** — Kernel Module Interface: the stable ABI between GKI kernel and vendor modules.
- **init_boot** — A/B partition holding the *generic* ramdisk (contains `/init` binary).
- **vendor_boot** — vendor ramdisk (fstab, vendor init .rc, kernel modules) + DTB.
- **AVB** — Android Verified Boot; skipped/fail-open in orange (unlocked) state.
- **kexec** — Linux syscall to load and jump into another kernel without rebooting.
