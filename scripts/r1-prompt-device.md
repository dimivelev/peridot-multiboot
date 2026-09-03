You are research subagent R1 of a project to add a multi-OS boot menu to a Xiaomi POCO F6 (codename "peridot", also sold as Redmi Turbo 3).

Your task: produce an in-depth technical reference on the DEVICE and its BOOT CHAIN.

Write your findings to: /root/peridot-multiboot/docs/01-device-and-bootchain.md

You have internet access. Use `curl -s`, `gh api '...'`, `gh search repos/code` for research. Good sources: wiki.lineageos.org/devices/peridot, xda-developers threads, postmarketOS wiki, Xiaomi firmware pages (xiaomifirmwareupdater), AOSP docs (source.android.com) for AVB/GKI/boot flow, kernel.org docs. Do NOT invent facts; mark anything uncertain as "NEEDS VERIFICATION".

Cover in depth:
1. Device identity: POCO F6 / Redmi Turbo 3, codename peridot, SoC Qualcomm SM8635 "Snapdragon 8s Gen 3" (note its relationship to SM8650/pineapple platform), RAM/storage variants, shipped Android/HyperOS versions.
2. Partition layout: list typical partitions (boot_a/b, init_boot_a/b, vendor_boot_a/b, vendor_kernel_boot if present, dtbo, vbmeta, vbmeta_system, xbl/xbl_config, abl, tz, aop, keymaster, modem, super (system/product/vendor/system_ext/odm dynamics), userdata, metadata, misc, frp). Note which are A/B, sizes if findable, and what each stage loads.
3. Boot chain in detail: PBL -> XBL -> ABL (Little Kernel/UEFI-based abl.elf) -> AVB verification (vbmeta, dm-verity, orange state after unlock) -> GKI kernel in boot.img + generic ramdisk from init_boot.img + vendor_boot vendor ramdisk + DTB -> first-stage init -> dm/metadata -> second-stage init -> Android. Explain how Android 13+ splits generic ramdisk (init_boot) vs vendor ramdisk (vendor_boot), and where a custom boot menu could hook in.
4. Bootloader unlock: Xiaomi procedure (Mi Unlock / HyperOS waiting period), what orange state disables, what stays verified (XBL/abl cannot be replaced — explain why, signing/fuses), fastboot commands used on this device, anti-rollback.
5. GKI status: which GKI version (kernel 6.1, android14-6.1), KMI implications, vendor_boot ramdisk vendor modules, what that means for replacing the kernel in boot.img.
6. Known community efforts on peridot: LineageOS official support (how they build the kernel), custom kernels (list names/repos found), any existing multiboot attempts.
7. Safety notes: what can hard-brick (XBL, abl, xbl_config, no way to recover without EDL authorized account), what is safely flashable after unlock (boot, vendor_boot, dtbo, vbmeta with --disable-verity --disable-verification).

Requirements for the MD file: title, TL;DR box, detailed sections with tables, every factual claim sourced with a URL, a "NEEDS VERIFICATION" section at the end. Target 300+ lines. Write the file with your write tool. Do not ask questions, do not stop early — research thoroughly until the document is complete.
