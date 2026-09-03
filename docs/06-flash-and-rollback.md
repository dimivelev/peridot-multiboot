# Flashing, Backups & Rollback (POCO F6 / peridot)

> ⚠️ Unlocking wipes userdata ONCE (at unlock). Everything after that is reversible
> as long as you never flash `xbl`, `xbl_config`, `abl`, `tz`, `aop`, `keymaster`,
> `modem`, or `devcfg` partitions — those are signed and effectively unrecoverable
> without an authorized EDL account.

## 0. One-time setup

1. **Unlock bootloader** (Xiaomi procedure: bind Mi account in developer options →
   Mi Unlock tool on Windows → 168h/7d waiting period → unlock). This wipes userdata.
2. **Save stock images** from the fastboot ROM package matching your exact HyperOS build
   (extract `images/boot.img`, `init_boot.img`, `vendor_boot.img`, `vbmeta.img`,
   `vbmeta_system.img`, `dtbo.img`).
   **Store these forever — they are your get-out-of-jail card.**
3. Disable AVB enforcement once:
   ```
   fastboot flash vbmeta --disable-verity --disable-verification vbmeta.img
   ```
   (orange state warning at boot is expected and harmless)

## 1. Installing multiboot (order matters)

```bash
# from the Actions artifacts (or local builds):
scripts/flash.sh boot        peridot-multiboot/boot.img        # custom GKI kernel (KEXEC+simpledrm)
scripts/flash.sh initboot    peridot-multiboot/init_boot.img   # bootmenu as /init
scripts/flash.sh vendorboot  peridot-multiboot/vendor_boot.img # multiboot.rc
```
Validate on first boot: menu must appear within ~5 s of kernel start, Vol± navigates,
Power selects; doing nothing auto-picks the previous choice.

## 1a. Try the menu FIRST — zero extraction, zero flashing (recommended)

`init_boot`/`vendor_boot` must be repacked from **your** stock images because they contain
Xiaomi's proprietary first-stage init + build-pinned DTB/fstab/modules (and we can't
distribute Xiaomi firmware). But the kernel + menu alone are fully buildable from source,
so CI produces **`boot-menu-test.img`** (multiboot kernel + ramdisk containing ONLY the menu):

```bash
fastboot boot boot-menu-test.img   # temporary — nothing is written to any partition
```

- The menu draws (simpledrm), Vol± / Power navigate, 5 s timeout auto-reboots.
- When `/init.real` is absent (test image), the menu reboots instead of panicking.
- If the menu renders and keys respond → flashing the real thing is just §1.

Only the **permanent install** needs the one-time stock-image extraction (`extract-rom-images.sh`),
the same one-time pull Magisk asks for.

## 2. Testing the kexec/mainline path safely

1. Menu → "Mainline Linux (kexec, exp.)" only after `kexec` smoke tests
   (`/proc/kexec` availability, `kexec -l` + `kexec -e` with a trivial kernel) from
   an adb root shell on the running Android.
2. Golden rule: **kexec experiments are boot-image-only** — if the handoff hangs,
   force-restart (Power 10 s); no persistent state was touched, stock boot returns
   via the same menu.

## 3. Rollback matrix

| Symptom | Fix |
|---|---|
| Bootloop after flashing boot.img | `fastboot flash boot boot.stock.img` |
| Menu never appears (init_boot bad) | `fastboot flash init_boot init_boot.stock.img` |
| Android boots but vendor modules fail (logcat: EKEYREJECTED / CRC errors) | use the stock-variant kernel artifact, or re-flash stock boot |
| vendor_boot repack broke first-stage init | `fastboot flash vendor_boot vendor_boot.stock.img` |
| Entire multiboot removal | `scripts/flash.sh restore BACKUP_DIR` |
| OTA arrived | do **not** accept while multiboot images are installed; update, then re-run §1 with fresh stock images of the new build |

## 4. Hard-brick boundary (what NOT to touch)

- Never flash/erase: `xbl`, `xbl_config`, `abl`, `aop`, `tz`, `hyp`, `keymaster`, `bootconfig`,
  `modem*`, `devcfg`, `qupfw`, `uefisecapp`, `imagefv`, `multiimgoem`.
- No TWRP-style recovery magic can save those on SM8635; EDL (9008) requires an authorized
  Xiaomi account/credit — assume it's unavailable.
- If boot.img/init_boot/vendor_boot are the only partitions you touch, the worst case is
  `fastboot` still works → always recoverable.

## 5. Anti-rollback (ARB)

Xiaomi increments anti-rollback versions with some firmware updates. Downgrading below
your device's ARB can hard-brick. **Never flash older firmware packages blindly** —
check your current ARB via fastboot `getvar anti` (if supported).
