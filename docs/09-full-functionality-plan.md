# Full Functionality: Ingredient List & How to Get Everything

> Target end state: boot menu on every boot → multiple Android ROMs + Linux rootfs, all
> switchable, stock always recoverable. This doc lists every ingredient, its status, and
> the exact way to obtain/build it.

## 0. Prerequisites matrix

| # | Ingredient | Status | How to get |
|---|---|---|---|
| 1 | Unlocked bootloader | ❌ **your device** (time-gated: start NOW) | §1 below |
| 2 | Custom GKI kernel (`boot-multiboot.img`) | ✅ **built** — CI run 33805213043 | Actions → artifacts (or `/mnt/storagebox/artifacts-*`) |
| 3 | Bootmenu binary | ✅ **built** | same artifacts |
| 4 | Zero-flash menu trial (`boot-menu-test.img`) | ✅ **built** | same artifacts |
| 5 | Stock `init_boot.img` + `vbmeta.img` (your build) | ❌ **your firmware** | §2 below |
| 6 | Repacked `init_boot.mb.img` (menu injected) | ⏳ 1 command once #5 exists | §3 below |
| 7 | PC + USB-C cable + platform-tools | ❌ **you** | `adb`/`fastboot` from developer.android.com |
| 8 | Second Android ROM (images) | ❌ **pick a ROM** | §4 below |
| 9 | Multi-ROM switch glue (multiboot-mount) | ❌ **not written yet** — I build it (Phase 3) | §5 below |
| 10 | Non-Android Linux | ⏳ two tracks, see §6 | §6 below |

## 1. Bootloader unlock (start today — it has a waiting period)

1. Insert SIM, sign into Mi account, enable *Developer options* → *OEM unlocking* +
   *Mi Unlock status* (binds account to device).
2. On a Windows PC: install **Mi Unlock tool** (en.miui.com/unlock), sign in with the SAME
   account, run it → it reports the wait (community reports range 72 h–30 d; HyperOS-era
   accounts may need extra steps — doc 01 §4).
3. After the timer: repeat the tool → **Unlock**. ⚠️ **Wipes userdata** — back up first.
4. Verify: `fastboot oem device-info` → `unlocked: yes` (orange state at boot = expected).

## 2. Stock images matching YOUR firmware (the only "extract your own" part)

Why: `init_boot`/`vendor_boot` carry Xiaomi's proprietary first-stage init and build-pinned
DTB/fstab/modules; mixing builds risks first-stage failure, and Xiaomi firmware can't be
redistributed (that's why CI can't ship it — same reason Magisk patches *your* image).

1. On device: *Settings → About phone → HyperOS version* — note the **exact** string
   (e.g. `OS1.0.5.0.UNPCNXM`) **and** the region suffix.
2. Download the matching **fastboot ROM** (`.tgz`, several GB) for peridot/Redmi Turbo 3:
   - https://xiaomifirmwareupdater.com → peridot → your version → "Fastboot" (Recovery ROM
     works too but needs payload extraction instead — use fastboot ROM).
   - Mirrors: mifirm.net, bigota.d.miui.com links from XDA threads.
3. Extract the needed partitions:
   ```bash
   bash scripts/extract-rom-images.sh peridot_fastboot_OS1.0.5.0.tgz ./stock
   # → stock/boot.img init_boot.img vendor_boot.img vbmeta.img dtbo.img
   ```
4. **Keep this folder forever** — it is your rollback set (docs/06).

## 3. Build the multiboot init_boot (one command, on your PC)

```bash
# one-time: python3 + AOSP boot tools
bash scripts/fetch-android-tools.sh

# inject menu as /init (stock init kept as /init.real inside the ramdisk)
bash scripts/repack-initboot.sh stock/init_boot.img bootmenu/bootmenu init_boot.mb.img
```

Install order (docs/06 §1):
```bash
fastboot flash vbmeta --disable-verity --disable-verification stock/vbmeta.img   # once
scripts/flash.sh boot        boot-multiboot.img      # from CI artifacts
scripts/flash.sh initboot    init_boot.mb.img
# menu appears on every boot; Recovery/Fastboot entries work immediately
```

## 4. Second Android ROM

peridot has a healthy ROM scene (LineageOS 22 official, crDroid, PixelOS, …). Pick ONE
additional ROM — **Android 14/15 based** (GKI 6.1 compatible with our kernel).

1. Download its **recovery ROM zip** (contains `payload.bin`).
2. Extract the dynamic-partition images:
   ```bash
   pip install payload_dumper        # or payload-dumper-go binary
   python3 -m payload_dumper --partitions system,product,vendor,system_ext \
       -o rom2-images/ lineage_peridot.zip
   ```
   (`scripts/make-images.sh` wraps this; Phase 3 will finalize it.)
3. Push to device: `adb push rom2-images/ /data/multiboot/rom2/` (adb push to /data works
   after ROM1 is booted and setup finished).

## 5. Multi-ROM switching glue — what I still owe you (Phase 3)

- [ ] `multiboot-mount` helper: reads `/metadata/multiboot/active`, `losetup`s
      `/data/multiboot/romN/{system,product,vendor}.img`, bind-mounts over the running
      system before zygote (docs/02 §2.1 Scheme A) — plain static binary, goes into the
      vendor_boot ramdisk via `scripts/repack-vendorboot.sh`.
- [ ] Wire `multiboot.rc` triggers properly (first-stage-safe property wait).
- [ ] OTA re-apply script: after a HyperOS OTA, re-run §3 + §5 against the new stock images.
- [ ] Release automation: CI tag → bundle (kernel + menu + repack instructions).

Estimate: these are small, well-scoped pieces — no device needed to develop, CI-verifiable.

## 6. Non-Android Linux — two tracks

**Track A (works with what we already have): Linux rootfs on the peridot kernel.**
A systemd/Debian/Ubuntu rootfs as an ext4 image, booted by our kernel through a menu entry
that loop-mounts it and switch_roots (Halium-style: Android kernel drivers, non-Android
userspace). Needs: rootfs image (build via debootstrap, no device deps) + a small
`init.linux` first-stage (I write it). Display/touch/wifi come from the Android kernel +
vendor blobs injected into the rootfs (same glue Halium uses).

**Track B (true mainline): needs a SM8635 device tree — the long pole.**
Mainline has SM8550/SM8650 but **zero SM8635** DTS (doc 03 §5). A peridot DTS starts as a
derivative of `sm8550-mtp.dts` + peridot's stock DTB properties (dump from device:
`adb shell su -c "dtc -I fs -O dts /sys/firmware/devicetree/base"`) and needs UART or
fastboot console iteration. Once a mainline kernel boots: kexec path is already enabled
in our kernel (`CONFIG_KEXEC=y`), fallback stays `fastboot boot`.

## 7. Execution order (put simply)

```
TODAY      start unlock timer (§1)                     ← only time-gated item
T+7..30d   unlock (wipes data) → back up → verify orange state
           fastboot boot boot-menu-test.img            ← see the menu, flash nothing
           flash kernel + repacked init_boot (§3)      ← permanent menu
           test Recovery/Fastboot menu entries
WEEK+      pick ROM2, extract images (§4), flash glue when I ship Phase 3 (§5)
LATER      Linux rootfs (Track A) / mainline DTS (Track B)
```
