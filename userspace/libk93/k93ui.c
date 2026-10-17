/* k93ui.c - framebuffer + text-console UI toolkit (clean-room).
 * Design notes:
 *  - iOS 4.3.4 SpringBoard is a full OpenGL ES compositor. On this x86_64
 *    clean-room OS the "display" is either the Linux framebuffer (vesafb,
 *    drawn directly with an 8x16 bitmap font) or an ANSI text console that is
 *    also mirrored on the serial port, which is how the OS is validated
 *    headlessly under QEMU.
 *  - One foreground app at a time (frontmost-application model). */
#define _GNU_SOURCE
#include "k93ui.h"
#include "k93util.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <termios.h>
#include <sys/mman.h>
#include <poll.h>
#include <signal.h>
#include <linux/fb.h>
#ifndef __user
#define __user
#endif
#include <linux/drm.h>
#include <linux/drm_mode.h>
#ifndef DRM_MODE_CONNECTED           /* removed from v7.2 uapi (kernel enum) */
#define DRM_MODE_UNKNOWN    0
#define DRM_MODE_CONNECTED  1
#define DRM_MODE_DISCONNECTED 2
#endif
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

extern const uint8_t k93_font8x16[4096];

enum { BACK_NONE, BACK_FB, BACK_TEXT };

static int drm_ioctl(int fd, unsigned long req, void *arg);

static struct {
    int backend;
    int cols, rows;
    int fbfd;
    int drmfb;                           /* DRM fb id (KMS backend) */
    uint8_t *fbr;                        /* mmapped framebuffer (physical) */
    uint8_t *fbw;                        /* shadow buffer (logical) */
    size_t fblen, wlen;
    int fbline, fbbpp, fbw_px, fbh_px;   /* fbline = physical pitch;
                                            fbw_px/fbh_px = logical dims */
    int wline;                           /* shadow buffer line stride */
    int sx, sy;                          /* physical scanout dims */
    int rot90;                           /* 1 = portrait (rotate blit) */
    int outfd;                           /* text backend output */
    struct termios saved;
    int tty_raw;
    char appname[64];
} G = { .backend = BACK_NONE };

/* ------------------------------------------------------------------ input */

/* US PC-104 scancode -> character (shift table separate) */
static const char sc_map[128] = {
    [KEY_1]='1',[KEY_2]='2',[KEY_3]='3',[KEY_4]='4',[KEY_5]='5',[KEY_6]='6',
    [KEY_7]='7',[KEY_8]='8',[KEY_9]='9',[KEY_0]='0',
    [KEY_MINUS]='-',[KEY_EQUAL]='=',[KEY_Q]='q',[KEY_W]='w',[KEY_E]='e',
    [KEY_R]='r',[KEY_T]='t',[KEY_Y]='y',[KEY_U]='u',[KEY_I]='i',[KEY_O]='o',
    [KEY_P]='p',[KEY_LEFTBRACE]='[',[KEY_RIGHTBRACE]=']',[KEY_A]='a',
    [KEY_S]='s',[KEY_D]='d',[KEY_F]='f',[KEY_G]='g',[KEY_H]='h',[KEY_J]='j',
    [KEY_K]='k',[KEY_L]='l',[KEY_SEMICOLON]=';',[KEY_APOSTROPHE]='\'',
    [KEY_GRAVE]='`',[KEY_BACKSLASH]='\\',[KEY_Z]='z',[KEY_X]='x',[KEY_C]='c',
    [KEY_V]='v',[KEY_B]='b',[KEY_N]='n',[KEY_M]='m',[KEY_COMMA]=',',
    [KEY_DOT]='.',[KEY_SLASH]='/',[KEY_SPACE]=' '
};
static const char sc_map_shift[128] = {
    [KEY_1]='!',[KEY_2]='@',[KEY_3]='#',[KEY_4]='$',[KEY_5]='%',[KEY_6]='^',
    [KEY_7]='&',[KEY_8]='*',[KEY_9]='(',[KEY_0]=')',[KEY_MINUS]='_',
    [KEY_EQUAL]='+',[KEY_Q]='Q',[KEY_W]='W',[KEY_E]='E',[KEY_R]='R',[KEY_T]='T',
    [KEY_Y]='Y',[KEY_U]='U',[KEY_I]='I',[KEY_O]='O',[KEY_P]='P',
    [KEY_LEFTBRACE]='{',[KEY_RIGHTBRACE]='}',[KEY_A]='A',[KEY_S]='S',[KEY_D]='D',
    [KEY_F]='F',[KEY_G]='G',[KEY_H]='H',[KEY_J]='J',[KEY_K]='K',[KEY_L]='L',
    [KEY_SEMICOLON]=':',[KEY_APOSTROPHE]='"',[KEY_GRAVE]='~',
    [KEY_BACKSLASH]='|',[KEY_Z]='Z',[KEY_X]='X',[KEY_C]='C',[KEY_V]='V',
    [KEY_B]='B',[KEY_N]='N',[KEY_M]='M',[KEY_COMMA]='<',[KEY_DOT]='>',
    [KEY_SLASH]='?',[KEY_SPACE]=' '
};

static int evfd = -1;
static int shift_down = 0;

static void input_open_evdev(void)
{
    /* grab the first keyboard we can find */
    for (int i = 0; i < 16 && evfd < 0; i++) {
        char p[64];
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) continue;
        char name[256] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof name - 1), name) >= 0 &&
            (strcasestr(name, "keyboard") || strcasestr(name, "AT Translated") ||
             strcasestr(name, "QEMU") || strcasestr(name, "Virtual")))
            evfd = fd;
        else
            close(fd);
    }
}

static int key_from_evdev(void)
{
    struct input_event ie;
    if (evfd < 0) return -1;
    while (read(evfd, &ie, sizeof ie) == sizeof ie) {
        if (ie.type != EV_KEY) continue;
        if (ie.value == 0) {                       /* release */
            if (ie.code == KEY_LEFTSHIFT || ie.code == KEY_RIGHTSHIFT) shift_down = 0;
            continue;
        }
        if (ie.code == KEY_LEFTSHIFT || ie.code == KEY_RIGHTSHIFT) { shift_down = 1; continue; }
        switch (ie.code) {
        case KEY_UP: return UI_KEY_UP;
        case KEY_DOWN: return UI_KEY_DOWN;
        case KEY_LEFT: return UI_KEY_LEFT;
        case KEY_RIGHT: return UI_KEY_RIGHT;
        case KEY_PAGEUP: return UI_KEY_PGUP;
        case KEY_PAGEDOWN: return UI_KEY_PGDN;
        case KEY_HOME: return UI_KEY_HOME;
        case KEY_END: return UI_KEY_END;
        case KEY_DELETE: return UI_KEY_DELETE;
        case KEY_ENTER: case KEY_KPENTER: return UI_KEY_ENTER;
        case KEY_ESC: return UI_KEY_ESC;
        case KEY_BACKSPACE: return UI_KEY_BACKSPACE;
        case KEY_TAB: return UI_KEY_TAB;
        case KEY_F12: return UI_KEY_F12;
        default: break;
        }
        char c = shift_down ? sc_map_shift[ie.code] : sc_map[ie.code];
        if (c) return UI_KEY_CHAR_BASE + (unsigned char)c;
    }
    return -1;
}

/* console/tty byte parser with escape-sequence awareness */
static int parse_bytes(const unsigned char *b, size_t n, size_t *consumed)
{
    *consumed = 1;
    if (b[0] == 0x1b) {
        if (n >= 3 && b[1] == '[') {
            switch (b[2]) {
            case 'A': return UI_KEY_UP;
            case 'B': return UI_KEY_DOWN;
            case 'C': return UI_KEY_RIGHT;
            case 'D': return UI_KEY_LEFT;
            case 'H': return UI_KEY_HOME;
            case 'F': return UI_KEY_END;
            case '5': if (n >= 4 && b[3] == '~') { *consumed = 4; return UI_KEY_PGUP; } break;
            case '6': if (n >= 4 && b[3] == '~') { *consumed = 4; return UI_KEY_PGDN; } break;
            case '3': if (n >= 4 && b[3] == '~') { *consumed = 4; return UI_KEY_DELETE; } break;
            default: break;
            }
            *consumed = 3;
            return UI_KEY_NONE;
        }
        return UI_KEY_ESC;                         /* lone ESC */
    }
    switch (b[0]) {
    case '\r': case '\n': return UI_KEY_ENTER;
    case 127: case 8: return UI_KEY_BACKSPACE;
    case '\t': return UI_KEY_TAB;
    case 3: return UI_KEY_CTRL_C;
    default:
        if (b[0] >= 32 && b[0] < 127) return UI_KEY_CHAR_BASE + b[0];
        return UI_KEY_NONE;
    }
}

static int stdin_is_tty = 0;
static struct termios stdin_saved;
static int infd = 0;                   /* input fd: stdin, or the serial console when
                                        * we were spawned with /dev/null on stdin
                                        * (e.g. by launchd) */

static void stdin_raw(void)
{
    if (!isatty(infd)) return;
    struct termios t;
    if (tcgetattr(infd, &stdin_saved) == 0) {
        t = stdin_saved;
        t.c_lflag &= ~(ICANON | ECHO | ISIG);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(infd, TCSANOW, &t);
        stdin_is_tty = 1;
    }
}

static int key_from_stdin(int timeout_ms)
{
    struct pollfd p = { .fd = infd, .events = POLLIN };
    if (poll(&p, 1, timeout_ms) <= 0) return -1;
    unsigned char b[16];
    ssize_t n = read(infd, b, sizeof b);
    if (n <= 0) return -1;
    size_t used = 0;
    int k = parse_bytes(b, (size_t)n, &used);
    return k;
}

int ui_getkey(int timeout_ms)
{
    /* 1) evdev (graphical/VT use) */
    if (evfd >= 0) {
        struct pollfd p = { .fd = evfd, .events = POLLIN };
        if (poll(&p, 1, 0) > 0) {
            int k = key_from_evdev();
            if (k > 0) return k;
        }
    }
    /* 2) stdin (serial console in QEMU, interactive tty) */
    if (G.backend != BACK_NONE) {
        int k = key_from_stdin(timeout_ms);
        if (k > 0) return k;
    }
    if (timeout_ms > 0) usleep((useconds_t)timeout_ms * 1000);
    return UI_KEY_NONE;
}

/* ------------------------------------------------------------------ output */

static void fb_draw_glyph_col(int x, int y, char ch, int inverted)
{
    const uint8_t *g = k93_font8x16 + ((unsigned char)ch) * 16;
    int bpp = G.fbbpp / 8;
    for (int row = 0; row < 16; row++) {
        uint8_t bits = g[row];
        uint8_t *line = G.fbw + (size_t)(y * 16 + row) * (size_t)G.wline;
        for (int col = 0; col < 8; col++) {
            int on = (bits >> (7 - col)) & 1;
            if (inverted) on = !on;
            uint8_t *px = line + (size_t)(x * 8 + col) * (size_t)bpp;
            uint8_t v = on ? 0xFF : 0x00;
            if (bpp == 4) { px[0] = px[1] = px[2] = v; px[3] = 0xFF; }
            else if (bpp == 3) { px[0] = px[1] = px[2] = v; }
            else if (bpp == 2) {                       /* RGB565 */
                uint16_t c = on ? 0xFFFF : 0x0000;
                px[0] = (uint8_t)c; px[1] = (uint8_t)(c >> 8);
            }
        }
    }
}

/* display configuration: /etc/k93/display.conf (MODE=auto|<W>x<H>,
 * ORIENTATION=landscape|portrait), overridable per-boot from the kernel
 * command line (k93.mode= k93.orientation=) for tests. Landscape is the
 * default; orientation is static (no accelerometer/auto-rotate). */
static void display_conf(char *mode, size_t mlen, char *orient, size_t olen)
{
    k93_strlcpy(mode, "auto", mlen);
    k93_strlcpy(orient, "landscape", olen);
    size_t len = 0;
    char *c = k93_read_file("/etc/k93/display.conf", &len);
    if (c) {
        for (char *line = strtok(c, "\n"); line; line = strtok(NULL, "\n")) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            if (!strcmp(line, "MODE") && eq[1]) k93_strlcpy(mode, eq + 1, mlen);
            if (!strcmp(line, "ORIENTATION") && eq[1]) k93_strlcpy(orient, eq + 1, olen);
        }
        free(c);
    }
    /* kernel command line overrides (qemu-test) */
    size_t cl = 0;
    char *cmd = k93_read_file("/proc/cmdline", &cl);
    if (cmd) {
        for (char *tok = strtok(cmd, " \t\n"); tok; tok = strtok(NULL, " \t\n")) {
            if (!strncmp(tok, "k93.mode=", 9)) k93_strlcpy(mode, tok + 9, mlen);
            if (!strncmp(tok, "k93.orientation=", 16)) k93_strlcpy(orient, tok + 16, olen);
        }
        free(cmd);
    }
}

static void fb_render_all(void)
{
    if (G.backend != BACK_FB || !G.fbw) return;
    int bpp = G.fbbpp / 8;
    if (G.rot90) {
        /* portrait: rotate the logical buffer 90 degrees clockwise into the
         * landscape scanout: logical (x,y) -> physical (sx-1-y, x).
         * Static rotation; the monitor stays physically landscape. */
        for (int y = 0; y < G.fbh_px; y++) {
            const uint8_t *src = G.fbw + (size_t)y * (size_t)G.wline;
            uint8_t *dst = G.fbr + (size_t)(G.sx - 1 - y) * (size_t)bpp;
            for (int x = 0; x < G.fbw_px; x++) {
                memcpy(dst + (size_t)x * (size_t)G.fbline,
                       src + (size_t)x * (size_t)bpp, (size_t)bpp);
            }
        }
    } else {
        int rows = G.fbh_px < G.sy ? G.fbh_px : G.sy;
        int bytes = G.fbw_px * bpp < G.fbline ? G.fbw_px * bpp : G.fbline;
        for (int y = 0; y < rows; y++)
            memcpy(G.fbr + (size_t)y * (size_t)G.fbline,
                   G.fbw + (size_t)y * (size_t)G.wline, (size_t)bytes);
    }
    if (G.drmfb > 0 && G.fbfd >= 0) {
        /* KMS (DRM) scanout: drivers with shadow planes (bochs-drm) only
         * copy our mmap'd buffer to real VRAM on damage notification -
         * tell the kernel the whole screen changed after each flush. */
        struct drm_clip_rect clip = {
            0, 0, (uint16_t)G.sx, (uint16_t)G.sy,
        };
        struct drm_mode_fb_dirty_cmd d = {
            .fb_id = (uint32_t)G.drmfb,
            .num_clips = 1,
            .clips_ptr = (uintptr_t)&clip,
        };
        drm_ioctl(G.fbfd, DRM_IOCTL_MODE_DIRTYFB, &d);
    }
}

void ui_puts(int x, int y, const char *s)
{
    if (G.backend == BACK_FB) {
        for (int i = 0; s[i] && x + i < G.cols; i++)
            fb_draw_glyph_col(x + i, y, s[i], 0);
    } else if (G.backend == BACK_TEXT) {
        char line[512];
        int n = snprintf(line, sizeof line, "\033[%d;%dH%s", y + 1, x + 1, s);
        write(G.outfd, line, (size_t)n);
    }
}

void ui_puts_inv(int x, int y, const char *s)
{
    if (G.backend == BACK_FB) {
        for (int i = 0; s[i] && x + i < G.cols; i++)
            fb_draw_glyph_col(x + i, y, s[i], 1);
    } else if (G.backend == BACK_TEXT) {
        char line[512];
        int n = snprintf(line, sizeof line, "\033[%d;%dH\033[7m%s\033[0m", y + 1, x + 1, s);
        write(G.outfd, line, (size_t)n);
    }
}

void ui_printf(int x, int y, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ui_puts(x, y, buf);
}

void ui_hline(int y, int attr)
{
    char line[512];
    int n = G.cols < 511 ? G.cols : 511;
    memset(line, (attr ? ' ' : '-'), (size_t)n);
    line[n] = 0;
    ui_puts(0, y, line);
}

void ui_clear(void)
{
    if (G.backend == BACK_FB) {
        memset(G.fbw, 0, G.wlen);
    } else if (G.backend == BACK_TEXT) {
        const char *seq = "\033[2J\033[H\033[0m";
        write(G.outfd, seq, strlen(seq));
    }
}

void ui_flush(void)
{
    if (G.backend == BACK_FB) fb_render_all();
}

void ui_statusbar(const char *title)
{
    char left[256], right[128], mid[128] = "";
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof ts, "%H:%M", &tm);

    long cap = k93_sysinfo_long("/sys/class/power_supply/BAT0/capacity");
    if (cap < 0) cap = k93_sysinfo_long("/sys/class/power_supply/CMB0/capacity");
    char bat[32];
    if (cap >= 0) snprintf(bat, sizeof bat, "%ld%%", cap);
    else k93_strlcpy(bat, "AC", sizeof bat);

    if (title && title[0]) {
        char net[64] = "";
        long operstate = k93_sysinfo_long("/sys/class/net/eth0/operstate");
        if (operstate < 0) operstate = k93_sysinfo_long("/sys/class/net/enp0s3/operstate");
        if (operstate == 1) k93_strlcpy(net, " net:up", sizeof net);
        else k93_strlcpy(net, " net:down", sizeof net);
        snprintf(mid, sizeof mid, "  %s", G.appname);
        snprintf(right, sizeof right, "%s%s %s", net, net[0] ? "" : "", ts);
    } else {
        k93_strlcpy(right, ts, sizeof right);
    }
    char bar[512];
    if (title && title[0])
        snprintf(left, sizeof left, "%s", title);
    else
        snprintf(left, sizeof left, "%s", G.appname);
    snprintf(bar, sizeof bar, "%-*.s %s", 0, "", "");
    /* compose: left  mid  right(battery+clock) */
    char row[512];
    int width = G.cols;
    char rightfull[160];
    snprintf(rightfull, sizeof rightfull, "%s %s", bat, right);
    int rl = (int)strlen(rightfull);
    int ll = (int)strlen(left);
    int ml = (int)strlen(mid);
    if (ll + ml + rl + 3 > width) { ml = 0; }
    int pad1 = width - ll - rl - ml;
    if (pad1 < 1) pad1 = 1;
    snprintf(row, sizeof row, "%s%*s%s%s", left, pad1, "", mid, "");
    /* place right text at the end */
    char full[512];
    snprintf(full, sizeof full, "%-*s%s", width - rl - 1 < 0 ? 0 : width - rl - 1, row, rightfull);
    ui_puts_inv(0, 0, full);
    (void)bar;
    ui_flush();
}

void ui_statusmsg(const char *msg)
{
    char row[512];
    snprintf(row, sizeof row, "%-*.s", 0, "");
    char clean[512];
    snprintf(clean, sizeof clean, "%-*s", G.cols - 1 > 0 ? G.cols - 1 : 1, msg ? msg : "");
    ui_puts(0, G.rows - 1, clean);
    ui_flush();
}

int ui_width(void) { return G.cols; }
int ui_height(void) { return G.rows; }
int ui_is_graphical(void) { return G.backend == BACK_FB; }

/* ------------------------------------------------------------------ init */

static int fb_open(void)
{
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) return -1;
    struct fb_fix_screeninfo fi;
    struct fb_var_screeninfo vi;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fi) < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &vi) < 0) {
        close(fd);
        return -1;
    }
    if (vi.bits_per_pixel != 16 && vi.bits_per_pixel != 24 && vi.bits_per_pixel != 32) {
        close(fd);
        return -1;
    }
    size_t len = fi.smem_len ? fi.smem_len : (size_t)fi.line_length * vi.yres_virtual;
    uint8_t *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return -1; }
    G.fbr = m; G.fblen = len;
    G.fbw = k93_xmalloc(len);
    memset(G.fbw, 0, len);
    G.fbfd = fd;
    G.fbline = (int)fi.line_length;
    G.fbbpp = (int)vi.bits_per_pixel;
    G.fbw_px = (int)vi.xres;
    G.fbh_px = (int)vi.yres;
    G.sx = G.fbw_px; G.sy = G.fbh_px;      /* fbdev: landscape only */
    G.rot90 = 0;
    G.wline = G.fbline;
    G.wlen = len;
    G.cols = G.fbw_px / 8;
    G.rows = G.fbh_px / 16;
    G.backend = BACK_FB;
    return 0;
}

/* --- DRM/KMS backend -----------------------------------------------------
 * Modern kernels (e.g. Fedora's 7.2 config) ship CONFIG_FB_DEVICE off: the
 * fbdev core keeps framebuffers kernel-internal (fbcon only) and never
 * creates /dev/fb0. The userspace graphics path is then DRM/KMS: we become
 * DRM master, create a dumb buffer, add it as a framebuffer and scan it out
 * via SETCRTC. Render target is the mmap'd dumb buffer, so the rest of the
 * toolkit (shadow buffer + 8x16 font) is unchanged. */

static int drm_ioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

/* diagnostics go to the console device: when spawned by launchd our stderr
 * is /dev/null, so fprintf would vanish */
static void drm_dbg(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    int c = open("/dev/console", O_WRONLY | O_NOCTTY);
    if (c >= 0) { ssize_t w = write(c, line, strlen(line)); (void)w; close(c); }
}

static int drm_open(void)
{
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { drm_dbg("k93ui: open card0: %s\n", strerror(errno)); return -1; }

    struct drm_mode_card_res res;
    memset(&res, 0, sizeof res);
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 ||
        res.count_connectors == 0 || res.count_crtcs == 0) {
        drm_dbg("k93ui: getresources: %s (con=%u crtc=%u)\n",
                strerror(errno), res.count_connectors, res.count_crtcs);
        close(fd);
        return -1;
    }
    uint32_t con[32], crt[32], enc[32];
    if (res.count_connectors > 32 || res.count_crtcs > 32 || res.count_encoders > 32) {
        close(fd);
        return -1;
    }
    res.connector_id_ptr = (uintptr_t)con;
    res.crtc_id_ptr = (uintptr_t)crt;
    res.encoder_id_ptr = (uintptr_t)enc;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        drm_dbg("k93ui: getresources(2): %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* first connected connector with a mode list */
    uint32_t conn_id = 0, crtc_id = crt[0];
    struct drm_mode_modeinfo mode;
    memset(&mode, 0, sizeof mode);
    int got = 0;
    drm_dbg("k93ui: %u connectors, %u crtcs, first mode %dx%d\n",
            res.count_connectors, res.count_crtcs, mode.hdisplay, mode.vdisplay);
    for (uint32_t i = 0; i < res.count_connectors && !got; i++) {
        struct drm_mode_get_connector cc;
        memset(&cc, 0, sizeof cc);
        cc.connector_id = con[i];
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &cc) < 0) {
            drm_dbg("k93ui: getconnector %u: %s\n", con[i], strerror(errno));
            continue;
        }
        if (cc.count_modes == 0) {
            drm_dbg("k93ui: connector %u status=%u has no modes\n", con[i], cc.connection);
            continue;
        }
        struct drm_mode_modeinfo modes[32];
        uint32_t n = cc.count_modes < 32 ? cc.count_modes : 32;
        cc.modes_ptr = (uintptr_t)modes;
        cc.count_modes = n;
        cc.encoders_ptr = (uintptr_t)enc;       /* stale counts with NULL ptrs
                                                 * = kernel EFAULT */
        cc.count_encoders = res.count_encoders < 32 ? res.count_encoders : 32;
        cc.props_ptr = 0;
        cc.prop_values_ptr = 0;
        cc.count_props = 0;
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &cc) < 0) {
            drm_dbg("k93ui: getconnector-modes %u: %s\n", con[i], strerror(errno));
            continue;
        }
        mode = modes[0];                        /* first mode = preferred */
        /* requested mode from display.conf / cmdline: pick matching entry */
        char want[32], orient[32];
        display_conf(want, sizeof want, orient, sizeof orient);
        if (strstr(want, "x")) {
            int w = atoi(want), h = atoi(want + strcspn(want, "x") + 1);
            for (uint32_t m = 0; m < cc.count_modes && m < 32; m++)
                if ((int)modes[m].hdisplay == w && (int)modes[m].vdisplay == h) {
                    mode = modes[m];
                    break;
                }
        }
        conn_id = con[i];
        got = 1;
    }
    if (!got) {
        drm_dbg("k93ui: no connected connector with modes\n");
        close(fd);
        return -1;
    }

    /* render buffer: 32bpp dumb buffer in the scanout size */
    struct drm_mode_create_dumb cd = {
        .width = (uint32_t)mode.hdisplay,
        .height = (uint32_t)mode.vdisplay,
        .bpp = 32,
    };
    if (drm_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) { close(fd); return -1; }

    struct drm_mode_fb_cmd fb = {
        .width = cd.width, .height = cd.height,
        .pitch = cd.pitch, .bpp = 32, .depth = 24, .handle = cd.handle,
    };
    if (drm_ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) {
        drm_dbg("k93ui: addfb: %s\n", strerror(errno));
        struct drm_mode_destroy_dumb dd = { .handle = cd.handle };
        drm_ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
        close(fd);
        return -1;
    }

    struct drm_mode_map_dumb md = { .handle = cd.handle };
    if (drm_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
        drm_dbg("k93ui: mapdumb: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    uint8_t *m = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (m == MAP_FAILED) {
        drm_dbg("k93ui: mmap dumb: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    struct drm_mode_crtc crtc = {
        .set_connectors_ptr = (uintptr_t)&conn_id,
        .count_connectors = 1,
        .crtc_id = crtc_id,
        .fb_id = fb.fb_id,
    };
    crtc.mode_valid = 1;
    memcpy(&crtc.mode, &mode, sizeof mode);
    if (drm_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0) {
        drm_dbg("k93ui: setcrtc: %s\n", strerror(errno));
        munmap(m, cd.size);
        close(fd);
        return -1;
    }
    drm_dbg("k93ui: DRM/KMS scanout %dx%d pitch=%u on crtc %u\n",
            cd.width, cd.height, cd.pitch, crtc_id);
    G.drmfb = fb.fb_id;

    char orient[32], mode_req[32];
    display_conf(mode_req, sizeof mode_req, orient, sizeof orient);
    G.rot90 = !strcmp(orient, "portrait");
    G.sx = (int)cd.width;                  /* physical scanout */
    G.sy = (int)cd.height;
    if (G.rot90) { G.fbw_px = G.sy; G.fbh_px = G.sx; }   /* logical canvas */
    else         { G.fbw_px = G.sx; G.fbh_px = G.sy; }
    G.fbr = m; G.fblen = cd.size;
    G.wline = G.fbw_px * 4;                /* shadow stride = logical width */
    G.wlen = (size_t)G.wline * (size_t)G.fbh_px;
    G.fbw = k93_xmalloc(G.wlen);
    memset(G.fbw, 0, G.wlen);
    G.fbfd = fd;
    G.fbline = (int)cd.pitch;
    G.fbbpp = 32;
    G.cols = G.fbw_px / 8;
    G.rows = G.fbh_px / 16;
    drm_dbg("k93ui: canvas %dx%d orientation=%s\n",
            G.fbw_px, G.fbh_px, G.rot90 ? "portrait" : "landscape");
    G.backend = BACK_FB;
    return 0;
}

/* list the current connector's modes as "WxH" strings for the Settings
 * Display pane (independent of ui_init - Settings may call this any time) */
int ui_display_modes(char out[][32], int max)
{
    if (max <= 0) return 0;
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return 0;
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof res);
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 || !res.count_connectors) {
        close(fd);
        return 0;
    }
    uint32_t con[32], crt[32], enc[32];
    uint32_t nc = res.count_connectors < 32 ? res.count_connectors : 32;
    res.connector_id_ptr = (uintptr_t)con;
    res.crtc_id_ptr = (uintptr_t)crt;
    res.encoder_id_ptr = (uintptr_t)enc;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) { close(fd); return 0; }
    int n = 0;
    for (uint32_t i = 0; i < nc && n < max; i++) {
        struct drm_mode_get_connector cc;
        memset(&cc, 0, sizeof cc);
        cc.connector_id = con[i];
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &cc) < 0 || !cc.count_modes)
            continue;
        struct drm_mode_modeinfo modes[32];
        uint32_t nm = cc.count_modes < 32 ? cc.count_modes : 32;
        cc.modes_ptr = (uintptr_t)modes;
        cc.count_modes = nm;
        cc.encoders_ptr = (uintptr_t)enc;
        cc.count_encoders = res.count_encoders < 32 ? res.count_encoders : 32;
        cc.props_ptr = 0;
        cc.prop_values_ptr = 0;
        cc.count_props = 0;
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &cc) < 0) continue;
        for (uint32_t m = 0; m < nm && n < max; m++) {
            snprintf(out[n], 32, "%dx%d", modes[m].hdisplay, modes[m].vdisplay);
            /* de-duplicate refresh variants (mode lists repeat WxH) */
            int dup = 0;
            for (int k = 0; k < n; k++)
                if (!strcmp(out[k], out[n])) { dup = 1; break; }
            if (!dup) n++;
        }
        if (n) break;
    }
    close(fd);
    return n;
}

static int open_console(void)
{
    /* /dev/console follows the kernel's console= setting (serial when
     * console=ttyS0), so try it before the VTs; serial-driven validation
     * is the primary interaction path for this OS. */
    const char *cands[] = { "/dev/console", "/dev/ttyS0", "/dev/tty1", NULL };
    const char *envc = getenv("K93_CONSOLE");
    if (envc) {
        int fd = open(envc, O_WRONLY | O_NOCTTY);
        if (fd >= 0) return fd;
    }
    for (int i = 0; cands[i]; i++) {
        int fd = open(cands[i], O_WRONLY | O_NOCTTY);
        if (fd >= 0) return fd;
    }
    return -1;
}

int ui_init(const char *appname)
{
    signal(SIGPIPE, SIG_IGN);
    k93_strlcpy(G.appname, appname ? appname : "K93OS", sizeof G.appname);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* serial-first: if we were started with a controlling serial console, keep
     * using stdin/stdout so QEMU's captured serial shows the UI. */
    int have_fb = 0;
    if (!k93_cmdline_flag("notui_fb")) {
        have_fb = (fb_open() == 0);             /* classic fbdev, if the
                                                 * kernel exposes /dev/fb0 */
        if (!have_fb) have_fb = (drm_open() == 0); /* DRM/KMS dumb buffer */
    }

    if (!have_fb) {
        G.backend = BACK_TEXT;
        G.cols = 80; G.rows = 25;
        struct winsize ws;
        if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 20 && ws.ws_row >= 8) {
            G.cols = ws.ws_col; G.rows = ws.ws_row;
        } else {
            int cfd = open("/dev/ttyS0", O_RDWR | O_NOCTTY);
            if (cfd >= 0 && ioctl(cfd, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 20) {
                G.cols = ws.ws_col; G.rows = ws.ws_row;
            }
            if (cfd >= 0) close(cfd);
        }
        G.outfd = open_console();
        if (G.outfd < 0) G.outfd = 1;
        if (!isatty(0)) {
            /* spawned detached (launchd): take input from the console device */
            const char *c = getenv("K93_CONSOLE");
            if (!c) c = "/dev/ttyS0";
            infd = open(c, O_RDWR | O_NOCTTY);
            if (infd < 0) infd = 0;
        }
    }
    stdin_raw();
    input_open_evdev();
    ui_clear();
    return 0;
}

void ui_shutdown(void)
{
    if (stdin_is_tty) tcsetattr(infd, TCSANOW, &stdin_saved);
    if (G.backend == BACK_TEXT && isatty(G.outfd)) {
        const char *seq = "\033[0m\033[2J\033[H";
        write(G.outfd, seq, strlen(seq));
    }
    if (G.fbw) { free(G.fbw); G.fbw = NULL; }
    if (G.fbr) { munmap(G.fbr, G.fblen); G.fbr = NULL; }
    if (G.fbfd > 0) { close(G.fbfd); G.fbfd = -1; }
    if (infd > 2 && infd != G.outfd) close(infd);
    infd = 0;
    if (G.outfd > 2) close(G.outfd);
    G.backend = BACK_NONE;
}

/* ---------------------------------------------------------------- widgets */

int ui_menu(const char *title, const char *const *items, int n, int sel)
{
    if (sel < 0) sel = 0;
    if (n <= 0) return -1;
    int top = 0;
    int view = G.rows - 4;
    if (view < 1) view = 1;
    for (;;) {
        ui_clear();
        ui_statusbar(title);
        for (int i = 0; i < view && top + i < n; i++) {
            int idx = top + i;
            char row[512];
            snprintf(row, sizeof row, " %c %-.499s", idx == sel ? '>' : ' ', items[idx]);
            int y = 2 + i;
            if (idx == sel) ui_puts_inv(0, y, row);
            else ui_puts(0, y, row);
        }
        if (top > 0) ui_printf(G.cols - 4, 2, "[^]");
        if (top + view < n) ui_printf(G.cols - 4, G.rows - 2, "[v]");
        ui_statusmsg("Up/Down select  Enter open  Esc back");
        ui_flush();
        int k = ui_getkey(250);
        switch (k) {
        case UI_KEY_UP: case UI_KEY_CHAR_BASE + 'k':
            if (sel > 0) sel--;
            break;
        case UI_KEY_DOWN: case UI_KEY_CHAR_BASE + 'j':
            if (sel < n - 1) sel++;
            break;
        case UI_KEY_PGUP: sel -= view; if (sel < 0) sel = 0; break;
        case UI_KEY_PGDN: sel += view; if (sel > n - 1) sel = n - 1; break;
        case UI_KEY_HOME: sel = 0; break;
        case UI_KEY_END: sel = n - 1; break;
        case UI_KEY_ENTER: return sel;
        case UI_KEY_ESC: case UI_KEY_CTRL_C: case UI_KEY_BACKSPACE: return -1;
        default: break;
        }
        if (sel < top) top = sel;
        if (sel >= top + view) top = sel - view + 1;
    }
}

void ui_msgbox(const char *title, const char *body)
{
    ui_clear();
    ui_statusbar(title);
    int y = 2;
    char tmp[512];
    k93_strlcpy(tmp, body ? body : "", sizeof tmp);
    char *save = NULL;
    for (char *line = strtok_r(tmp, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (y >= G.rows - 2) break;
        ui_puts(2, y++, line);
    }
    ui_statusmsg("Press any key");
    ui_flush();
    while (ui_getkey(250) == UI_KEY_NONE) ;
}

int ui_confirm(const char *title, const char *body)
{
    const char *items[] = { "Cancel", "Yes, continue" };
    int r = ui_menu(title, items, 2, 0);
    (void)body;
    return r == 1;
}

int ui_input(const char *prompt, char *buf, size_t cap)
{
    size_t len = strlen(buf);
    size_t cur = len;
    for (;;) {
        ui_clear();
        ui_statusbar(prompt);
        ui_puts(0, 2, "> ");
        char line[512];
        snprintf(line, sizeof line, "%.480s", buf);
        ui_puts(2, 2, line);
        char cursor[64];
        snprintf(cursor, sizeof cursor, "^");
        ui_puts(2 + (int)cur, 3, cursor);
        ui_statusmsg("Enter accept  Esc cancel  (chars to type)");
        ui_flush();
        int k = ui_getkey(250);
        if (k == UI_KEY_ENTER) return 0;
        if (k == UI_KEY_ESC || k == UI_KEY_CTRL_C) return 1;
        if (k == UI_KEY_BACKSPACE) {
            if (cur > 0) { memmove(buf + cur - 1, buf + cur, len - cur + 1); cur--; len--; }
        } else if (k == UI_KEY_DELETE) {
            if (cur < len) { memmove(buf + cur, buf + cur + 1, len - cur); len--; }
        } else if (k == UI_KEY_LEFT) { if (cur > 0) cur--; }
        else if (k == UI_KEY_RIGHT) { if (cur < len) cur++; }
        else if (k == UI_KEY_HOME) cur = 0;
        else if (k == UI_KEY_END) cur = len;
        else if (k >= UI_KEY_CHAR_BASE && k < UI_KEY_CHAR_BASE + 128 && len + 1 < cap) {
            memmove(buf + cur + 1, buf + cur, len - cur + 1);
            buf[cur] = (char)(k - UI_KEY_CHAR_BASE);
            cur++; len++;
        }
    }
}

static void textview_render(const char *title, char **lines, int nlines, int top, int sel)
{
    ui_clear();
    ui_statusbar(title);
    int view = G.rows - 4;
    if (view < 1) view = 1;
    for (int i = 0; i < view && top + i < nlines; i++) {
        char row[512];
        snprintf(row, sizeof row, "%.500s", lines[top + i]);
        if (top + i == sel) ui_puts_inv(0, 2 + i, row);
        else ui_puts(0, 2 + i, row);
    }
    ui_statusmsg("Up/Dn scroll  PgUp/PgDn  Esc close");
    ui_flush();
}

int ui_textview(const char *title, const char *text)
{
    size_t len = text ? strlen(text) : 0;
    char *copy = k93_xmalloc(len + 2);
    memcpy(copy, text ? text : "", len);
    copy[len] = '\n';
    copy[len + 1] = 0;
    int n = 1;
    for (size_t i = 0; i < len; i++) if (copy[i] == '\n') n++;
    char **lines = k93_xmalloc(sizeof(char *) * (size_t)n);
    int idx = 0;
    char *p = copy;
    while (*p && idx < n) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        lines[idx++] = p;
        if (!nl) break;
        p = nl + 1;
    }
    n = idx;
    int top = 0, sel = 0;
    for (;;) {
        textview_render(title, lines, n, top, sel);
        int k = ui_getkey(250);
        int view = G.rows - 4; if (view < 1) view = 1;
        switch (k) {
        case UI_KEY_UP: if (sel > 0) sel--; break;
        case UI_KEY_DOWN: if (sel < n - 1) sel++; break;
        case UI_KEY_PGUP: sel -= view; if (sel < 0) sel = 0; break;
        case UI_KEY_PGDN: sel += view; if (sel > n - 1) sel = n - 1; break;
        case UI_KEY_ESC: case UI_KEY_ENTER: case UI_KEY_CTRL_C:
            free(lines); free(copy); return 0;
        default: break;
        }
        if (sel < top) top = sel;
        if (sel >= top + view) top = sel - view + 1;
    }
}

/* multiline editor: buffer of lines, char insertion on current line */
int ui_editbox(const char *title, char *buf, size_t cap)
{
    int cap_lines = 64;
    char **lines = k93_xmalloc(sizeof(char *) * (size_t)cap_lines);
    int nlines = 0;
    {
        char *copy = strdup(buf);
        char *p = copy;
        while (nlines < cap_lines) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = 0;
            lines[nlines++] = strdup(p);
            if (!nl) break;
            p = nl + 1;
        }
        free(copy);
    }
    if (nlines == 0) lines[nlines++] = strdup("");
    int cur_line = 0, cur_col = 0, top = 0;
    for (;;) {
        int view = G.rows - 4; if (view < 1) view = 1;
        ui_clear();
        ui_statusbar(title);
        for (int i = 0; i < view && top + i < nlines; i++) {
            int li = top + i;
            char row[512];
            snprintf(row, sizeof row, "%.500s", lines[li]);
            if (li == cur_line) {
                ui_puts(0, 2 + i, row);
                ui_puts_inv(cur_col, 2 + i, " ");
            } else {
                ui_puts(0, 2 + i, row);
            }
        }
        ui_statusmsg("Enter=newline  Esc=save+close  (type to edit)");
        ui_flush();
        int k = ui_getkey(250);
        char *cl = lines[cur_line];
        int cl_len = (int)strlen(cl);
        if (k == UI_KEY_ESC || k == UI_KEY_CTRL_C) break;
        else if (k == UI_KEY_ENTER) {
            if (nlines >= cap_lines) continue;
            char *nw = k93_xmalloc((size_t)cl_len - cur_col + 2);
            k93_strlcpy(nw, cl + cur_col, (size_t)(cl_len - cur_col + 1));
            cl[cur_col] = 0;
            for (int i = nlines; i > cur_line + 1; i--) lines[i] = lines[i - 1];
            lines[cur_line + 1] = nw;
            nlines++;
            cur_line++;
            cur_col = 0;
        } else if (k == UI_KEY_BACKSPACE) {
            if (cur_col > 0) {
                memmove(cl + cur_col - 1, cl + cur_col, (size_t)(cl_len - cur_col) + 1);
                cur_col--;
            } else if (cur_line > 0) {
                char *prev = lines[cur_line - 1];
                int plen = (int)strlen(prev);
                char *merged = k93_xmalloc((size_t)(plen + cl_len + 1));
                memcpy(merged, prev, (size_t)plen);
                memcpy(merged + plen, cl, (size_t)cl_len + 1);
                free(prev); free(cl);
                lines[cur_line - 1] = merged;
                for (int i = cur_line; i < nlines - 1; i++) lines[i] = lines[i + 1];
                nlines--;
                cur_line--;
                cur_col = plen;
            }
        } else if (k == UI_KEY_DELETE) {
            if (cur_col < cl_len) {
                memmove(cl + cur_col, cl + cur_col + 1, (size_t)(cl_len - cur_col));
            } else if (cur_line + 1 < nlines) {
                char *nx = lines[cur_line + 1];
                int xl = (int)strlen(nx);
                char *merged = k93_xmalloc((size_t)(cl_len + xl + 1));
                memcpy(merged, cl, (size_t)cl_len);
                memcpy(merged + cl_len, nx, (size_t)xl + 1);
                free(cl); free(nx);
                lines[cur_line] = merged;
                for (int i = cur_line + 1; i < nlines - 1; i++) lines[i] = lines[i + 1];
                nlines--;
            }
        } else if (k == UI_KEY_LEFT) { if (cur_col > 0) cur_col--; else if (cur_line > 0) { cur_line--; cur_col = (int)strlen(lines[cur_line]); } }
        else if (k == UI_KEY_RIGHT) { if (cur_col < cl_len) cur_col++; else if (cur_line + 1 < nlines) { cur_line++; cur_col = 0; } }
        else if (k == UI_KEY_UP) { if (cur_line > 0) { cur_line--; int l = (int)strlen(lines[cur_line]); if (cur_col > l) cur_col = l; } }
        else if (k == UI_KEY_DOWN) { if (cur_line + 1 < nlines) { cur_line++; int l = (int)strlen(lines[cur_line]); if (cur_col > l) cur_col = l; } }
        else if (k >= UI_KEY_CHAR_BASE && k < UI_KEY_CHAR_BASE + 128) {
            if (cl_len + 2 < 500) {
                memmove(cl + cur_col + 1, cl + cur_col, (size_t)(cl_len - cur_col) + 1);
                cl[cur_col] = (char)(k - UI_KEY_CHAR_BASE);
                cur_col++;
            }
        }
        if (cur_line < top) top = cur_line;
        if (cur_line >= top + view) top = cur_line - view + 1;
    }
    /* serialize back */
    size_t o = 0;
    buf[0] = 0;
    for (int i = 0; i < nlines; i++) {
        size_t l = strlen(lines[i]);
        if (o + l + 2 >= cap) break;
        memcpy(buf + o, lines[i], l);
        o += l;
        if (i + 1 < nlines) buf[o++] = '\n';
        buf[o] = 0;
    }
    for (int i = 0; i < nlines; i++) free(lines[i]);
    free(lines);
    return 0;
}
