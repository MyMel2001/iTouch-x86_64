/* SpringBoard.app - K93OS-x86 home screen (clean-room).
 * The original SpringBoard (observed in the supplied rootfs as
 * System/Library/CoreServices/SpringBoard.app) is the iOS shell that shows the
 * icon grid and launches frontmost applications. Same role here; the
 * implementation is original and renders through libk93 (framebuffer or
 * serial/VT text). */
#define _GNU_SOURCE
#include "k93ui.h"
#include "k93util.h"
#include "k93app.h"
#include "k93icons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* ---- GUI home screen ---------------------------------------------------- */

/* glossy backdrop: vertical gradient + faint light bands, all from code */
static void draw_wallpaper(void)
{
    int W = ui_px_width(), H = ui_px_height();
    uint32_t top = ui_rgb(0x1B, 0x2A, 0x4A), bot = ui_rgb(0x0B, 0x11, 0x20);
    ui_fill_grad(0, 0, W, H, top, bot);
    uint32_t sheen = ui_tint(top, 118, 100);
    for (int y = 0; y < H; y += 8)        /* 3px sheen, 5px gap */
        ui_fill(0, y, W, 3, sheen);
}

#define GRID_COLS 4
#define ICON_PX   80

/* draw one app icon + label at a cell origin */
static void draw_icon_cell(int cx, int cy, int cw, const k93_app *app, int sel)
{
    const uint32_t *img = k93_icon_by_name(app->ident, NULL, NULL);
    int iw = ICON_PX, ih = ICON_PX;
    int ix = cx + (cw - ICON_PX) / 2;
    int iy = cy;
    ui_blit(ix, iy, iw, ih, img);
    if (sel) {
        ui_frame(ix - 3, iy - 3, ICON_PX + 6, ICON_PX + 6, 16, 2, ui_rgb(0xFF, 0xFF, 0xFF));
        /* label sits in a blue capsule under the selected icon */
        char lab[48];
        snprintf(lab, sizeof lab, "%.20s", app->name);
        int lw = ui_text_width(1, lab) + 16;
        if (lw > cw - 8) lw = cw - 8;
        int ly = iy + ICON_PX + 6;
        ui_roundrect(cx + (cw - lw) / 2, ly, lw, 20, 9,
                     ui_tint(ui_rgb(0x3B, 0x82, 0xF6), 116, 100), ui_rgb(0x1D, 0x4E, 0xD8));
        ui_text_bold(cx + (cw - ui_text_width(1, lab)) / 2, ly + 2, 1, lab,
                     ui_rgb(0xFF, 0xFF, 0xFF), 0x14306A);
    } else {
        char lab[48];
        snprintf(lab, sizeof lab, "%.20s", app->name);
        ui_text_sh(cx + (cw - ui_text_width(1, lab)) / 2, iy + ICON_PX + 6, 1, lab,
                   ui_rgb(0xE8, 0xEE, 0xF6), 0x060C18);
    }
}

static void draw_home_gui(const k93_app *apps, int n, int sel, const char *hint)
{
    int W = ui_px_width(), H = ui_px_height();
    draw_wallpaper();
    int cols = GRID_COLS;
    int cw = W / cols;
    int top = 28;
    int rowh = ICON_PX + 40;
    int view = (H - top - 36) / rowh;
    if (view < 1) view = 1;
    int pagesel_row = sel / cols;
    int trow = 0;
    if (pagesel_row >= view) trow = pagesel_row - view + 1;
    for (int i = 0; i < view * cols && trow * cols + i < n; i++) {
        int idx = trow * cols + i;
        int cx = (i % cols) * cw;
        int cy = top + (i / cols) * rowh;
        draw_icon_cell(cx, cy, cw, &apps[idx], idx == sel);
    }
    char title[64];
    snprintf(title, sizeof title, "SpringBoard");
    ui_statusbar(title);
    ui_statusmsg(hint ? hint : "Arrows  Enter launch  F12 shell  Esc exit");
    ui_flush();
}

static void draw_home(const k93_app *apps, int n, int sel, const char *hint)
{
    ui_clear();
    ui_statusbar("K93OS");
    char title[128];
    snprintf(title, sizeof title, "SpringBoard - %d apps", n);
    ui_puts(1, 1, title);
    int view = ui_height() - 5;
    if (view < 1) view = 1;
    int top = 0;
    if (sel >= view) top = sel - view + 1;
    for (int i = 0; i < view && top + i < n; i++) {
        int idx = top + i;
        char row[256];
        snprintf(row, sizeof row, " %c %-40.40s %-24.24s %s",
                 idx == sel ? '>' : ' ',
                 apps[idx].name, apps[idx].ident,
                 apps[idx].sideloaded ? "[sideloaded]" : "");
        if (idx == sel) ui_puts_inv(0, 2 + i, row);
        else ui_puts(0, 2 + i, row);
    }
    ui_statusmsg(hint ? hint : "Up/Down  Enter launch  F12 shell  Esc exit");
    ui_flush();
}

int main(void)
{
    if (ui_init("SpringBoard") < 0) return 1;
    k93_app *apps = k93_xmalloc(sizeof(k93_app) * 256);
    int n = k93_app_scan(apps, 256);
    k93_log("springboard", "scanned %d application bundles", n);
    int sel = 0;
    char hint[96] = "";
    for (;;) {
        if (n == 0) {
            if (ui_is_graphical()) {
                draw_wallpaper();
                int W = ui_px_width(), H = ui_px_height();
                const char *msg = "No applications installed";
                ui_text_bold((W - ui_text_width(1, msg)) / 2, H / 2 - 8, 1,
                             msg, ui_rgb(0xE8, 0xEE, 0xF6), 0x060C18);
                ui_statusbar("SpringBoard");
                ui_statusmsg("Esc exit");
                ui_flush();
            } else {
                ui_clear();
                ui_statusbar("SpringBoard");
                ui_puts(2, 3, "No applications installed.");
                ui_puts(2, 4, "Copy .ipa bundles into /var/mobile/Media/Downloads");
                ui_puts(2, 5, "and install them with the Installer app.");
                ui_statusmsg("Esc exit");
                ui_flush();
            }
            int k = ui_getkey(250);
            if (k == UI_KEY_ESC || k == UI_KEY_CTRL_C) break;
            if (k == UI_KEY_F12) break;
            continue;
        }
        if (ui_is_graphical()) draw_home_gui(apps, n, sel, hint);
        else draw_home(apps, n, sel, hint);
        int k = ui_getkey(250);
        hint[0] = 0;
        int cols = ui_is_graphical() ? GRID_COLS : 1;
        switch (k) {
        case UI_KEY_UP:   sel -= cols; if (sel < 0) sel = 0; break;
        case UI_KEY_DOWN: sel += cols; if (sel >= n) sel = n - 1; break;
        case UI_KEY_LEFT: if (sel > 0) sel--; break;
        case UI_KEY_RIGHT:if (sel < n - 1) sel++; break;
        case UI_KEY_HOME: sel = 0; break;
        case UI_KEY_PGUP: sel -= cols * 4; if (sel < 0) sel = 0; break;
        case UI_KEY_PGDN: sel += cols * 4; if (sel >= n) sel = n - 1; break;
        case UI_KEY_ESC: case UI_KEY_CTRL_C:
            ui_shutdown();
            return 0;
        case UI_KEY_F12: {
            /* hand the console to a root shell, then come back */
            ui_shutdown();
            pid_t p = fork();
            if (p == 0) {
                execl("/bin/busybox", "busybox", "sh", "-l", (char *)NULL);
                _exit(127);
            }
            int st = 0;
            waitpid(p, &st, 0);
            ui_init("SpringBoard");
            break;
        }
        case UI_KEY_ENTER: {
            if (ui_is_graphical()) {
                draw_wallpaper();
                int W = ui_px_width();
                const uint32_t *img = k93_icon_by_name(apps[sel].ident, NULL, NULL);
                int iw = ICON_PX, ih = ICON_PX;
                ui_blit((W - iw) / 2, (ui_px_height() - ih) / 2 - 20, iw, ih, img);
                char msg[64];
                snprintf(msg, sizeof msg, "Launching %s", apps[sel].name);
                ui_text_bold((W - ui_text_width(1, msg)) / 2,
                             (ui_px_height() + ICON_PX) / 2, 1, msg,
                             ui_rgb(0xE8, 0xEE, 0xF6), 0x060C18);
                ui_flush();
            } else {
                ui_clear();
                ui_printf(1, 2, "Launching %s ...", apps[sel].name);
                ui_flush();
            }
            usleep(150000);
            ui_shutdown();
            int rc = k93_app_launch(&apps[sel]);
            ui_init("SpringBoard");
            if (rc != 0) {
                char msg[192];
                snprintf(msg, sizeof msg, "%s exited with status %d",
                         apps[sel].name, WEXITSTATUS(rc) ? WEXITSTATUS(rc) : rc);
                ui_msgbox("Application", msg);
            }
            /* refresh list: sideloaded apps may have appeared */
            free(apps);
            apps = k93_xmalloc(sizeof(k93_app) * 256);
            n = k93_app_scan(apps, 256);
            if (sel >= n) sel = n > 0 ? n - 1 : 0;
            break;
        }
        default: break;
        }
    }
    free(apps);
    ui_shutdown();
    return 0;
}
