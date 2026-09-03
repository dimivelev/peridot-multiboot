/*
 * bootmenu — pre-init multi-OS boot menu for POCO F6 (peridot)
 *
 * Runs as /init inside the (repacked) init_boot generic ramdisk, BEFORE Android's
 * first-stage init. It:
 *   1. mounts devtmpfs/sysfs/proc
 *   2. draws a menu (DRM dumb buffer via simpledrm; fb0 legacy; blind mode ok)
 *   3. reads volume/power keys from /dev/input/event*
 *   4. persists the choice to /metadata/multiboot/active
 *   5. execs /init.real (original Android init)
 *
 * Design rules: no libc dynamic deps, no threads, fail-open (always exec init.real),
 * watchdog-friendly (never blocks forever: global timeout falls back to default entry).
 *
 * DISPLAY BACKENDS (verified against gki_defconfig — see docs/04 §4):
 *   1. DRM dumb buffer on /dev/dri/card0 — the REAL path on GKI: CONFIG_FB is not
 *      set, so /dev/graphics/fb0 does not exist; our kernel fragment enables
 *      CONFIG_DRM_SIMPLEDRM=y so simpledrm binds the ABL-provided framebuffer
 *      before vendor display modules load. (TODO Phase 1: implement dumb-buffer
 *      backend; current code ships the fbdev backend which only fires on
 *      non-GKI kernels with FB=y.)
 *   2. fbdev /dev/graphics/fb0 — legacy fallback
 *   3. blind mode — no display: timeout persists default choice anyway; keys still work
 *
 * Build (static, aarch64):
 *   make CC=aarch64-linux-musl-gcc static
 *   # or: zig cc -target aarch64-linux-musl ...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>

#define MENU_TIMEOUT_SEC   5
#define ACTIVE_PATH        "/metadata/multiboot/active"
#define INIT_REAL          "/init.real"
#define FALLBACK_ENTRY     "android"

typedef struct {
    const char *label;
    const char *value;
} entry_t;

static const entry_t entries[] = {
    { "Android (primary ROM)",        "android"    },
    { "Android (ROM 2)",              "rom2"       },
    { "Linux rootfs (android kernel)","linux-hal"  },
    { "Mainline Linux (kexec, exp.)", "mainline"   },
    { "Recovery",                     "recovery"   },
    { "Fastboot",                     "fastboot"   },
};
#define N_ENTRIES (int)(sizeof(entries) / sizeof(entries[0]))

/* ---------------- basic mounts ---------------- */

static void mount_fs(const char *src, const char *tgt, const char *type,
                     unsigned long flags, const void *data)
{
    mkdir(tgt, 0755);
    mount(src, tgt, type, flags, data);
}

static void early_mounts(void)
{
    mount_fs("devtmpfs", "/dev",     "devtmpfs", MS_NOSUID, "mode=0755");
    mount_fs("proc",     "/proc",    "proc",     MS_NOSUID|MS_NODEV|MS_NOEXEC, NULL);
    mount_fs("sysfs",    "/sys",     "sysfs",    MS_NOSUID|MS_NODEV|MS_NOEXEC, NULL);
    mount_fs("none",     "/config",  "configfs", MS_NOSUID|MS_NODEV|MS_NOEXEC, NULL);
    mount_fs("none",     "/dev/pts", "devpts",   MS_NOSUID|MS_NOEXEC, "mode=0620");
}

/* ---------------- framebuffer ---------------- */

static int fb_fd = -1;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
static unsigned char *fbmem;

static int fb_open(void)
{
    fb_fd = open("/dev/graphics/fb0", O_RDWR);
    if (fb_fd < 0)
        fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0)
        return -1;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        close(fb_fd); fb_fd = -1; return -1;
    }
    if (vinfo.bits_per_pixel != 32) { close(fb_fd); fb_fd = -1; return -1; }
    fbmem = malloc(finfo.line_length * vinfo.yres_virtual);
    if (!fbmem) { close(fb_fd); fb_fd = -1; return -1; }
    return 0;
}

static void fb_close(void)
{
    if (fb_fd >= 0) { close(fb_fd); fb_fd = -1; }
    free(fbmem); fbmem = NULL;
}

/* BGRX 32bpp; channels are memory addresses: [+0]=B [+1]=G [+2]=R [+3]=X on sm8x */
static void fb_rect(int x, int y, int w, int h, unsigned r, unsigned g, unsigned b)
{
    if (fb_fd < 0) return;
    long bpp = 4;
    for (int row = y; row < y + h && row < (int)vinfo.yres; row++) {
        unsigned char *line = fbmem + (long)row * finfo.line_length;
        for (int col = x; col < x + w && col < (int)vinfo.xres; col++) {
            unsigned char *p = line + (long)col * bpp;
            p[0] = b; p[1] = g; p[2] = r; p[3] = 0xff;
        }
    }
}

/* Ultra-minimal 5x7 bitmap font: only uppercase, digits, space, (),:.-/ */
static const unsigned char font5x7[][5] = {
    ['A']=0x7E,0x11,0x11,0x11,0x7E, ['B']=0x7F,0x49,0x49,0x49,0x36,
    ['C']=0x3E,0x41,0x41,0x41,0x22, ['D']=0x7F,0x41,0x41,0x22,0x1C,
    ['E']=0x7F,0x49,0x49,0x49,0x41, ['F']=0x7F,0x09,0x09,0x01,0x01,
    ['G']=0x3E,0x41,0x41,0x51,0x32, ['H']=0x7F,0x08,0x08,0x08,0x7F,
    ['I']=0x00,0x41,0x7F,0x41,0x00, ['L']=0x7F,0x40,0x40,0x40,0x40,
    ['M']=0x7F,0x02,0x04,0x02,0x7F, ['N']=0x7F,0x04,0x08,0x10,0x7F,
    ['O']=0x3E,0x41,0x41,0x41,0x3E, ['P']=0x7F,0x09,0x09,0x09,0x06,
    ['R']=0x7F,0x09,0x19,0x29,0x46, ['S']=0x26,0x49,0x49,0x49,0x32,
    ['T']=0x01,0x01,0x7F,0x01,0x01, ['U']=0x3F,0x40,0x40,0x40,0x3F,
    ['X']=0x63,0x14,0x08,0x14,0x63, ['Y']=0x03,0x04,0x78,0x04,0x03,
    ['0']=0x3E,0x51,0x49,0x45,0x3E, ['1']=0x00,0x42,0x7F,0x40,0x00,
    ['2']=0x42,0x61,0x51,0x49,0x46, ['3']=0x21,0x41,0x45,0x4B,0x31,
    ['4']=0x18,0x14,0x12,0x7F,0x10, ['5']=0x27,0x45,0x45,0x45,0x39,
    ['6']=0x3C,0x4A,0x49,0x49,0x30, ['7']=0x01,0x71,0x09,0x05,0x03,
    ['8']=0x36,0x49,0x49,0x49,0x36, ['9']=0x06,0x49,0x49,0x29,0x1E,
    [' ']=0x00,0x00,0x00,0x00,0x00, ['(']=0x00,0x1C,0x22,0x41,0x00,
    [')']=0x00,0x41,0x22,0x1C,0x00, [':']=0x00,0x36,0x36,0x00,0x00,
    ['-']=0x08,0x08,0x08,0x08,0x08, ['>']=0x20,0x10,0x08,0x04,0x02,
    ['/']=0x20,0x10,0x08,0x04,0x02, ['.']=0x00,0x30,0x30,0x00,0x00,
};

static void fb_char(int x, int y, char c)
{
    const unsigned char *g = (c >= 0 && (size_t)c < sizeof(font5x7)/sizeof(font5x7[0]))
                             ? font5x7[(int)c] : NULL;
    if (!g) return;
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 7; row++)
            if (g[col] & (1 << row))
                fb_rect(x + col * 3, y + row * 3, 3, 3, 255, 255, 255);
}

static void fb_text(int x, int y, const char *s)
{
    for (; *s; s++, x += 18) fb_char(x, y, *s);
}

static void fb_flush(void)
{
    if (fb_fd >= 0) {
        vinfo.yoffset = (vinfo.yoffset == 0) ? 1 : 0; /* pan if double-buffered */
        ioctl(fb_fd, FBIOPAN_DISPLAY, &vinfo);
        lseek(fb_fd, 0, SEEK_SET);
        write(fb_fd, fbmem, (size_t)finfo.line_length * vinfo.yres);
    }
}

/* uppercase helper for the tiny font */
static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static void draw_menu(int sel)
{
    if (fb_fd < 0) return;
    fb_rect(0, 0, vinfo.xres, vinfo.yres, 16, 16, 24);
    int y = vinfo.yres / 8;
    fb_text(vinfo.xres / 8, y, "PERIDOT MULTIBOOT"); y += 60;
    for (int i = 0; i < N_ENTRIES; i++) {
        char line[64];
        snprintf(line, sizeof(line), "%s %s", (i == sel) ? ">" : " ", entries[i].label);
        if (i == sel) fb_rect(vinfo.xres / 8 - 12, y - 8, vinfo.xres * 3 / 4, 40, 48, 48, 160);
        for (char *p = line; *p; p++) *p = up(*p);
        fb_text(vinfo.xres / 8, y, line);
        y += 48;
    }
    fb_text(vinfo.xres / 8, y + 40, "VOL:MOVE POWER:SELECT 5S:AUTO");
    fb_flush();
}

/* ---------------- input ---------------- */

static int open_keyboards(int *fds, int max)
{
    DIR *d = opendir("/dev/input");
    if (!d) return 0;
    struct dirent *e;
    int n = 0;
    char path[256];
    while ((e = readdir(d)) && n < max) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned char bits[(KEY_MAX + 7) / 8];
        memset(bits, 0, sizeof(bits));
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) { close(fd); continue; }
        /* only devices that have volume keys (gpio-keys on qcom) */
        if ((bits[KEY_VOLUMEUP / 8] & (1 << (KEY_VOLUMEUP % 8)))) fds[n++] = fd;
        else close(fd);
    }
    closedir(d);
    return n;
}

/* returns selected index, or default on timeout */
static int menu_loop(int sel)
{
    int fds[16];
    int nfds = open_keyboards(fds, 16);
    time_t deadline = time(NULL) + MENU_TIMEOUT_SEC;

    while (time(NULL) < deadline) {
        for (int i = 0; i < nfds; i++) {
            struct input_event ev;
            ssize_t r;
            while ((r = read(fds[i], &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
                if (ev.type != EV_KEY || ev.value != 1) continue;
                switch (ev.code) {
                case KEY_VOLUMEUP:   sel = (sel + N_ENTRIES - 1) % N_ENTRIES; draw_menu(sel); deadline = time(NULL) + MENU_TIMEOUT_SEC; break;
                case KEY_VOLUMEDOWN: sel = (sel + 1) % N_ENTRIES;             draw_menu(sel); deadline = time(NULL) + MENU_TIMEOUT_SEC; break;
                case KEY_POWER:      goto done;
                }
            }
            (void)r;
        }
        usleep(50 * 1000);
    }
done:
    for (int i = 0; i < nfds; i++) close(fds[i]);
    return sel;
}

/* ---------------- persistence & exec ---------------- */

static int mount_metadata(void)
{
    const char *devs[] = { "/dev/block/by-name/metadata", "/dev/block/bootdevice/by-name/metadata", NULL };
    for (int i = 0; devs[i]; i++) {
        if (access(devs[i], F_OK) == 0) {
            mkdir("/metadata", 0755);
            if (mount(devs[i], "/metadata", "ext4", MS_NOSUID|MS_NODEV, NULL) == 0) return 0;
            if (mount(devs[i], "/metadata", "f2fs", MS_NOSUID|MS_NODEV, NULL) == 0) return 0;
        }
    }
    return -1;
}

static void save_choice(const char *v)
{
    if (mount_metadata() != 0) return;
    mkdir("/metadata/multiboot", 0755);
    FILE *f = fopen(ACTIVE_PATH, "w");
    if (f) { fprintf(f, "%s\n", v); fclose(f); sync(); }
    umount2("/metadata", MNT_DETACH);
}

static const char *read_default(void)
{
    static char buf[64];
    if (mount_metadata() != 0) return FALLBACK_ENTRY;
    FILE *f = fopen(ACTIVE_PATH, "r");
    if (!f) return FALLBACK_ENTRY;
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return FALLBACK_ENTRY; }
    fclose(f);
    buf[strcspn(buf, "\n")] = 0;
    umount2("/metadata", MNT_DETACH);
    for (int i = 0; i < N_ENTRIES; i++)
        if (strcmp(buf, entries[i].value) == 0) return entries[i].value;
    return FALLBACK_ENTRY;
}

static void do_reboot_target(const char *v)
{
    if (strcmp(v, "recovery") == 0)
        syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                LINUX_REBOOT_CMD_RESTART2, "recovery");
    else if (strcmp(v, "fastboot") == 0)
        syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                LINUX_REBOOT_CMD_RESTART2, "bootloader");
}

int main(void)
{
    early_mounts();

    const char *def = read_default();
    int sel = 0;
    for (int i = 0; i < N_ENTRIES; i++)
        if (strcmp(entries[i].value, def) == 0) { sel = i; break; }

    if (fb_open() == 0) draw_menu(sel);
    sel = menu_loop(sel);
    if (fb_fd >= 0) fb_flush();
    fb_close();

    const char *v = entries[sel].value;
    save_choice(v);

    if (strcmp(v, "recovery") == 0 || strcmp(v, "fastboot") == 0) {
        do_reboot_target(v);
        /* if reboot2 failed, fall through to android */
        v = "android";
    }

    sync();
    /* kexec path handled by a helper invoked later (see docs 02 §2.3) — v1 always execs init */
    execl(INIT_REAL, "init", (char *)NULL);

    /* last resort: try android init from vendor ramdisk location variants */
    execl("/init.android", "init", (char *)NULL);
    return 1; /* kernel panics with "no init" — but we synced our choice already */
}
