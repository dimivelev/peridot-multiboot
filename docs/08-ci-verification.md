# CI Verification Results — build #33757300386 (2026-09-03)

> **First fully successful pipeline:** kernel compiled, packaged, artifacts uploaded.
> Repo: https://github.com/dimivelev/peridot-multiboot · Run:
> https://github.com/dimivelev/peridot-multiboot/actions/runs/33757300386

## 1. What was built

| Variant | Result | Artifacts |
|---|---|---|
| `peridot kernel (stock)` | ✅ 18 min | `boot-stock.img` (128 MB w/ AVB footer), `Image`, `Image.gz` (13.8 MB), `.config` |
| `peridot kernel (multiboot)` | ✅ 18 min | `boot-multiboot.img`, `Image`, `Image.gz`, `.config` |
| Kernel source | `MiCode/Xiaomi_Kernel_OpenSource@peridot-u-oss` commit `062233df` (the clone in this repo's setup) |

## 2. Verified: the multiboot modification is exactly as designed

`diff kernel-stock.config kernel-multiboot.config` (20 lines):

```
< # CONFIG_KEXEC is not set          > CONFIG_KEXEC=y
< # CONFIG_KEXEC_FILE is not set     > CONFIG_KEXEC_FILE=y
< CONFIG_MODULE_SIG_PROTECT=y        > # CONFIG_MODULE_SIG_PROTECT is not set
                                     > CONFIG_CRASH_CORE=y          (auto dependency)
                                     > CONFIG_KEXEC_CORE=y          (auto dependency)
                                     > CONFIG_APERTURE_HELPERS=y    (auto dependency)
                                     > CONFIG_DRM_SIMPLEDRM=y
```

- Only additive changes (plus the two protected-config disables) → KMI-safe by design ✔
- `CONFIG_KEXEC_SIG` not set (unsigned mainline kernels accepted) ✔
- kconfig auto-resolved simpledrm's `APERTURE_HELPERS` dependency ✔

## 3. boot.img sanity

- Magic `ANDROID!` ✔, kernel_size = 13,811,840 (matches `Image.gz`) ✔, no ramdisk/DTB (GKI) ✔
- header size 1584 (v4) ✔, AVB hash footer added (`avbtool add_hash_footer`, algorithm NONE —
  unsigned; orange-state devices flash vbmeta with verification disabled anyway)

## 4. Pipeline hardening learned along the way (all fixed in workflow)

| Failure | Fix |
|---|---|
| `pip install mkbootimg` — not on PyPI | `scripts/fetch-android-tools.sh`: install from googlesource repo archive incl. `gki/` package (master mkbootimg.py imports it) |
| `mkdir: command not found` mid-run | fetch script wrote `PATH` via `GITHUB_ENV` (REPLACES the whole runner PATH) → switched to `GITHUB_PATH` (prepends) |
| make died in ~12 s, error invisible | `tee /mnt/build.log` failed — **/mnt is root-owned on runners**; also `cmd \| tail` masks exit codes → now: chown'd `/mnt/ccache`, build.log in workspace, `set -o pipefail` + `PIPESTATUS`, error-group dump |
| `can't open file "drivers/misc/hwid/Kconfig"` | MiCode peridot-u-oss is an incomplete export → `scripts/repair-kernel-tree.sh` fetches the device-agnostic MI hwid driver from `Pzqqt/android_kernel_xiaomi_marble` + iteratively stubs any other missing Kconfig sources |
| toolchain re-downloaded per attempt | separate `toolchain` job caches `~/toolchain` (`actions/cache`, key `clang-r530567-v1`) |

## 5. Remaining on-device verification (Phase 1 of roadmap)

1. Flash `boot-multiboot.img` → check boot; `dmesg` for module load errors (EKEYREJECTED /
   vermagic / CRC) → resolves the MODVERSIONS-vs-byte-for-byte vermagic question (doc 04 §5)
2. Confirm simpledrm bound: `/proc/device-tree/reserved-memory/framebuffer*`,
   `/sys/class/drm/card0-*/status`
3. `/proc/version` — capture the stock `UTS_RELEASE` as a hedge
