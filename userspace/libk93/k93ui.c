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
#include <linux/kd.h>
#ifndef __user
#define __user
#endif
#include <linux/drm.h>
#include <linux/drm_mode.h>

/* fourcc constants not exposed via linux/drm_mode.h (drm_fourcc.h not pulled) */
#ifndef DRM_FORMAT_XRGB8888
#define DRM_FORMAT_XRGB8888 ((uint32_t)('X' | ('R' << 8) | ('2' << 16) | ('4' << 24)))
#endif
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR ((uint64_t)0)
#endif
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
static int kms_flip(void);
static void cursor_stamp(int ox, int oy, int draw);

static struct {
    int backend;
    int cols, rows;
    int fbfd;
    int drmfb;                           /* DRM fb id (KMS backend) */
    uint32_t planeid;                    /* primary plane id (KMS backend) */
    uint32_t drmcrtc, drmconn;           /* adopted scanout crtc/connector */
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

/* ---- mouse + super/Windows key ---------------------------------------- */
static int mfd[8], nmfd;                /* relative mice (evdev), summed */
static int m_x = -1, m_y = -1;          /* cursor px; -1 = never moved */
static int m_click[3];                  /* pending presses: 0=left 1=right 2=mid */
static int m_wheel = 0;                 /* pending lines: +down / -up */
static int meta_held = 0, meta_fired = 0;
static struct timespec meta_t0;         /* super-key press time */
static int pending_key = -1;            /* key synthesized by policy, queued */

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

static void input_open_mouse(void)
{
    /* Open EVERY relative-motion device that is not the keyboard: a plain
     * PS/2 two/three-button mouse and a USB HID 3-button+wheel mouse can
     * coexist; deltas from all of them are summed in mouse_pump().
     * ponytail: absolute pointers (USB tablet/trackpad, ABS_X/ABS_Y) are
     * not opened; add EV_ABS handling when such hardware matters. */
    for (int i = 0; i < 16 && nmfd < (int)(sizeof mfd / sizeof mfd[0]); i++) {
        char p[64];
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) continue;
        struct stat st1, st2;
        if (evfd >= 0 && fstat(fd, &st1) == 0 && fstat(evfd, &st2) == 0 &&
            st1.st_rdev == st2.st_rdev) { close(fd); continue; }
        unsigned long rel = 0;
        if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), &rel) < 0 ||
            !(rel & (1UL << REL_X))) { close(fd); continue; }
        mfd[nmfd++] = fd;
    }
}

/* Cursor position survives process switches (app launch/re-launch, shell
 * re-init) so the pointer does not teleport back to screen center.
 * ponytail: RAM state only within one process; this small file bridges
 * processes. Replace with a shared-memory page if writes ever matter. */
static void cursor_state_write(void)
{
    if (m_x < 0) return;
    FILE *f = fopen("/run/k93.cursor", "w");
    if (!f) f = fopen("/tmp/k93.cursor", "w");
    if (!f) return;
    fprintf(f, "%d %d\n", m_x, m_y);
    fclose(f);
}

static void cursor_state_read(void)
{
    FILE *f = fopen("/run/k93.cursor", "r");
    if (!f) f = fopen("/tmp/k93.cursor", "r");
    if (!f) return;
    int x, y;
    if (fscanf(f, "%d %d", &x, &y) == 2 && x >= 0 && y >= 0 &&
        x < G.fbw_px && y < G.fbh_px) {
        m_x = x;
        m_y = y;
    }
    fclose(f);
}

static void mouse_pump(void)
{
    int dx = 0, dy = 0, moved = 0;
    struct input_event ie;
    for (int f = 0; f < nmfd; f++) {
        while (read(mfd[f], &ie, sizeof ie) == sizeof ie) {
            if (ie.type == EV_REL) {
                if (ie.code == REL_X) { dx += ie.value; moved = 1; }
                else if (ie.code == REL_Y) { dy += ie.value; moved = 1; }
                else if (ie.code == REL_WHEEL) m_wheel -= (int)ie.value;
            } else if (ie.type == EV_KEY) {
                int b = ie.code == BTN_LEFT ? 0 : ie.code == BTN_RIGHT ? 1 :
                        ie.code == BTN_MIDDLE ? 2 : -1;
                if (b >= 0 && ie.value == 1 && m_click[b] < 8) m_click[b]++;
            }
        }
    }
    if (!moved) return;
    int ox = m_x, oy = m_y;
    if (m_x < 0) {                       /* first motion: spawn at center */
        ox = m_x = G.fbw_px / 2;
        oy = m_y = G.fbh_px / 2;
    }
    m_x += dx; m_y += dy;
    if (m_x < 0) m_x = 0;
    if (m_y < 0) m_y = 0;
    if (m_x >= G.fbw_px) m_x = G.fbw_px - 1;
    if (m_y >= G.fbh_px) m_y = G.fbh_px - 1;
    if (m_x == ox && m_y == oy) return;
    /* live motion: erase the old stamp and draw the new one straight into
     * the scanout so the cursor tracks without waiting for an app redraw */
    if (G.backend == BACK_FB) {
        cursor_stamp(ox, oy, 0);
        cursor_stamp(m_x, m_y, 1);
        if (G.drmfb > 0 && G.fbfd >= 0) kms_flip();
        cursor_state_write();
    }
}

/* Super/Windows key tap and middle click = "go home". SpringBoard sets
 * K93_NO_SUPER_EXIT so the shell itself survives and gets a synthetic
 * UI_KEY_HOME instead; a frontmost app quits back to the home screen.
 * Exit status 2 tells SpringBoard to open the multitasking view. */
static void super_home(void)
{
    if (getenv("K93_NO_SUPER_EXIT")) { pending_key = UI_KEY_HOME; return; }
    ui_shutdown();
    exit(0);
}

static void super_switcher(void)
{
    if (getenv("K93_NO_SUPER_EXIT")) { pending_key = UI_KEY_SWITCHER; return; }
    ui_shutdown();
    exit(2);
}

static void super_pump(void)
{
    if (!meta_held || meta_fired) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - meta_t0.tv_sec) * 1000L +
              (now.tv_nsec - meta_t0.tv_nsec) / 1000000L;
    if (ms >= 5000) { meta_fired = 1; super_switcher(); }
}

static int key_from_evdev(void)
{
    struct input_event ie;
    if (evfd < 0) return -1;
    while (read(evfd, &ie, sizeof ie) == sizeof ie) {
        if (ie.type != EV_KEY) continue;
        if (ie.code == KEY_LEFTMETA || ie.code == KEY_RIGHTMETA) {
            if (ie.value == 1) {
                meta_held = 1; meta_fired = 0;
                clock_gettime(CLOCK_MONOTONIC, &meta_t0);
            } else if (ie.value == 0 && meta_held) {
                meta_held = 0;
                if (!meta_fired) super_home();   /* tap: home */
            }
            continue;
        }
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
    if (pending_key >= 0) { int k = pending_key; pending_key = -1; return k; }
    /* 1) evdev (graphical/VT use); super-key policy runs inside */
    if (evfd >= 0) {
        struct pollfd p = { .fd = evfd, .events = POLLIN };
        if (poll(&p, 1, 0) > 0) {
            int k = key_from_evdev();
            if (k > 0) return k;
            if (pending_key >= 0) { k = pending_key; pending_key = -1; return k; }
        }
    }
    /* 2) mouse: pump motion/buttons, fire the super-key hold timer, then
     * synthesize clicks/wheel for widgets that do not hit-test themselves */
    mouse_pump();
    super_pump();
    if (m_click[2]) { m_click[2] = 0; super_home(); }   /* middle = home */
    if (m_click[0]) { m_click[0]--; return UI_KEY_ENTER; }
    if (m_click[1]) { m_click[1]--; return UI_KEY_ESC; }
    if (m_wheel) {
        int s = m_wheel > 0 ? 1 : -1;
        m_wheel -= s;
        return s > 0 ? UI_KEY_DOWN : UI_KEY_UP;
    }
    if (pending_key >= 0) { int k = pending_key; pending_key = -1; return k; }
    /* 3) stdin (serial console in QEMU, interactive tty) */
    if (G.backend != BACK_NONE) {
        int k = key_from_stdin(timeout_ms);
        if (k > 0) return k;
    }
    if (timeout_ms > 0) usleep((useconds_t)timeout_ms * 1000);
    return UI_KEY_NONE;
}

/* ---- mouse state for widgets that hit-test (pageview, SpringBoard grid) */
int ui_mouse_x(void) { mouse_pump(); return m_x; }
int ui_mouse_y(void) { mouse_pump(); return m_y; }
int ui_mouse_visible(void) { return m_x >= 0; }
int ui_mouse_clicked(int btn)
{
    mouse_pump();
    if ((unsigned)btn > 2) return 0;
    int n = m_click[btn];
    m_click[btn] = 0;
    return n > 0;
}
int ui_mouse_scroll(void)
{
    mouse_pump();
    int s = m_wheel;
    m_wheel = 0;
    return s;
}

/* ------------------------------------------------------------------ output */

/* --- pixel primitives (graphical backend; text console no-ops) --------- */

/* per-row page color: the app chrome background under text. ui_clear fills
 * with this gradient and ui_puts paints each glyph cell with it, so normal
 * text still erases what was under it while sitting on a shaded page. */
static uint32_t page_row_color(int y)
{
    uint32_t t = ui_rgb(0x0f, 0x13, 0x1b), b = ui_rgb(0x1b, 0x23, 0x31);
    int h = G.fbh_px > 1 ? G.fbh_px : 1;
    int d = h - 1 > 0 ? h - 1 : 1;
    int p = y < 0 ? 0 : (y >= h ? h - 1 : y);
    int r = (int)((t >> 16) & 255) + ((int)((b >> 16) & 255) - (int)((t >> 16) & 255)) * p / d;
    int g = (int)((t >> 8) & 255) + ((int)((b >> 8) & 255) - (int)((t >> 8) & 255)) * p / d;
    int bl = (int)(t & 255) + ((int)(b & 255) - (int)(t & 255)) * p / d;
    return ui_rgb((uint8_t)r, (uint8_t)g, (uint8_t)bl);
}

uint32_t ui_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

uint32_t ui_tint(uint32_t c, int num, int den)
{
    int r = (int)((c >> 16) & 255) * num / den;
    int g = (int)((c >> 8) & 255) * num / den;
    int b = (int)(c & 255) * num / den;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    return ui_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

static inline int px_ok(int x, int y)
{
    return (unsigned)x < (unsigned)G.fbw_px && (unsigned)y < (unsigned)G.fbh_px;
}

static inline void px_put(int x, int y, uint32_t c)   /* caller clips */
{
    uint8_t *p = G.fbw + (size_t)y * (size_t)G.wline + (size_t)x * (size_t)(G.fbbpp / 8);
    if (G.fbbpp == 32) { p[0] = (uint8_t)c; p[1] = (uint8_t)(c >> 8); p[2] = (uint8_t)(c >> 16); p[3] = 0xFF; }
    else if (G.fbbpp == 24) { p[0] = (uint8_t)c; p[1] = (uint8_t)(c >> 8); p[2] = (uint8_t)(c >> 16); }
    else {                              /* RGB565 */
        uint16_t v = (uint16_t)(((c >> 3) & 0x1f) << 11 | ((c >> 2) & 0x3f) << 5 | ((c >> 3) & 0x1f));
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    }
}

static inline uint32_t px_get(int x, int y)
{
    const uint8_t *p = G.fbw + (size_t)y * (size_t)G.wline + (size_t)x * (size_t)(G.fbbpp / 8);
    if (G.fbbpp == 32) return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
    if (G.fbbpp == 24) return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
    uint16_t v = (uint16_t)(p[0] | (uint16_t)p[1] << 8);
    return ui_rgb((uint8_t)((v >> 11) << 3), (uint8_t)((v >> 5) << 2), (uint8_t)(v << 3));
}

/* fill a rect clipped to the logical canvas (any backend-safe; no-op off-FB) */
static void fill_rect(int x, int y, int w, int h, uint32_t c)
{
    if (G.backend != BACK_FB || w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > G.fbw_px ? G.fbw_px : x + w;
    int y1 = y + h > G.fbh_px ? G.fbh_px : y + h;
    for (int yy = y0; yy < y1; yy++) {
        uint8_t *line = G.fbw + (size_t)yy * (size_t)G.wline;
        if (G.fbbpp == 32) {
            uint32_t *row = (uint32_t *)(line + (size_t)x0 * 4);
            for (int xx = x0; xx < x1; xx++) row[xx - x0] = c;
        } else {
            for (int xx = x0; xx < x1; xx++) px_put(xx, yy, c);
        }
    }
}

static int isqrt(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

/* linear per-channel mix of two colors, p/d of the way from a to b */
static uint32_t mix(uint32_t a, uint32_t b, int p, int d)
{
    if (d <= 0) return a;
    if (p < 0) p = 0;
    if (p > d) p = d;
    int r = (int)((a >> 16) & 255) + ((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * p / d;
    int g = (int)((a >> 8) & 255) + ((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * p / d;
    int bl = (int)(a & 255) + ((int)(b & 255) - (int)(a & 255)) * p / d;
    return ui_rgb((uint8_t)r, (uint8_t)g, (uint8_t)bl);
}

/* rounded-rect corner x-inset for row j within a corner zone of radius r */
static int rr_inset(int j, int r)
{
    if (j >= r) return 0;
    int dy = r - j - 1;
    return r - isqrt(r * r - dy * dy);
}

void ui_fill(int x, int y, int w, int h, uint32_t rgb)
{
    fill_rect(x, y, w, h, rgb);
}

void ui_fill_grad(int x, int y, int w, int h, uint32_t top, uint32_t bot)
{
    if (G.backend != BACK_FB || w <= 0 || h <= 0) return;
    for (int i = 0; i < h; i++) {
        uint32_t c = h > 1 ? mix(top, bot, i, h - 1) : top;
        fill_rect(x, y + i, w, 1, c);
    }
}

void ui_roundrect(int x, int y, int w, int h, int r, uint32_t top, uint32_t bot)
{
    if (G.backend != BACK_FB || w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 0) { ui_fill_grad(x, y, w, h, top, bot); return; }
    for (int j = 0; j < h; j++) {
        int jj = h - 1 - j;
        int a = rr_inset(j, r), b = rr_inset(jj, r);
        int in = a > b ? a : b;
        if (in * 2 >= w) continue;
        uint32_t c = h > 1 ? mix(top, bot, j, h - 1) : top;
        fill_rect(x + in, y + j, w - 2 * in, 1, c);
    }
}

void ui_frame(int x, int y, int w, int h, int r, int thick, uint32_t c)
{
    if (G.backend != BACK_FB || w <= 0 || h <= 0 || thick <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 0) {
        fill_rect(x, y, w, thick, c);
        fill_rect(x, y + h - thick, w, thick, c);
        fill_rect(x, y, thick, h, c);
        fill_rect(x + w - thick, y, thick, h, c);
        return;
    }
    int ri = r - thick;
    if (ri < 0) ri = 0;
    for (int j = 0; j < h; j++) {
        int jj = h - 1 - j;
        int o = rr_inset(j, r);
        int ob = rr_inset(jj, r);
        if (o < ob) o = ob;
        int ii = rr_inset(j, ri), ib = rr_inset(jj, ri);
        int in = ii > ib ? ii : ib;
        int L = x + o, R = x + w - o;
        if (R - L <= 0) continue;
        if (in > o) {   /* fully inside corner band: fill the whole span */
            fill_rect(L, y + j, R - L, 1, c);
            continue;
        }
        int Li = x + in, Ri = x + w - in;
        if (Li - L > 0) fill_rect(L, y + j, Li - L, 1, c);
        if (R - Ri > 0) fill_rect(Ri, y + j, R - Ri, 1, c);
    }
}

void ui_blit_scaled(int x, int y, int dw, int dh,
                    const uint32_t *src, int sw, int sh)
{
    if (G.backend != BACK_FB || !src || dw <= 0 || dh <= 0 ||
        sw <= 0 || sh <= 0) return;
    for (int j = 0; j < dh; j++) {
        int yy = y + j;
        if ((unsigned)yy >= (unsigned)G.fbh_px) continue;
        size_t sy = (size_t)j * (size_t)sh / (size_t)dh;
        for (int i = 0; i < dw; i++) {
            int xx = x + i;
            if ((unsigned)xx >= (unsigned)G.fbw_px) continue;
            uint32_t c = src[sy * (size_t)sw + (size_t)i * (size_t)sw / (size_t)dw];
            uint32_t a = c >> 24;
            if (a == 0) continue;
            if (a >= 255) { px_put(xx, yy, c & 0xFFFFFF); continue; }
            uint32_t d = px_get(xx, yy);
            uint32_t r = (((c >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a) + 127) / 255;
            uint32_t g = (((c >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a) + 127) / 255;
            uint32_t b = ((c & 255) * a + (d & 255) * (255 - a) + 127) / 255;
            px_put(xx, yy, ui_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b));
        }
    }
}

void ui_blit(int x, int y, int w, int h, const uint32_t *src)
{
    ui_blit_scaled(x, y, w, h, src, w, h);   /* 1:1 sampling == plain blit */
}

/* draw the 8x16 font at a pixel position, scale >= 1, opaque fg */
static void text_draw(int x, int y, int scale, const char *s, uint32_t fg)
{
    if (G.backend != BACK_FB || scale < 1) return;
    for (int ci = 0; s[ci]; ci++) {
        const uint8_t *gl = k93_font8x16 + ((unsigned char)s[ci]) * 16;
        int gx = x + ci * 8 * scale;
        for (int row = 0; row < 16; row++) {
            uint8_t bits = gl[row];
            if (!bits) continue;
            for (int col = 0; col < 8; col++) {
                if (!((bits >> (7 - col)) & 1)) continue;
                fill_rect(gx + col * scale, y + row * scale, scale, scale, fg);
            }
        }
    }
}

void ui_text(int x, int y, int scale, const char *s, uint32_t fg)
{
    text_draw(x, y, scale, s, fg);
}

void ui_text_sh(int x, int y, int scale, const char *s, uint32_t fg, uint32_t shadow)
{
    if (G.backend != BACK_FB || !s || !s[0]) return;
    int o = scale >= 2 ? 2 : 1;
    text_draw(x, y + o, scale, s, shadow);
    text_draw(x, y, scale, s, fg);
}

void ui_text_bold(int x, int y, int scale, const char *s, uint32_t fg, uint32_t shadow)
{
    if (G.backend != BACK_FB || !s || !s[0]) return;
    int o = scale >= 2 ? 2 : 1;
    text_draw(x, y + o, scale, s, shadow);
    text_draw(x, y, scale, s, fg);
    text_draw(x + 1, y, scale, s, fg);   /* faux bold: overstrike */
}

int ui_text_width(int scale, const char *s)
{
    if (scale < 1) scale = 1;
    return (int)strlen(s ? s : "") * 8 * scale;
}

int ui_px_width(void)  { return G.backend == BACK_FB ? G.fbw_px : G.cols * 8; }
int ui_px_height(void) { return G.backend == BACK_FB ? G.fbh_px : G.rows * 16; }

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
            uint8_t br, bg, bb;
            if (inverted) { br = bg = bb = v; }        /* black-on-white cell */
            else if (on)  { br = bg = bb = 0xFF; }     /* white glyph */
            else {                                      /* page gradient bg */
                uint32_t pc = page_row_color(y * 16 + row);
                br = (uint8_t)(pc >> 16); bg = (uint8_t)(pc >> 8); bb = (uint8_t)pc;
            }
            if (bpp == 4) { px[0] = bb; px[1] = bg; px[2] = br; px[3] = 0xFF; }
            else if (bpp == 3) { px[0] = bb; px[1] = bg; px[2] = br; }
            else if (bpp == 2) {                       /* RGB565 */
                uint16_t c = (uint16_t)(((br >> 3) << 11) | ((bg >> 2) << 5) | (bb >> 3));
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

/* clean-room pointer cursor, drawn from code over the scanout after each
 * shadow->scanout copy (the shadow buffer stays clean, so partial redraws
 * never leave trails). B = black outline, W = white fill, . = clear. */
static const char *cur_arrow[16] = {
    "B...........",
    "BB..........",
    "BWB.........",
    "BWWB........",
    "BWWWB.......",
    "BWWWWB......",
    "BWWWWWB.....",
    "BWWWWWWB....",
    "BWWWWWWWB...",
    "BWWWWWWWWB..",
    "BWWWWWWWWWB.",
    "BWWWWWBBBBB.",
    "....BWWB....",
    "....BWWB....",
    ".....BWB....",
    ".....BBB....",
};

/* stamp the arrow into the scanout at logical (ox,oy): draw=1 paints the
 * cursor, draw=0 restores the clean shadow pixel underneath (live motion).
 * fb_render_all stamps the current position after every full copy. */
static void cursor_stamp(int ox, int oy, int draw)
{
    if (G.backend != BACK_FB || !G.fbr || !G.fbw || ox < 0) return;
    int bpp = G.fbbpp / 8;
    for (int ry = 0; ry < 16; ry++) {
        for (int rx = 0; rx < 12; rx++) {
            int lx = ox + rx, ly = oy + ry;
            if ((unsigned)lx >= (unsigned)G.fbw_px ||
                (unsigned)ly >= (unsigned)G.fbh_px) continue;
            uint32_t col = 0;
            if (draw) {
                if (cur_arrow[ry][rx] == '.') continue;
                col = cur_arrow[ry][rx] == 'B' ? 0u : 0xFFFFFFu;
            } else {
                const uint8_t *s = G.fbw + (size_t)ly * (size_t)G.wline +
                                   (size_t)lx * (size_t)bpp;
                col = s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16);
            }
            int px, py;
            if (G.rot90) { px = G.sx - 1 - ly; py = lx; }  /* same transform
                                                              as fb_render_all */
            else { px = lx; py = ly; }
            if ((unsigned)px >= (unsigned)G.sx ||
                (unsigned)py >= (unsigned)G.sy) continue;
            uint8_t *d = G.fbr + (size_t)py * (size_t)G.fbline +
                         (size_t)px * (size_t)bpp;
            if (bpp >= 3) {
                d[0] = col & 0xFF;
                d[1] = (col >> 8) & 0xFF;
                d[2] = (col >> 16) & 0xFF;
            }
            if (bpp == 4) d[3] = 0xFF;
        }
    }
}

static void cursor_scanout(void)
{
    cursor_stamp(m_x, m_y, 1);
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
    cursor_scanout();
    if (G.drmfb > 0 && G.fbfd >= 0) {
        /* KMS: an atomic re-commit is the only thing that reaches the host.
         * mmap + DIRTYFB update KMS state but never drive the plane's
         * atomic_update, so no set_scanout/resource_flush happens and the
         * display keeps stale content. kms_flip() re-commits the adopted
         * buffer (full damage -> full plane update -> host flush). */
        kms_flip();
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
        for (int y = 0; y < G.fbh_px; y++) {
            uint32_t c = page_row_color(y);
            fill_rect(0, y, G.fbw_px, 1, c);
        }
    } else if (G.backend == BACK_TEXT) {
        const char *seq = "\033[2J\033[H\033[0m";
        write(G.outfd, seq, strlen(seq));
    }
}

void ui_flush(void)
{
    if (G.backend == BACK_FB) {
        fb_render_all();
    }
}

/* --- GUI widget chrome (graphical backend). Original iOS-4-*era* look:
 * dark gradient status bar, beveled blue-gray nav bar, light grouped pages,
 * blue selection. All shapes/colors are drawn from code (ui_roundrect,
 * ui_fill_grad, ui_tint), never copied bitmaps. */

#define GUI_WHITE  0xFFFFFFu
#define GUI_INK    0x14202Eu
#define GUI_SUBINK 0x6B7787u
#define GUI_PAGE   0xD9DEE6u
#define GUI_GROUP  0xF2F4F7u
#define GUI_SEP    0xC3CAD4u
#define GUI_BAR0   0x556070u          /* status/hint bar: dark slate */
#define GUI_BAR1   0x2A3340u
#define GUI_NAV0   0xA9B6C8u          /* nav bar: light blue-gray, bevel */
#define GUI_NAV1   0x7C8AA0u
#define GUI_NAVEDGE 0x4C586Au
#define GUI_SEL0   0x5C9CF0u          /* selection: iOS blue */
#define GUI_SEL1   0x2F6FD0u

static void gui_caret(int x, int y, int up, uint32_t c)   /* small triangle */
{
    for (int i = 0; i < 5; i++) {
        int w = up ? 9 - 2 * i : 2 + 2 * i;
        ui_fill(x + (9 - w) / 2, up ? y + i : y + 4 - i, w, 1, c);
    }
}

/* full-width dark bar (status at top, hint at bottom), white 1x text */
static void gui_bar(int ybot, int h, const char *left, const char *center,
                    const char *right)
{
    int W = G.fbw_px;
    ui_fill_grad(0, ybot, W, h, ui_rgb(0x56, 0x62, 0x74), ui_rgb(0x1E, 0x26, 0x33));
    ui_fill(0, ybot + h - 1, W, 1, ui_rgb(0x0A, 0x0E, 0x14));
    int ty = ybot + (h - 16) / 2;
    if (left && left[0]) {
        char t[128];
        snprintf(t, sizeof t, "%.126s", left);
        ui_text_sh(8, ty, 1, t, GUI_WHITE, ui_rgb(0x10, 0x14, 0x1C));
    }
    if (right && right[0]) {
        char t[128];
        snprintf(t, sizeof t, "%.126s", right);
        ui_text_sh(W - 8 - ui_text_width(1, t), ty, 1, t, GUI_WHITE,
                   ui_rgb(0x10, 0x14, 0x1C));
    }
    if (center && center[0]) {
        char t[128];
        snprintf(t, sizeof t, "%.126s", center);
        int cx = (W - ui_text_width(1, t)) / 2;
        ui_text_sh(cx, ty, 1, t, GUI_WHITE, ui_rgb(0x10, 0x14, 0x1C));
    }
}

/* compose the same right-side info string the text status bar shows */
static void gui_infobar(char *right, size_t cap)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof ts, "%H:%M", &tm);
    long operstate = k93_sysinfo_long("/sys/class/net/eth0/operstate");
    if (operstate < 0) operstate = k93_sysinfo_long("/sys/class/net/enp0s3/operstate");
    long cap_b = k93_sysinfo_long("/sys/class/power_supply/BAT0/capacity");
    if (cap_b < 0) cap_b = k93_sysinfo_long("/sys/class/power_supply/CMB0/capacity");
    snprintf(right, cap, "%ld%% %s %s", cap_b >= 0 ? cap_b : 100L,
             operstate == 1 ? "up" : "down", ts);
}

static void gui_statusbar(const char *carrier)
{
    char right[64];
    gui_infobar(right, sizeof right);
    gui_bar(0, 20, carrier ? carrier : G.appname, NULL, right);
    /* full-width pure-white gloss line: chrome accent + the screendump
     * signature (horizontal run in landscape, vertical after rot90) */
    ui_fill(0, 20, G.fbw_px, 2, ui_rgb(0xFF, 0xFF, 0xFF));
}

static void gui_hintbar(const char *msg)
{
    gui_bar(G.fbh_px - 24, 24, msg, NULL, NULL);
}

/* blue-gray beveled nav bar with a bold centered title */
static void gui_navbar(const char *title)
{
    int W = G.fbw_px;
    ui_fill_grad(0, 20, W, 44, ui_rgb(0xB6, 0xC2, 0xD4), ui_rgb(0x83, 0x92, 0xA8));
    ui_fill(0, 20, W, 1, ui_rgb(0xD8, 0xE0, 0xEA));
    ui_fill(0, 63, W, 1, ui_rgb(0x4E, 0x5A, 0x6C));
    if (title && title[0]) {
        char t[160];
        snprintf(t, sizeof t, "%.158s", title);
        int cx = (W - ui_text_width(1, t)) / 2;
        ui_text_sh(cx, 34, 1, t, GUI_WHITE, ui_rgb(0x3A, 0x46, 0x58));
    }
}

/* light grouped page with a rounded white panel; returns panel rect */
/* shared page-panel geometry (used by gui_page and the mouse hit test) */
static void page_panel(int panel_h, int *px, int *py, int *pw, int *ph)
{
    int margin = 24;
    if (panel_h > G.fbh_px - 64 - 24 - 40) panel_h = G.fbh_px - 64 - 24 - 40;
    *px = margin;
    *py = 88;
    *pw = G.fbw_px - 2 * margin;
    *ph = panel_h;
}

static void gui_page(const char *title, int panel_h, int *px, int *py,
                     int *pw, int *ph)
{
    ui_clear();
    ui_statusbar(NULL);
    gui_navbar(title);
    ui_fill(0, 64, G.fbw_px, G.fbh_px - 64 - 24, ui_rgb(0xCB, 0xD2, 0xDC));
    page_panel(panel_h, px, py, pw, ph);
    ui_roundrect(*px, *py, *pw, *ph, 12, ui_rgb(0xFF, 0xFF, 0xFF),
                 ui_rgb(0xEC, 0xEF, 0xF4));
}

/* one row inside a grouped panel; sel draws the iOS blue capsule with a
 * caret + white bold label, otherwise dark label on the white cell. */
#define GUI_ROW_H 34
static void gui_row(int x, int y, int w, const char *label, int sel)
{
    if (sel) {
        ui_roundrect(x + 4, y + 2, w - 8, GUI_ROW_H - 4, 9,
                     ui_tint(GUI_SEL0, 118, 100), GUI_SEL1);
        gui_caret(x + 12, y + GUI_ROW_H / 2 - 2, 1, GUI_WHITE);
        ui_text_bold(x + 30, y + (GUI_ROW_H - 16) / 2, 1, label, GUI_WHITE,
                     ui_rgb(0x1E, 0x40, 0x80));
    } else {
        ui_text_sh(x + 30, y + (GUI_ROW_H - 16) / 2, 1, label, GUI_INK,
                   ui_rgb(0xEE, 0xF1, 0xF6));
        ui_fill(x + 4, y + GUI_ROW_H - 1, w - 8, 1, GUI_SEP);
    }
}

/* ---- status bar / status message: graphical skin over the text logic --- */

void ui_statusbar(const char *title)
{
    if (G.backend == BACK_FB) {
        gui_statusbar(title);
        ui_flush();
        return;
    }
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
    char row[256];
    int width = G.cols;
    char rightfull[160];
    snprintf(rightfull, sizeof rightfull, "%.60s %.60s", bat, right);
    int rl = (int)strlen(rightfull);
    int padw = width - rl - 1 < 0 ? 0 : width - rl - 1;
    if (padw > 120) padw = 120;              /* keep inside row[256] */
    snprintf(row, sizeof row, "%.80s%*s%.80s", left, padw, "", mid);
    /* place right text at the end */
    char full[512];
    int plen = (int)strlen(row);
    int pad2 = 320 - plen;               /* 320 + rightfull[160] < 512 */
    if (pad2 < 0) pad2 = 0;
    snprintf(full, sizeof full, "%-*s%.159s", pad2, row, rightfull);
    ui_puts_inv(0, 0, full);
    (void)bar;
    ui_flush();
}

void ui_statusmsg(const char *msg)
{
    if (G.backend == BACK_FB) {
        gui_hintbar(msg ? msg : "");
        ui_flush();
        return;
    }
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
    /* fbcon may have left stale glyphs in the scanout buffer; the shadow is
     * cleared but only the logical region gets blitted, so clear the real
     * buffer too or the leftovers stay on screen. */
    memset(m, 0, len);
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

/* Resolve an object's property ID by name. Modern KMS drivers are atomic-only:
 * the legacy SETCRTC ioctl updates kernel state but never flushes a full modeset
 * to hardware, so scanout keeps showing fbcon's buffer. A real atomic commit is
 * the only way to hand the CRTC to our framebuffer. */
static uint32_t drm_obj_prop(int fd, uint32_t obj, uint32_t type, const char *name)
{
    struct drm_mode_obj_get_properties gp;
    memset(&gp, 0, sizeof gp);
    gp.obj_id = obj;
    gp.obj_type = type;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) < 0 || !gp.count_props)
        return 0;
    uint32_t ids[64], n = gp.count_props < 64 ? gp.count_props : 64;
    uint64_t vals[64];
    gp.props_ptr = (uintptr_t)ids;
    gp.prop_values_ptr = (uintptr_t)vals;
    gp.count_props = n;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) < 0) return 0;
    for (uint32_t i = 0; i < n; i++) {
        struct drm_mode_get_property pr; memset(&pr, 0, sizeof pr);
        pr.prop_id = ids[i];
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &pr) < 0) continue;
        if (!strncmp(pr.name, name, DRM_PROP_NAME_LEN)) return ids[i];
    }
    return 0;
}

/* Resolve an object's property *value* by name: same lookup as drm_obj_prop,
 * but reads back the current value. The kernel needs a count-probe first. */
static int drm_obj_prop_val(int fd, uint32_t obj, uint32_t type,
                            const char *name, uint64_t *out)
{
    struct drm_mode_obj_get_properties gp;
    memset(&gp, 0, sizeof gp);
    gp.obj_id = obj;
    gp.obj_type = type;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) < 0 || !gp.count_props)
        return -1;
    uint32_t ids[64], n = gp.count_props < 64 ? gp.count_props : 64;
    uint64_t vals[64];
    gp.props_ptr = (uintptr_t)ids;
    gp.prop_values_ptr = (uintptr_t)vals;
    gp.count_props = n;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) < 0) return -1;
    for (uint32_t i = 0; i < n; i++) {
        struct drm_mode_get_property pr; memset(&pr, 0, sizeof pr);
        pr.prop_id = ids[i];
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &pr) < 0) continue;
        if (strncmp(pr.name, name, DRM_PROP_NAME_LEN)) continue;
        /* count-probe the values array, then fetch */
        memset(&pr, 0, sizeof pr);
        pr.prop_id = ids[i];
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &pr) < 0 || !pr.count_values)
            return -1;
        uint64_t v = 0;
        pr.values_ptr = (uintptr_t)&v;
        pr.count_values = 1;
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &pr) < 0) return -1;
        *out = v;
        return 0;
    }
    return -1;
}

/* Atomic commit: point the primary plane at our fb and drive the CRTC with the
 * chosen mode. Returns 0 on success. */
static int drm_atomic_scanout(int fd, uint32_t crtc_id, uint32_t conn_id,
                              uint32_t fb_id, const struct drm_mode_modeinfo *mode)
{
    drm_ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP,
              &(struct drm_set_client_cap){ DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1 });
    drm_ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP,
              &(struct drm_set_client_cap){ DRM_CLIENT_CAP_ATOMIC, 1 });

    /* find the primary plane whose possible_crtcs covers our CRTC index */
    struct drm_mode_get_plane_res pr; memset(&pr, 0, sizeof pr);
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) < 0 || !pr.count_planes)
        return -1;
    uint32_t planes[32], pn = pr.count_planes < 32 ? pr.count_planes : 32;
    pr.plane_id_ptr = (uintptr_t)planes;
    pr.count_planes = pn;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) < 0) return -1;

    /* CRTC index = position of crtc_id in GETRESOURCES crtcs list */
    struct drm_mode_card_res res2; memset(&res2, 0, sizeof res2);
    uint32_t cidx = 0;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res2) == 0 && res2.count_crtcs) {
        uint32_t cl[32], cn = res2.count_crtcs < 32 ? res2.count_crtcs : 32;
        res2.crtc_id_ptr = (uintptr_t)cl;
        res2.count_crtcs = cn;
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res2) == 0)
            for (uint32_t i = 0; i < cn; i++) if (cl[i] == crtc_id) cidx = i;
    }

    /* Pick the PRIMARY plane for this CRTC. The kernel enumerates planes in an
     * arbitrary order (virtio-gpu lists the cursor plane first); attaching the
     * framebuffer to a non-primary plane leaves the primary scanning fbcon's
     * buffer, which is why the screen keeps showing garbage. The plane's
     * standard "type" property is Primary=1, Overlay=2, Cursor=3. */
    uint32_t plane = 0, fallback = 0;
    for (uint32_t i = 0; i < pr.count_planes; i++) {
        struct drm_mode_get_plane pl; memset(&pl, 0, sizeof pl);
        pl.plane_id = planes[i];
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &pl) < 0) continue;
        if (!(pl.possible_crtcs & (1u << cidx))) continue;
        if (!fallback) fallback = planes[i];
        uint64_t ptype = 0;
        if (drm_obj_prop_val(fd, planes[i], DRM_MODE_OBJECT_PLANE, "type", &ptype) == 0
            && ptype == 1) {                      /* 1 = DRM_PLANE_TYPE_PRIMARY */
            plane = planes[i];
            break;
        }
    }
    if (!plane) plane = fallback;
    if (!plane) { drm_dbg("k93ui: no primary plane for crtc %u idx %u\n", crtc_id, cidx); return -1; }

    uint32_t p_fb   = drm_obj_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "FB_ID");
    uint32_t p_crtc = drm_obj_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    uint32_t p_src  = drm_obj_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_X");
    uint32_t p_src_y= drm_obj_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    uint32_t p_src_w= drm_obj_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_W");
    uint32_t p_src_h= drm_obj_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_H");
    uint32_t p_cx   = drm_obj_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    uint32_t p_cy   = drm_obj_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    uint32_t p_cw   = drm_obj_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    uint32_t p_ch   = drm_obj_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    uint32_t c_mode = drm_obj_prop(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    uint32_t c_act  = drm_obj_prop(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    uint32_t o_conn = drm_obj_prop(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    if (!p_fb || !p_crtc || !c_mode || !c_act || !o_conn) {
        drm_dbg("k93ui: missing props fb=%u crtc=%u mode=%u act=%u conn=%u\n",
                p_fb, p_crtc, c_mode, c_act, o_conn);
        return -1;
    }

    /* MODE_ID takes a blob; create one from our modeline */
    struct drm_mode_create_blob mb; memset(&mb, 0, sizeof mb);
    mb.data = (uintptr_t)mode;
    mb.length = sizeof *mode;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &mb) < 0) {
        drm_dbg("k93ui: create_blob: %s\n", strerror(errno));
        return -1;
    }

    uint64_t W = mode->hdisplay, H = mode->vdisplay;
    /* three objects: plane gets 10 props, crtc gets MODE_ID+ACTIVE, connector
     * gets CRTC_ID */
    uint32_t objs2[3] = { plane, crtc_id, conn_id };
    uint32_t npc2[3]  = { 10,    2,       1 };

    uint32_t props[13] = {
        p_fb, p_crtc, p_src, p_src_y, p_src_w, p_src_h, p_cx, p_cy, p_cw, p_ch,
        c_mode, c_act,
        o_conn,
    };
    uint64_t vals[13] = {
        fb_id, crtc_id, 0, 0, W << 16, H << 16, 0, 0, W, H,
        mb.blob_id, 1,
        crtc_id,
    };

    struct drm_mode_atomic at; memset(&at, 0, sizeof at);
    at.flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
    at.count_objs = 3;
    at.objs_ptr = (uintptr_t)objs2;
    at.count_props_ptr = (uintptr_t)npc2;
    at.props_ptr = (uintptr_t)props;
    at.prop_values_ptr = (uintptr_t)vals;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &at) < 0) {
        drm_dbg("k93ui: atomic commit: %s\n", strerror(errno));
        struct drm_mode_destroy_blob db = { .blob_id = mb.blob_id };
        drm_ioctl(fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &db);
        return -1;
    }
    G.planeid = plane;
    struct drm_mode_destroy_blob db = { .blob_id = mb.blob_id };
    drm_ioctl(fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &db);
    return 0;
}

/* Atomically (re)commit the adopted scanout buffer. mmap + DIRTYFB never reach
 * the host: DIRTYFB does not drive the plane's atomic_update, so no
 * set_scanout/resource_flush is issued and the display keeps the old content.
 * Only a real atomic commit does (same path fbcon's fbdev client uses). A
 * re-commit of the SAME fb yields full damage -> full plane update -> flush.
 * Mode is already set from the initial drm_atomic_scanout; flips just refresh
 * FB_ID/geometry. Returns 0 on success. */
static int kms_flip(void)
{
    int fd = G.fbfd;
    uint32_t plane = G.planeid, crtc = G.drmcrtc, conn = G.drmconn;
    uint32_t fb = (uint32_t)G.drmfb;
    uint32_t p_fb    = drm_obj_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "FB_ID");
    uint32_t p_crtc  = drm_obj_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    uint32_t p_sx    = drm_obj_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_X");
    uint32_t p_sy    = drm_obj_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    uint32_t p_sw    = drm_obj_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_W");
    uint32_t p_sh    = drm_obj_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_H");
    uint32_t p_cx    = drm_obj_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    uint32_t p_cy    = drm_obj_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    uint32_t p_cw    = drm_obj_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    uint32_t p_ch    = drm_obj_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    uint32_t c_act   = drm_obj_prop(fd, crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    uint32_t o_conn  = conn ? drm_obj_prop(fd, conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID") : 0;
    if (!p_fb || !p_crtc || !c_act) return -1;
    uint64_t W = (uint64_t)G.sx, H = (uint64_t)G.sy;
    uint32_t objs[3] = { plane, crtc, conn };
    uint32_t npc[3]  = { 10, 1, conn ? 1u : 0u };
    uint32_t props[12] = {
        p_fb, p_crtc, p_sx, p_sy, p_sw, p_sh, p_cx, p_cy, p_cw, p_ch,
        c_act,
        o_conn,
    };
    uint64_t vals[12] = {
        fb, crtc, 0, 0, W << 16, H << 16, 0, 0, W, H,
        1,
        crtc,
    };
    struct drm_mode_atomic at; memset(&at, 0, sizeof at);
    at.flags = 0;                       /* same mode: fast page-flip */
    at.count_objs = conn ? 3u : 2u;
    at.objs_ptr = (uintptr_t)objs;
    at.count_props_ptr = (uintptr_t)npc;
    at.props_ptr = (uintptr_t)props;
    at.prop_values_ptr = (uintptr_t)vals;
    return drm_ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &at);
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

    /* We render straight into a KMS dumb framebuffer. /dev/fb0 never exists
     * (kernel is built CONFIG_FB_DEVICE=n so fb_device_create() makes no node),
     * so fbdev userspace is dead and DRM/KMS is the only path. fbcon owns the
     * CRTC at boot via its in-kernel fbdev client, so we: SET_MASTER, find the
     * active primary plane + its current mode, create our OWN dumb buffer at
     * that mode, and atomically commit it. Using a DIFFERENT fb than fbcon's
     * forces virtio_gpu_primary_plane_update's set_scanout path (skipped when
     * the fb is unchanged), which points the host scanout at our pixels;
     * kms_flip() re-commits then push damage. (Legacy SETCRTC is a state-only
     * no-op on atomic drivers.) */
    drm_ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP,
              &(struct drm_set_client_cap){ DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1 });
    if (drm_ioctl(fd, DRM_IOCTL_SET_MASTER, NULL) < 0)
        drm_dbg("k93ui: setmaster: %s\n", strerror(errno));

    struct drm_mode_get_plane_res pr; memset(&pr, 0, sizeof pr);
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) < 0 || !pr.count_planes) {
        drm_dbg("k93ui: no planes: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    uint32_t pls[32], pn = pr.count_planes < 32 ? pr.count_planes : 32;
    pr.plane_id_ptr = (uintptr_t)pls; pr.count_planes = pn;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) < 0) { close(fd); return -1; }

    /* Find the active primary plane + its CRTC. We only need the CRTC (and its
     * mode, below) — our own dumb buffer replaces scan_fb after ADDFB. */
    uint32_t scan_fb = 0, scan_crtc = 0, scan_plane = 0;
    for (uint32_t i = 0; i < pr.count_planes; i++) {
        struct drm_mode_get_plane pl; memset(&pl, 0, sizeof pl);
        pl.plane_id = pls[i];
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &pl) < 0) continue;
        uint64_t ptype = 99;
        drm_obj_prop_val(fd, pls[i], DRM_MODE_OBJECT_PLANE, "type", &ptype);
        if (ptype == 1 && pl.fb_id) {           /* DRM_PLANE_TYPE_PRIMARY */
            scan_fb = pl.fb_id;
            scan_crtc = pl.crtc_id;
            scan_plane = pls[i];
            break;
        }
        if (!scan_fb && pl.fb_id) { scan_fb = pl.fb_id; scan_crtc = pl.crtc_id;
                                    scan_plane = pls[i]; }
    }
    if (!scan_fb) { drm_dbg("k93ui: no active plane\n"); close(fd); return -1; }

    /* Get the CRTC's active mode (fbdev set it) so our buffer matches. */
    struct drm_mode_crtc mc; memset(&mc, 0, sizeof mc);
    mc.crtc_id = scan_crtc;
    struct drm_mode_modeinfo amode; memset(&amode, 0, sizeof amode);
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &mc) == 0 && mc.mode_valid)
        amode = mc.mode;
    if (!amode.hdisplay || !amode.vdisplay) {
        drm_dbg("k93ui: no active mode on crtc %u\n", scan_crtc);
        close(fd);
        return -1;
    }
    unsigned W = amode.hdisplay, H = amode.vdisplay;

    /* Our OWN scanout buffer: a dumb fb at the active mode size. Using a
     * DIFFERENT fb than fbcon's forces virtio_gpu_primary_plane_update's
     * set_scanout path (it is skipped when the fb is unchanged), which is what
     * actually points the QEMU host scanout at our pixels. */
    struct drm_mode_create_dumb cd = { .width = W, .height = H, .bpp = 32 };
    if (drm_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
        drm_dbg("k93ui: create_dumb: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    struct drm_mode_fb_cmd2 fb2; memset(&fb2, 0, sizeof fb2);
    fb2.width = W; fb2.height = H;
    fb2.pixel_format = DRM_FORMAT_XRGB8888;
    fb2.handles[0] = cd.handle; fb2.pitches[0] = cd.pitch; fb2.offsets[0] = 0;
    fb2.modifier[0] = DRM_FORMAT_MOD_LINEAR;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb2) < 0) {   /* fall back to legacy ADDFB */
        struct drm_mode_fb_cmd fb1; memset(&fb1, 0, sizeof fb1);
        fb1.width = W; fb1.height = H; fb1.pitch = cd.pitch;
        fb1.bpp = 32; fb1.depth = 24; fb1.handle = cd.handle;
        if (drm_ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb1) < 0) {
            drm_dbg("k93ui: addfb: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
        fb2.fb_id = fb1.fb_id;
    }
    scan_fb = fb2.fb_id;                          /* now OUR framebuffer */

    struct drm_mode_map_dumb md = { .handle = cd.handle };
    if (drm_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
        drm_dbg("k93ui: mapdumb: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    size_t alen = cd.size;
    uint8_t *am = mmap(NULL, alen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (am == MAP_FAILED) {
        drm_dbg("k93ui: mmap our fb: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* freeze fbcon (KD_GRAPHICS) so it stops drawing over the shared crtc */
    for (int vt = 1; vt <= 12; vt++) {
        char vtdev[16];
        snprintf(vtdev, sizeof vtdev, "/dev/tty%d", vt);
        int vtr = open(vtdev, O_RDWR | O_NOCTTY);
        if (vtr < 0) continue;
        ioctl(vtr, KDSETMODE, KD_GRAPHICS);
        close(vtr);
    }

    G.drmfb = scan_fb;                   /* DIRTYFB / kms_flip target */
    char orient[32], mode_req[32];
    display_conf(mode_req, sizeof mode_req, orient, sizeof orient);
    G.rot90 = !strcmp(orient, "portrait");
    G.sx = (int)W;                       /* physical scanout */
    G.sy = (int)H;
    if (G.rot90) { G.fbw_px = G.sy; G.fbh_px = G.sx; }   /* logical canvas */
    else         { G.fbw_px = G.sx; G.fbh_px = G.sy; }
    G.fbr = am; G.fblen = alen;
    G.fbline = (int)cd.pitch;
    G.fbbpp = 32;
    G.wline = G.fbw_px * 4;              /* shadow stride = logical width */
    G.wlen = (size_t)G.wline * (size_t)G.fbh_px;
    G.fbw = k93_xmalloc(G.wlen);
    memset(G.fbw, 0, G.wlen);
    G.fbfd = fd;
    G.cols = G.fbw_px / 8;
    G.rows = G.fbh_px / 16;

    /* Atomically commit OUR buffer. A DIFFERENT fb than fbcon's forces the
     * driver's set_scanout (skipped when fb is unchanged), pointing the QEMU
     * host scanout at our pixels. Uses the CRTC's current mode. */
    G.planeid = scan_plane;
    G.drmcrtc = scan_crtc;
    G.drmconn = con[0];
    drm_atomic_scanout(fd, scan_crtc, con[0], scan_fb, &amode);
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
    if (appname && *appname)
        k93_strlcpy(G.appname, appname, sizeof G.appname);
    else if (!G.appname[0])
        k93_strlcpy(G.appname, "K93OS", sizeof G.appname);
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
    if (G.backend == BACK_FB) {
        input_open_mouse();
        cursor_state_read();          /* keep pointer where the last app left it */
    }
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
    if (evfd >= 0) { close(evfd); evfd = -1; shift_down = 0; }
    for (int f = 0; f < nmfd; f++) close(mfd[f]);
    nmfd = 0;
    if (G.backend == BACK_FB) cursor_state_write();
    m_x = m_y = -1;
    m_click[0] = m_click[1] = m_click[2] = 0;
    m_wheel = 0; meta_held = 0; meta_fired = 0; pending_key = -1;
    if (infd > 2 && infd != G.outfd) close(infd);
    infd = 0;
    if (G.outfd > 2) close(G.outfd);
    G.backend = BACK_NONE;
}

/* ---------------------------------------------------------------- widgets */

/* draw one frame of a grouped menu (graphical backend) */
static void menu_gui(const char *title, const char *const *items, int n,
                     int sel, int top)
{
    int avail = G.fbh_px - 64 - 24 - 24;          /* below navbar, above hint */
    int view = avail / GUI_ROW_H;
    if (view < 1) view = 1;
    int px, py, pw, ph;
    int rows = n - top < view ? n - top : view;
    gui_page(title, rows * GUI_ROW_H + 8, &px, &py, &pw, &ph);
    for (int i = 0; i < rows; i++)
        gui_row(px, py + 4 + i * GUI_ROW_H, pw, items[top + i], top + i == sel);
    if (top > 0) gui_caret(px + pw - 18, py + 6, 0, GUI_SUBINK);
    if (top + view < n) gui_caret(px + pw - 18, py + ph - 12, 1, GUI_SUBINK);
    ui_statusmsg("Up/Down select   Enter open   Esc back");
    ui_flush();
}

int ui_menu(const char *title, const char *const *items, int n, int sel)
{
    if (sel < 0) sel = 0;
    if (n <= 0) return -1;
    int top = 0;
    int view = G.rows - 4;
    if (G.backend == BACK_FB)
        view = (G.fbh_px - 64 - 24 - 24) / GUI_ROW_H;   /* grouped rows */
    if (view < 1) view = 1;
    for (;;) {
        if (G.backend == BACK_FB) {
            menu_gui(title, items, n, sel, top);
        } else {
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
        }
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

/* iOS-style alert: light rounded panel, title, body lines, blue OK button.
 * Returns the pixel geometry so the caller can draw the button label. */
static void msgbox_gui(const char *title, const char *body, int *bx, int *by,
                       int *bw, int *bh)
{
    int px, py, pw;
    int avail = G.fbh_px - 64 - 24 - 24;
    int ph = avail < 320 ? avail : 320;
    gui_page(title, ph, &px, &py, &pw, &ph);
    int tx = px + 24, ty = py + 20, wrap = (pw - 48) / 8;
    if (wrap < 8) wrap = 8;
    char tmp[1024];
    k93_strlcpy(tmp, body ? body : "", sizeof tmp);
    char *save = NULL;
    for (char *line = strtok_r(tmp, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        /* wrap each logical line to the panel width, one visual line per pass */
        while (*line && ty <= py + ph - 60) {
            int take = (int)strlen(line);
            if (take > wrap) take = wrap;
            char chunk[512];
            snprintf(chunk, sizeof chunk, "%.*s", take, line);
            ui_text(tx, ty, 1, chunk, GUI_INK);
            ty += 20;
            line += take;
        }
        if (ty > py + ph - 60) break;
    }
    int w = 120, h = 40;
    int x = px + (pw - w) / 2, y = py + ph - h - 16;
    ui_roundrect(x, y, w, h, 10, ui_tint(GUI_SEL0, 118, 100), GUI_SEL1);
    *bx = x; *by = y; *bw = w; *bh = h;
}

void ui_msgbox(const char *title, const char *body)
{
    if (G.backend == BACK_FB) {
        int bx, by, bw, bh;
        msgbox_gui(title, body, &bx, &by, &bw, &bh);
        const char *ok = "OK";
        ui_text_bold(bx + (bw - ui_text_width(1, ok)) / 2, by + (bh - 16) / 2,
                     1, ok, GUI_WHITE, 0x1E4080);
        ui_statusmsg("Press any key");
        ui_flush();
        while (ui_getkey(250) == UI_KEY_NONE) ;
        return;
    }
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

/* one large action button (icon/confirm skin). sel draws the blue capsule. */
static void gui_button(int x, int y, int w, int h, const char *label, int sel)
{
    if (sel)
        ui_roundrect(x, y, w, h, 9, ui_tint(GUI_SEL0, 118, 100), GUI_SEL1);
    else
        ui_roundrect(x, y, w, h, 9, ui_rgb(0xF4, 0xF6, 0xF9), ui_rgb(0xDD, 0xE2, 0xEA));
    int tx = x + (w - ui_text_width(1, label)) / 2;
    int ty = y + (h - 16) / 2;
    if (sel) ui_text_bold(tx, ty, 1, label, GUI_WHITE, 0x1E4080);
    else     ui_text_bold(tx, ty, 1, label, GUI_INK, 0xFFFFFF);
}

int ui_confirm(const char *title, const char *body)
{
    if (G.backend == BACK_FB) {
        const char *opts[2] = { "Cancel", "Continue" };
        int sel = 1;                    /* blue = confirm */
        for (;;) {
            int bx, by, bw, bh;
            msgbox_gui(title, body, &bx, &by, &bw, &bh);
            int half = bw / 2 - 6;
            gui_button(bx, by, half, bh, opts[0], sel == 0);
            gui_button(bx + bw - half, by, half, bh, opts[1], sel == 1);
            ui_statusmsg("Left/Right choose   Enter select   Esc cancel");
            ui_flush();
            int k = ui_getkey(250);
            if (k == UI_KEY_LEFT || k == UI_KEY_RIGHT || k == UI_KEY_TAB) sel = !sel;
            else if (k == UI_KEY_ENTER) return sel;
            else if (k == UI_KEY_ESC || k == UI_KEY_CTRL_C) return 0;
        }
    }
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
        if (G.backend == BACK_FB) {
            int px, py, pw, ph;
            gui_page(prompt, 3, &px, &py, &pw, &ph);
            ph = 72;
            ui_roundrect(px, py, pw, ph, 8, ui_rgb(0xFF, 0xFF, 0xFF),
                         ui_rgb(0xE8, 0xEC, 0xF2));
            char shown[512];
            snprintf(shown, sizeof shown, "%.480s", buf);
            ui_text(px + 14, py + (ph - 16) / 2, 1, shown, GUI_INK);
            int cx = px + 14 + ui_text_width(1, shown);
            ui_fill(cx, py + (ph - 16) / 2, 9, 16, GUI_SEL0);   /* caret */
            ui_statusmsg("Enter accept   Esc cancel   (type to enter)");
            ui_flush();
        } else {
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
        }
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
    if (G.backend == BACK_FB) {
        /* document skin: white page, dark text, blue selection band */
        int px, py, pw, ph;
        gui_page(title, G.fbh_px - 64 - 24 - 12, &px, &py, &pw, &ph);
        int view = (ph - 8) / 18; if (view < 1) view = 1;
        for (int i = 0; i < view && top + i < nlines; i++) {
            int y = py + 4 + i * 18;
            if (top + i == sel) {
                ui_roundrect(px + 2, y - 1, pw - 4, 19, 4, ui_tint(GUI_SEL0, 112, 100), GUI_SEL1);
                ui_text(px + 14, y, 1, lines[top + i], GUI_WHITE);
            } else {
                ui_text(px + 14, y, 1, lines[top + i], GUI_INK);
            }
        }
        ui_statusmsg("Up/Dn scroll   PgUp/PgDn   Esc close");
        ui_flush();
        return;
    }
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
        int view = G.rows - 4;
        if (G.backend == BACK_FB)
            view = (G.fbh_px - 64 - 24 - 12 - 8) / 18;
        if (view < 1) view = 1;
        int k = ui_getkey(250);
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

/* --------------------------------------------------- styled page view */

#define PAGE_SEL  0x3F7FDCu
#define PAGE_LINK 0x1856B0u

static int page_h(const ui_pline *l, int text_rows)
{
    if (l->img) return text_rows ? 1 : l->imgh + 10;
    if (text_rows) return 1;
    int sc = l->scale < 1 ? 1 : (l->scale > 2 ? 2 : l->scale);
    return sc * 16 + (sc > 1 ? 6 : 2);
}

static void page_render(const char *title, ui_page *p, int top, int sel)
{
    if (G.backend == BACK_FB) {
        int px, py, pw, ph;
        gui_page(title, G.fbh_px - 64 - 24 - 12, &px, &py, &pw, &ph);
        int viewh = ph - 8;
        int y = py + 4;
        for (int i = top; i < p->n && y < py + ph; i++) {
            const ui_pline *l = &p->lines[i];
            int lh = page_h(l, 0);
            if (lh > viewh) lh = viewh;
            int issel = (i == sel);
            if (issel)
                ui_roundrect(px + 2, y - 1, pw - 4, lh, 4,
                             ui_tint(GUI_SEL0, 112, 100), GUI_SEL1);
            if (l->img) {
                int ix = px + 14 + l->indent;
                if (l->imgw < pw - 28) ix = px + (pw - l->imgw) / 2;
                ui_blit(ix, y + 3, l->imgw, l->imgh, l->img);
            } else {
                if (l->bg && !issel)
                    ui_fill(px + 2, y, pw - 4, lh, l->bg);
                int tw = 0;
                for (int r = 0; r < l->nruns; r++)
                    tw += ui_text_width(l->scale > 0 ? l->scale : 1,
                                        l->runs[r].text);
                int x = px + 14 + l->indent;
                if (l->center) {
                    x = px + (pw - tw) / 2;
                    if (x < px + 14 + l->indent) x = px + 14 + l->indent;
                }
                for (int r = 0; r < l->nruns; r++) {
                    const ui_prun *rn = &l->runs[r];
                    uint32_t fg = issel ? GUI_WHITE
                                : (rn->link >= 0 ? PAGE_LINK : rn->fg);
                    int sc = l->scale > 0 ? l->scale : 1;
                    if (rn->bold)
                        ui_text_bold(x, y + 3, sc, rn->text, fg,
                                     issel ? PAGE_SEL : 0xEEF1F6u);
                    else
                        ui_text_sh(x, y + 3, sc, rn->text, fg,
                                   issel ? PAGE_SEL : 0xFFFFFFu);
                    x += ui_text_width(sc, rn->text);
                }
            }
            y += lh;
        }
        int has_link = 0;
        for (int i = 0; i < p->n; i++)
            if (p->lines[i].link >= 0) { has_link = 1; break; }
        ui_statusmsg(has_link
                     ? "Up/Dn scroll   Enter open link   Esc close"
                     : "Up/Dn scroll   Esc close");
        ui_flush();
        return;
    }
    /* text backend */
    ui_clear();
    ui_statusbar(title);
    int view = G.rows - 4; if (view < 1) view = 1;
    for (int i = 0; i < view && top + i < p->n; i++) {
        const ui_pline *l = &p->lines[top + i];
        char row[512]; size_t o = 0; row[0] = 0;
        if (l->img)
            snprintf(row, sizeof row, "[image %dx%d]", l->imgw, l->imgh);
        else for (int r = 0; r < l->nruns && o + 1 < sizeof row; r++) {
            size_t tl = strlen(l->runs[r].text);
            if (o + tl > sizeof row - 1) tl = sizeof row - 1 - o;
            memcpy(row + o, l->runs[r].text, tl);
            o += tl; row[o] = 0;
        }
        if (top + i == sel) ui_puts_inv(0, 2 + i, row);
        else ui_puts(0, 2 + i, row);
    }
    ui_flush();
}

/* mouse hit test: which rendered line covers (x,y)? Same walk as page_render. */
static int page_hit(ui_page *p, int top, int x, int y)
{
    if (!p || p->n < 1) return -1;
    int px, py, pw, ph;
    page_panel(G.fbh_px - 64 - 24 - 12, &px, &py, &pw, &ph);
    if (x < px || x >= px + pw) return -1;
    int viewh = ph - 8;
    int ly = py + 4;
    for (int i = top; i < p->n && ly < py + ph; i++) {
        int lh = page_h(&p->lines[i], 0);
        if (lh > viewh) lh = viewh;
        if (y >= ly && y < ly + lh) return i;
        ly += lh;
    }
    return -1;
}

int ui_pageview(const char *title, ui_page *p,
                int (*tick)(ui_page *p, void *ctx), void *ctx)
{
    int sel = p->sel, top = p->top;       /* preserved across re-entries */
    if (sel >= p->n) sel = p->n - 1;
    if (sel < 0) sel = 0;
    if (top < 0 || top > sel) top = 0;
    if (!p || p->n < 1) return -1;
    for (;;) {
        page_render(title, p, top, sel);
        if (sel >= p->n) sel = p->n - 1;
        /* mouse: right = back, wheel scrolls, left activates the link under
         * the cursor. Clicks are consumed here so ui_getkey cannot turn them
         * into Enter on the selection. */
        int handled = 0;
        if (ui_mouse_clicked(1)) { p->sel = sel; p->top = top; return -1; }
        int mw = ui_mouse_scroll();
        if (mw) {
            sel += mw;
            if (sel < 0) sel = 0;
            if (sel > p->n - 1) sel = p->n - 1;
            handled = 1;
        }
        if (ui_mouse_clicked(0)) {
            handled = 1;
            int li = page_hit(p, top, ui_mouse_x(), ui_mouse_y());
            if (li >= 0 && p->lines[li].link >= 0) {
                p->sel = li; p->top = top;
                return p->lines[li].link;
            }
        }
        int k = handled ? UI_KEY_NONE : ui_getkey(250);
        int view, viewh = 0;
        if (G.backend == BACK_FB) {
            viewh = (G.fbh_px - 64 - 24 - 12 - 8);
            int y = 0, i = top;
            while (i < p->n && y + page_h(&p->lines[i], 0) <= viewh) {
                y += page_h(&p->lines[i], 0); i++;
            }
            view = i - top; if (view < 1) view = 1;
        } else {
            view = G.rows - 4; if (view < 1) view = 1;
        }
        switch (k) {
        case UI_KEY_UP: if (sel > 0) sel--; break;
        case UI_KEY_DOWN: if (sel < p->n - 1) sel++; break;
        case UI_KEY_PGUP: sel -= view; if (sel < 0) sel = 0; break;
        case UI_KEY_PGDN: sel += view; if (sel > p->n - 1) sel = p->n - 1; break;
        case UI_KEY_ENTER:
            if (p->lines[sel].link >= 0) {
                p->sel = sel; p->top = top;
                return p->lines[sel].link;
            }
            break;
        case UI_KEY_ESC: case UI_KEY_CTRL_C:
            p->sel = sel; p->top = top;
            return -1;
        default: break;
        }
        p->sel = sel; p->top = top;
        if (tick && tick(p, ctx) == 2) {
            p->sel = sel; p->top = top;
            return -1;
        }
        /* keep selection visible */
        if (sel < top) top = sel;
        if (G.backend == BACK_FB) {
            int y = 0, i = top;
            while (i < sel && y + page_h(&p->lines[i], 0) < viewh) {
                y += page_h(&p->lines[i], 0); i++;
            }
            while (y + page_h(&p->lines[sel], 0) > viewh && top < sel) {
                y -= page_h(&p->lines[top], 0);
                if (y < 0) y = 0;
                top++;
            }
        } else {
            if (sel >= top + view) top = sel - view + 1;
        }
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
        ui_mouse_clicked(0);                       /* clicks are inert here */
        ui_mouse_clicked(1);
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
