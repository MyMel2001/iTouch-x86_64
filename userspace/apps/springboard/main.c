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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

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
            ui_clear();
            ui_statusbar("SpringBoard");
            ui_puts(2, 3, "No applications installed.");
            ui_puts(2, 4, "Copy .ipa bundles into /var/mobile/Media/Downloads");
            ui_puts(2, 5, "and install them with the Installer app.");
            ui_statusmsg("Esc exit");
            ui_flush();
            int k = ui_getkey(250);
            if (k == UI_KEY_ESC || k == UI_KEY_CTRL_C) break;
            if (k == UI_KEY_F12) break;
            continue;
        }
        draw_home(apps, n, sel, hint);
        int k = ui_getkey(250);
        hint[0] = 0;
        switch (k) {
        case UI_KEY_UP: if (sel > 0) sel--; break;
        case UI_KEY_DOWN: if (sel < n - 1) sel++; break;
        case UI_KEY_HOME: sel = 0; break;
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
            ui_clear();
            ui_printf(1, 2, "Launching %s ...", apps[sel].name);
            ui_flush();
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
