You are research subagent R2 of a project to add a multi-OS boot menu to a Xiaomi POCO F6 (codename "peridot", Redmi Turbo 3, Snapdragon 8s Gen 3 / SM8635, kernel 6.1 GKI, Android 14 HyperOS, A/B slots, unlocked bootloader).

Your task: survey ALL existing multi-boot / boot-menu approaches on Android devices and evaluate what is viable on THIS device (modern GKI 2.0, no custom bootloader possible).

Write your findings to: /root/peridot-multiboot/docs/03-multiboot-approaches.md

You have internet access: `curl -s`, `gh api`, `gh search repos/code`. Study real projects on GitHub, do NOT invent. Mark uncertain items "NEEDS VERIFICATION".

Cover:
1. chenxiaolong/MultiBoot (github.com/chenxiaolong/MultiBoot): how it patches boot ramdisk, menu UI (how it draws + reads keys pre-init), ROM layout on userdata, limitations (last supported Android version, no init_boot support?). Also chenxiaolong/avbroot: AVB re-signing, ramdisk injection via bootconfig/magisk-style patching — how it can inject our menu binary.
2. Legacy approaches: MultiROM (Tassadar) kexec-hardboot patch, Dual Boot Patcher — why they died (system-as-root, dynamic partitions, A/B, AVB).
3. kexec on Android: CONFIG_KEXEC in GKI kernels (check if android14-6.1 gki_defconfig enables KEXEC — state what you find), kexec -l vs kexec-hardboot (need for re-setup of hardware by outgoing kernel), kexec-tools static builds, loading a Linux kernel + DTB + initramfs from Android. Cold/real kexec limitations on Qualcomm (security: XBL/smem/hyp handoff, rpmh, "reboot to bootloader then fastboot boot" alternative).
4. Booting non-Android OSes on SM8635/SM8650-class devices: mainline Linux status for qcom SM8550/SM8650 (postmarketOS, pmbootstrap, sm8650 mainline DTS,RB3 Gen2), whether SM8635 needs a new DTS based on sm8550/sm8650, Halium 10+/UBports (Android-kernel-based rootfs — runs "apart from Android" UI like Ubuntu Touch while reusing peridot kernel), and postmarketOS on Xiaomi devices with android kernels (multi-profile boot via pmbootstrap initramfs menu).
5. Boot menu implementation techniques pre-init: static binary in init_boot ramdisk drawing to /dev/graphics/fb0 (or DRM dumb buffer), reading /dev/input/event* (volume keys = up/down, power = select; note keycodes), timeouts, setting androidboot.slot_suffix via kernel cmdline / bootconfig, writing chosen ROM to /misc or a file on /metadata, then exec switching: (a) continue to Android with chosen ROM root dir (chenxiaolong-style bind mounts via a patched init), (b) kexec into mainline/Linux kernel, (c) fastboot boot fallback, (d) reboot-recovery.
6. ROM layout schemes for multiple ANDROID ROMs on one userdata: directory-per-ROM (system_a in /data/multiboot/romN/system etc.), bind-mount strategy in first-stage vs second-stage init, dynamic partitions vs subdirectory approach on modern super-partition devices, what breaks (system-as-root, erofs, apex, vendor inconsistencies, vbmeta per-ROM).
7. Comparison matrix: approach x (invasiveness, kernel mods needed, supports Android+Android, Android+Linux, persistence across OTA, brick risk, works on peridot?). Then a "Recommended architecture for peridot" section: menu in init_boot/vendor_boot ramdisk + optional KEXEC-enabled GKI kernel + per-ROM dirs + kexec path for mainline Linux, with honest open risks.

Requirements: MD file with title, TL;DR, sections, comparison table, URLs for every claim, "NEEDS VERIFICATION" section. Target 350+ lines. Write the file with your write tool. Do not stop early.
