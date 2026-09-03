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
#include <sys/mman.h>
#include <linux/reboot.h>
#include <sys/syscall.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>

#ifndef SYS_reboot
#define SYS_reboot __NR_reboot
#endif

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

/* ---------------- display backend ---------------- */
/*
 * Backends tried in order:
 *   1. DRM dumb buffer on /dev/dri/card0  — GKI path (simpledrm binds the
 *      ABL-provided framebuffer before vendor display modules load; CONFIG_FB
 *      is unset in gki_defconfig so there is no /dev/graphics/fb0)
 *   2. fbdev /dev/graphics/fb0 | /dev/fb0 — non-GKI kernels only
 *   3. blind mode — no display: keys + timeout still persist the choice
 */

#include <linux/drm.h>
#include <linux/drm_mode.h>
#else
/* Minimal DRM mode-setting UAPI — musl doesn't ship drm headers. Mirrors
 * include/uapi/drm/drm_mode.h (only what bootmenu uses). */
#include <linux/types.h>

#ifndef _IOWR
#define _IOWR(type,nr,size) _IOC(_IOC_READ|_IOC_WRITE,(type),(nr),sizeof(size))
#endif
#define DRM_IOCTL_BOOTMENU_BASE 'd'

#define DRM_IOCTL_MODE_GETRESOURCES _IOWR(DRM_IOCTL_BOOTMENU_BASE,0xA0,struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC      _IOWR(DRM_IOCTL_BOOTMENU_BASE,0xA1,struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETENCODER   _IOWR(DRM_IOCTL_BOOTMENU_BASE,0xA6,struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR _IOWR(DRM_IOCTL_BOOTMENU_BASE,0xA7,struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_GETFB        _IOWR(DRM_IOCTL_BOOTMENU_BASE,0xAD,struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_MAP_DUMB     _IOWR(DRM_IOCTL_BOOTMENU_BASE,0xB3,struct drm_mode_map_dumb)

#define DRM_DISPLAY_MODE_LEN 32

struct drm_mode_card_res {
	__u64 fb_id_ptr;
	__u64 crtc_id_ptr;
	__u64 connector_id_ptr;
	__u64 encoder_id_ptr;
	__u32 count_fbs;
	__u32 count_crtcs;
	__u32 count_connectors;
	__u32 count_encoders;
	__u32 min_width;
	__u32 max_width;
	__u32 min_height;
	__u32 max_height;
};

struct drm_mode_modeinfo {
	__u32 clock;
	__u16 hdisplay;
	__u16 hsync_start;
	__u16 hsync_end;
	__u16 htotal;
	__u16 hskew;
	__u16 vdisplay;
	__u16 vsync_start;
	__u16 vsync_end;
	__u16 vtotal;
	__u16 vscan;
	__u32 vrefresh;
	__u32 flags;
	__u32 type;
	char name[DRM_DISPLAY_MODE_LEN];
};

struct drm_mode_crtc {
	__u64 set_connectors_ptr;
	__u32 count_connectors;
	__u32 crtc_id;
	__u32 fb_id;
	__u32 x;
	__u32 y;
	__u32 gamma_size;
	__u32 mode_valid;
	struct drm_mode_modeinfo mode;
};

struct drm_mode_get_encoder {
	__u32 encoder_type;
	__u32 possible_crtcs;
	__u32 possible_clones;
	__u32 crtc_id;
};

struct drm_mode_get_connector {
	__u64 encoders_ptr;
	__u64 props_ptr;
	__u64 prop_values_ptr;
	__u64 modes_ptr;
	__u32 count_modes;
	__u32 count_props;
	__u32 count_encoders;
	__u32 encoder_id;
	__u32 connector_id;
	__u32 connector_type;
	__u32 connector_type_id;
	__u32 connection;
	__u32 mm_width;
	__u32 mm_height;
	__u32 subpixel;
	__u32 pad;
};

struct drm_mode_fb_cmd {
	__u32 fb_id;
	__u32 width;
	__u32 height;
	__u32 pitch;
	__u32 bpp;
	__u32 depth;
	__u32 handle;
};

struct drm_mode_map_dumb {
	__u32 handle;
	__u32 pad;
	__u64 offset;
};
#endif /* no linux/drm.h */

static int  disp_fd = -1;          /* backend file descriptor */
static unsigned char *scr_mem;     /* mapped scanout / framebuffer */
static long  scr_stride;           /* bytes per line */
static int   scr_w, scr_h;         /* visible dimensions */
static int   scr_is_fbdev;         /* fbdev needs explicit flush/pan */

/* --- 1) DRM dumb buffer (simpledrm) --- */

static int drm_open(void)
{
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;

    /* we are root/master on a KMS device with exactly one connector/crtc (simpledrm) */
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    uint32_t conns[8], encs[8], crtcs[8];
    res.connector_id_ptr = (uintptr_t)conns; res.count_connectors = 8;
    res.encoder_id_ptr   = (uintptr_t)encs;  res.count_encoders   = 8;
    res.crtc_id_ptr      = (uintptr_t)crtcs; res.count_crtcs      = 8;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 ||
        res.count_connectors < 1 || res.count_crtcs < 1) { close(fd); return -1; }

    struct drm_mode_get_connector conn;
    memset(&conn, 0, sizeof(conn));
    conn.connector_id = conns[0];
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0 || !conn.encoder_id) {
        close(fd); return -1;
    }
    struct drm_mode_get_encoder enc;
    memset(&enc, 0, sizeof(enc));
    enc.encoder_id = conn.encoder_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) < 0 || !enc.crtc_id) {
        close(fd); return -1;
    }

    /* current crtc state: active fb + real display size */
    struct drm_mode_crtc crtc;
    memset(&crtc, 0, sizeof(crtc));
    crtc.crtc_id = enc.crtc_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) < 0 || !crtc.fb_id || !crtc.mode_valid) {
        close(fd); return -1;
    }
    scr_w = crtc.x + crtc.mode.hdisplay;   /* conservative visible area */
    scr_h = crtc.y + crtc.mode.vdisplay;

    /* map the ACTIVE scanout buffer (no mode switch, no flicker) */
    struct drm_mode_fb_cmd fbcmd;
    memset(&fbcmd, 0, sizeof(fbcmd));
    fbcmd.fb_id = crtc.fb_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETFB, &fbcmd) < 0 || !fbcmd.handle || fbcmd.bpp != 32) {
        close(fd); return -1;
    }
    struct drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = fbcmd.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) { close(fd); return -1; }
    scr_mem = mmap(NULL, fbcmd.pitch * fbcmd.height, PROT_WRITE | PROT_READ,
                   MAP_SHARED, fd, mreq.offset);
    if (scr_mem == MAP_FAILED) { close(fd); return -1; }

    scr_stride  = fbcmd.pitch;
    disp_fd     = fd;
    scr_is_fbdev = 0;
    return 0;
}

/* --- 2) legacy fbdev --- */

static int fbdev_open(void)
{
    int fd = open("/dev/graphics/fb0", O_RDWR);
    if (fd < 0) fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) return -1;
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0 || vinfo.bits_per_pixel != 32) {
        close(fd); return -1;
    }
    scr_mem = malloc(finfo.line_length * vinfo.yres_virtual);
    if (!scr_mem) { close(fd); return -1; }
    memset(scr_mem, 0, finfo.line_length * vinfo.yres_virtual);
    scr_w = vinfo.xres; scr_h = vinfo.yres;
    scr_stride = finfo.line_length;
    disp_fd = fd; scr_is_fbdev = 1;
    return 0;
}

static int disp_open(void)
{
    if (drm_open() == 0)   return 0;
    if (fbdev_open() == 0) return 0;
    disp_fd = -1;          /* blind mode */
    return -1;
}

static void disp_close(void)
{
    if (disp_fd >= 0) close(disp_fd);
    disp_fd = -1;
    scr_mem = NULL;
}

/* BGRX 32bpp; channels are memory addresses: [+0]=B [+1]=G [+2]=R [+3]=X on sm8x */
static void fb_rect(int x, int y, int w, int h, unsigned r, unsigned g, unsigned b)
{
    if (disp_fd < 0 || !scr_mem) return;
    long bpp = 4;
    for (int row = y; row < y + h && row < scr_h; row++) {
        unsigned char *line = scr_mem + (long)row * scr_stride;
        for (int col = x; col < x + w && col < scr_w; col++) {
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

static void disp_flush(void)
{
    if (disp_fd < 0 || !scr_mem) return;
    if (scr_is_fbdev) {
        struct fb_var_screeninfo vinfo;
        if (ioctl(disp_fd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
            vinfo.yoffset = (vinfo.yoffset == 0) ? 1 : 0; /* pan if double-buffered */
            ioctl(disp_fd, FBIOPAN_DISPLAY, &vinfo);
        }
        lseek(disp_fd, 0, SEEK_SET);
        write(disp_fd, scr_mem, (size_t)scr_stride * scr_h);
    }
    /* DRM dumb buffer: we draw straight into the live scanout — nothing to flush */
}

/* uppercase helper for the tiny font */
static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static void draw_menu(int sel)
{
    if (disp_fd < 0) return;
    fb_rect(0, 0, scr_w, scr_h, 16, 16, 24);
    int y = scr_h / 8;
    fb_text(scr_w / 8, y, "PERIDOT MULTIBOOT"); y += 60;
    for (int i = 0; i < N_ENTRIES; i++) {
        char line[64];
        snprintf(line, sizeof(line), "%s %s", (i == sel) ? ">" : " ", entries[i].label);
        if (i == sel) fb_rect(scr_w / 8 - 12, y - 8, scr_w * 3 / 4, 40, 48, 48, 160);
        for (char *p = line; *p; p++) *p = up(*p);
        fb_text(scr_w / 8, y, line);
        y += 48;
    }
    fb_text(scr_w / 8, y + 40, "VOL:MOVE POWER:SELECT 5S:AUTO");
    disp_flush();
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

    if (disp_open() == 0) draw_menu(sel);
    sel = menu_loop(sel);
    draw_menu(sel);            /* final frame (DRM: live scanout keeps it) */
    disp_close();

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
