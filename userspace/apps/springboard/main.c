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

/* glossy backdrop: user wallpaper image if set (iOS 4.0 home-screen
 * wallpaper), else the code-drawn vertical gradient + light bands. */
static void draw_wallpaper(void)
{
    int W = ui_px_width(), H = ui_px_height();
    size_t len = 0;
    char *c = k93_read_file("/etc/k93/wallpaper.conf", &len);
    char path[512] = "";
    if (c) {
        for (char *line = strtok(c, "\n"); line; line = strtok(NULL, "\n"))
            if (!strncmp(line, "PATH=", 5)) k93_strlcpy(path, line + 5, sizeof path);
        free(c);
    }
    if (path[0]) {
        ui_image im;
        if (ui_image_load(&im, path) == 0) {
            ui_blit_scaled(0, 0, W, H, im.px, im.w, im.h);
            ui_image_free(&im);
            return;
        }
    }
    uint32_t top = ui_rgb(0x1B, 0x2A, 0x4A), bot = ui_rgb(0x0B, 0x11, 0x20);
    ui_fill_grad(0, 0, W, H, top, bot);
    uint32_t sheen = ui_tint(top, 118, 100);
    for (int y = 0; y < H; y += 8)        /* 3px sheen, 5px gap */
        ui_fill(0, y, W, 3, sheen);
}

#define GRID_COLS 4
#define ICON_PX   80

/* draw one app icon + label at a cell origin */
static void draw_cell_label(int cx, int cy, int cw, const char *name, int sel)
{
    int iy = cy;
    char lab[48];
    snprintf(lab, sizeof lab, "%.20s", name);
    if (sel) {
        int lw = ui_text_width(1, lab) + 16;
        if (lw > cw - 8) lw = cw - 8;
        int ly = iy + ICON_PX + 6;
        ui_roundrect(cx + (cw - lw) / 2, ly, lw, 20, 9,
                     ui_tint(ui_rgb(0x3B, 0x82, 0xF6), 116, 100), ui_rgb(0x1D, 0x4E, 0xD8));
        ui_text_bold(cx + (cw - ui_text_width(1, lab)) / 2, ly + 2, 1, lab,
                     ui_rgb(0xFF, 0xFF, 0xFF), 0x14306A);
    } else {
        ui_text_sh(cx + (cw - ui_text_width(1, lab)) / 2, iy + ICON_PX + 6, 1, lab,
                   ui_rgb(0xE8, 0xEE, 0xF6), 0x060C18);
    }
}

static void draw_icon_cell(int cx, int cy, int cw, const k93_app *app, int sel)
{
    const uint32_t *img = k93_icon_by_name(app->ident, NULL, NULL);
    int ix = cx + (cw - ICON_PX) / 2;
    ui_blit(ix, cy, ICON_PX, ICON_PX, img);
    if (sel)
        ui_frame(ix - 3, cy - 3, ICON_PX + 6, ICON_PX + 6, 16, 2, ui_rgb(0xFF, 0xFF, 0xFF));
    draw_cell_label(cx, cy, cw, app->name, sel);
}

/* ---- folders (iOS 4.0: group apps on the home screen) ---- */
#define FOLDER_MAX  16
#define FOLDER_APPS 8
#define FOLDER_CONF "/var/mobile/Library/SpringBoard/folders.conf"
typedef struct { char name[64]; char idents[FOLDER_APPS][128]; int n; } folder_t;
static folder_t folders[FOLDER_MAX];
static int nfolders;

static void folders_load(void)
{
    nfolders = 0;
    size_t len = 0;
    char *c = k93_read_file(FOLDER_CONF, &len);
    if (!c) return;
    for (char *line = strtok(c, "\n"); line && nfolders < FOLDER_MAX;
         line = strtok(NULL, "\n")) {
        char *bar = strchr(line, '|');
        if (!bar) continue;
        *bar = 0;
        char *nm = k93_trim(line);
        if (!nm[0]) continue;
        folder_t *f = &folders[nfolders];
        k93_strlcpy(f->name, nm, sizeof f->name);
        f->n = 0;
        for (char *tok = strtok(bar + 1, ","); tok && f->n < FOLDER_APPS;
             tok = strtok(NULL, ",")) {
            char *id = k93_trim(tok);
            if (!id[0]) continue;
            k93_strlcpy(f->idents[f->n++], id, 128);
        }
        if (f->n > 0) nfolders++;
    }
    free(c);
}

static void folders_save(void)
{
    k93_mkdir_p("/var/mobile/Library/SpringBoard", 0777);
    size_t cap = 1;
    for (int i = 0; i < nfolders; i++) {
        cap += strlen(folders[i].name) + 2;
        for (int j = 0; j < folders[i].n; j++) cap += strlen(folders[i].idents[j]) + 1;
    }
    char *b = k93_xmalloc(cap);
    if (!b) return;
    size_t o = 0;
    for (int i = 0; i < nfolders; i++) {
        o += (size_t)snprintf(b + o, cap - o, "%s|", folders[i].name);
        for (int j = 0; j < folders[i].n; j++)
            o += (size_t)snprintf(b + o, cap - o, "%s%s",
                                  j ? "," : "", folders[i].idents[j]);
        o += (size_t)snprintf(b + o, cap - o, "\n");
    }
    k93_write_file(FOLDER_CONF, b, o, 0);
    free(b);
}

static void draw_folder_cell(int cx, int cy, int cw, const folder_t *f, int sel)
{
    int ix = cx + (cw - ICON_PX) / 2;
    uint32_t top = ui_rgb(0xF2, 0xB0, 0x4A), bot = ui_rgb(0xC8, 0x7A, 0x1E);
    ui_roundrect(ix, cy + 8, ICON_PX, ICON_PX - 8, 16, top, bot);
    ui_fill(ix, cy, ICON_PX, 30, top);            /* folder tab */
    char cnt[8];
    snprintf(cnt, sizeof cnt, "%d", f->n);
    ui_text_bold(ix + ICON_PX / 2 - (int)ui_text_width(1, cnt) / 2, cy + 34, 1,
                 cnt, ui_rgb(0xFF, 0xFF, 0xFF), 0x8A5A10);
    if (sel)
        ui_frame(ix - 3, cy - 3, ICON_PX + 6, ICON_PX + 6, 16, 2, ui_rgb(0xFF, 0xFF, 0xFF));
    draw_cell_label(cx, cy, cw, f->name, sel);
}

/* display list: app indices, then folders as -1-folder_index */
static int dlist[256];
static int ndlist;

static void build_dlist(const k93_app *apps, int n)
{
    ndlist = 0;
    for (int i = 0; i < n; i++) {
        int inf = 0;
        for (int fi = 0; fi < nfolders && !inf; fi++)
            for (int j = 0; j < folders[fi].n; j++)
                if (!strcmp(folders[fi].idents[j], apps[i].ident)) { inf = 1; break; }
        if (!inf && ndlist < 256) dlist[ndlist++] = i;
    }
    for (int fi = 0; fi < nfolders && ndlist < 256; fi++)
        dlist[ndlist++] = -1 - fi;
}

static void draw_home_gui(const k93_app *apps, int n, int sel, const char *hint)
{
    (void)n;
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
    for (int i = 0; i < view * cols && trow * cols + i < ndlist; i++) {
        int idx = trow * cols + i;
        int cx = (i % cols) * cw;
        int cy = top + (i / cols) * rowh;
        if (dlist[idx] < 0)
            draw_folder_cell(cx, cy, cw, &folders[-1 - dlist[idx]], idx == sel);
        else
            draw_icon_cell(cx, cy, cw, &apps[dlist[idx]], idx == sel);
    }
    char title[64];
    snprintf(title, sizeof title, "SpringBoard");
    ui_statusbar(title);
    ui_statusmsg(hint ? hint : "Arrows/Click launch  f:folder  Win=home  F12  Esc");
    ui_flush();
}

static void draw_home(const k93_app *apps, int n, int sel, const char *hint)
{
    (void)n;
    ui_clear();
    ui_statusbar("K93OS");
    char title[128];
    snprintf(title, sizeof title, "SpringBoard - %d apps", ndlist);
    ui_puts(1, 1, title);
    int view = ui_height() - 5;
    if (view < 1) view = 1;
    int top = 0;
    if (sel >= view) top = sel - view + 1;
    for (int i = 0; i < view && top + i < ndlist; i++) {
        int idx = top + i;
        char row[256];
        if (dlist[idx] < 0)
            snprintf(row, sizeof row, " %c [folder] %-40.40s",
                     idx == sel ? '>' : ' ', folders[-1 - dlist[idx]].name);
        else {
            const k93_app *a = &apps[dlist[idx]];
            snprintf(row, sizeof row, " %c %-40.40s %-24.24s %s",
                     idx == sel ? '>' : ' ',
                     a->name, a->ident,
                     a->sideloaded ? "[sideloaded]" : "");
        }
        if (idx == sel) ui_puts_inv(0, 2 + i, row);
        else ui_puts(0, 2 + i, row);
    }
    ui_statusmsg(hint ? hint : "Up/Down  Enter launch  f:folder  F12 shell  Esc exit");
    ui_flush();
}

/* ---- mouse ------------------------------------------------------------- */

/* home grid hit test; same trow math as draw_home_gui */
static int gui_hit_icon(int sel)
{
    int W = ui_px_width(), H = ui_px_height();
    int cw = W / GRID_COLS, top = 28, rowh = ICON_PX + 40;
    int view = (H - top - 36) / rowh;
    if (view < 1) view = 1;
    int trow = 0;
    if (sel / GRID_COLS >= view) trow = sel / GRID_COLS - view + 1;
    int mx = ui_mouse_x(), my = ui_mouse_y();
    if (mx < 0 || my < top) return -1;
    int col = mx / cw, row = (my - top) / rowh;
    if (col >= GRID_COLS || row >= view) return -1;
    int idx = trow * GRID_COLS + row * GRID_COLS + col;
    if (idx < 0 || idx >= ndlist) return -1;
    return idx;
}

/* ---- multitasking tray (iOS-6-style bottom bar of recent apps) --------- */

#define TRAY_MAX  8
#define TRAY_PX   64                     /* tray icon size */
#define TRAY_STEP 84
#define TRAY_H    116

static char recent[TRAY_MAX][128];
static int nrecent;

static void recent_add(const char *ident)
{
    int i, j;
    for (i = 0; i < nrecent; i++)
        if (!strcmp(recent[i], ident)) break;
    if (i == nrecent && nrecent == TRAY_MAX) i = TRAY_MAX - 1; /* drop oldest */
    for (j = i; j > 0; j--) memcpy(recent[j], recent[j - 1], sizeof recent[0]);
    k93_strlcpy(recent[0], ident, sizeof recent[0]);
    if (nrecent < TRAY_MAX) nrecent++;
}

/* recent apps still installed -> indices; returns count */
static int tray_index(const k93_app *apps, int n, int idxs[TRAY_MAX])
{
    int cnt = 0;
    for (int i = 0; i < nrecent && cnt < TRAY_MAX; i++)
        for (int j = 0; j < n; j++)
            if (!strcmp(apps[j].ident, recent[i])) { idxs[cnt++] = j; break; }
    return cnt;
}

static void tray_geom(int cnt, int *x0, int *iy)
{
    int W = ui_px_width(), H = ui_px_height();
    *x0 = (W - (cnt * TRAY_STEP - (TRAY_STEP - TRAY_PX))) / 2;
    *iy = H - 24 - TRAY_H + (TRAY_H - TRAY_PX - 20) / 2;
}

static void draw_tray(const k93_app *apps, int n, int tsel)
{
    int W = ui_px_width(), H = ui_px_height();
    int idxs[TRAY_MAX];
    int cnt = tray_index(apps, n, idxs);
    if (tsel >= cnt) tsel = cnt - 1;
    draw_home_gui(apps, n, -1, "");            /* home screen behind */
    int y0 = H - 24 - TRAY_H;
    uint32_t bar0 = ui_rgb(0x3B, 0x43, 0x52), bar1 = ui_rgb(0x15, 0x1A, 0x23);
    ui_fill_grad(0, y0, W, TRAY_H, bar0, bar1);
    uint32_t sheen = ui_tint(bar0, 108, 100);  /* faint linen bands, code-only */
    for (int y = y0 + 6; y < y0 + TRAY_H - 4; y += 8)
        ui_fill(0, y, W, 3, sheen);
    ui_fill(0, y0, W, 1, ui_rgb(0x93, 0x9E, 0xAE));
    int x0, iy;
    tray_geom(cnt, &x0, &iy);
    for (int i = 0; i < cnt; i++) {
        const k93_app *a = &apps[idxs[i]];
        int iw = ICON_PX, ih = ICON_PX;
        const uint32_t *img = k93_icon_by_name(a->ident, &iw, &ih);
        int x = x0 + i * TRAY_STEP;
        ui_blit_scaled(x, iy, TRAY_PX, TRAY_PX, img, iw, ih);
        if (i == tsel)
            ui_frame(x - 2, iy - 2, TRAY_PX + 4, TRAY_PX + 4, 12, 2,
                     ui_rgb(0xFF, 0xFF, 0xFF));
        char lab[16];
        snprintf(lab, sizeof lab, "%.9s", a->name);
        ui_text_sh(x + (TRAY_PX - ui_text_width(1, lab)) / 2, iy + TRAY_PX + 4,
                   1, lab, ui_rgb(0xE8, 0xEE, 0xF6), 0x060C18);
    }
    if (cnt == 0) {
        const char *msg = "No recent apps";
        ui_text_bold((W - ui_text_width(1, msg)) / 2, iy, 1, msg,
                     ui_rgb(0xE8, 0xEE, 0xF6), 0x060C18);
    }
    ui_statusbar("SpringBoard");
    ui_statusmsg("Left/Right  Enter open  Esc close");
    ui_flush();
}

static int tray_hit(int cnt, int mx, int my)
{
    if (cnt < 1 || mx < 0) return -1;
    int x0, iy;
    tray_geom(cnt, &x0, &iy);
    if (mx < x0 || my < iy - 4 || my > iy + TRAY_PX + 20) return -1;
    int i = (mx - x0) / TRAY_STEP;
    if (i >= cnt || mx > x0 + i * TRAY_STEP + TRAY_PX + 4) return -1;
    return i;
}

/* launching splash + foreground launch + bundle rescan. Returns the app's
 * exit status (2 = the app asked for the multitasking view). */
static int launch_idx(k93_app **papps, int *pn, int idx)
{
    k93_app *apps = *papps;
    int sel = idx;
    if (ui_is_graphical()) {
        draw_wallpaper();
        int W = ui_px_width(), H = ui_px_height();
        const uint32_t *img = k93_icon_by_name(apps[sel].ident, NULL, NULL);
        /* iOS 4.2 app-opening animation: icon zooms up, then launches */
        for (int i = 0; i < 5; i++) {
            int sz = ICON_PX + (ICON_PX * 3 / 2 - ICON_PX) * i / 4;
            draw_wallpaper();
            ui_blit_scaled((W - sz) / 2, (H - sz) / 2 - 20, sz, sz,
                           img, ICON_PX, ICON_PX);
            ui_flush();
            usleep(40000);
        }
        char msg[64];
        snprintf(msg, sizeof msg, "Launching %.40s", apps[sel].name);
        ui_text_bold((W - ui_text_width(1, msg)) / 2,
                     (H + ICON_PX) / 2, 1, msg,
                     ui_rgb(0xE8, 0xEE, 0xF6), 0x060C18);
        ui_flush();
    } else {
        ui_clear();
        ui_printf(1, 2, "Launching %s ...", apps[sel].name);
        ui_flush();
    }
    usleep(150000);
    recent_add(apps[sel].ident);
    unsetenv("K93_NO_SUPER_EXIT");        /* the child gets normal home policy */
    ui_shutdown();
    int rc = k93_app_launch(&apps[sel]);
    ui_init("SpringBoard");
    setenv("K93_NO_SUPER_EXIT", "1", 1);
    int st = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    if (rc != 0 && st != 2) {
        char msg[192];
        snprintf(msg, sizeof msg, "%s exited with status %d",
                 apps[sel].name, st > 0 ? st : rc);
        ui_msgbox("Application", msg);
    }
    /* refresh list: sideloaded apps may have appeared */
    free(apps);
    *papps = k93_xmalloc(sizeof(k93_app) * 256);
    *pn = k93_app_scan(*papps, 256);
    return st;
}

/* iOS-6-style app switcher; returns when the tray is closed */
static void tray_loop(k93_app **papps, int *pn)
{
    int tsel = 0;
    for (;;) {
        int idxs[TRAY_MAX];
        int cnt = tray_index(*papps, *pn, idxs);
        if (tsel > cnt - 1) tsel = cnt - 1;
        if (tsel < 0) tsel = 0;
        draw_tray(*papps, *pn, tsel);
        int k = UI_KEY_NONE;
        if (ui_is_graphical()) {
            int clk = ui_mouse_clicked(0);
            if (ui_mouse_clicked(1)) k = UI_KEY_ESC;      /* right = close */
            else if (clk) {
                int i = tray_hit(cnt, ui_mouse_x(), ui_mouse_y());
                if (i >= 0) { tsel = i; k = UI_KEY_ENTER; }
            }
        }
        if (k == UI_KEY_NONE) k = ui_getkey(250);
        switch (k) {
        case UI_KEY_LEFT:  case UI_KEY_UP:   if (tsel > 0) tsel--; break;
        case UI_KEY_RIGHT: case UI_KEY_DOWN: if (tsel < cnt - 1) tsel++; break;
        case UI_KEY_ENTER:
            if (cnt < 1 || tsel >= cnt) break;
            if (launch_idx(papps, pn, idxs[tsel]) == 2)
                continue;                /* app wants the switcher: stay */
            return;
        case UI_KEY_ESC: case UI_KEY_CTRL_C: case UI_KEY_HOME:
        case UI_KEY_SWITCHER:
            return;
        default: break;
        }
    }
}

/* iOS 4.0 folder helpers */
static void folder_create(k93_app **papps, int *pn, int appidx)
{
    k93_app *apps = *papps;
    const char *items[FOLDER_MAX + 2];
    int n = 0;
    for (int i = 0; i < nfolders && n < FOLDER_MAX; i++) items[n++] = folders[i].name;
    items[n++] = "New folder";
    items[n++] = "Cancel";
    int sel = ui_menu("Add to folder", items, n, 0);
    if (sel < 0 || sel == n - 1) return;
    int fi;
    if (sel == n - 2) {
        if (nfolders >= FOLDER_MAX) { ui_msgbox("Folders", "Too many folders."); return; }
        char name[64] = "";
        if (ui_input("Folder name", name, sizeof name) != 0 || !name[0]) return;
        fi = nfolders++;
        k93_strlcpy(folders[fi].name, name, sizeof folders[fi].name);
        folders[fi].n = 0;
    } else {
        fi = sel;
    }
    if (folders[fi].n >= FOLDER_APPS) { ui_msgbox("Folders", "Folder full."); return; }
    k93_strlcpy(folders[fi].idents[folders[fi].n++], apps[appidx].ident, 128);
    folders_save();
    ui_statusmsg("added to folder");
}

static void folder_view(k93_app **papps, int *pn, int fi)
{
    if (fi < 0 || fi >= nfolders) return;
    for (;;) {
        k93_app *apps = *papps;
        int n = *pn;
        char names[FOLDER_APPS][64];
        const char *items[FOLDER_APPS + 1];
        int appidx[FOLDER_APPS];
        int cnt = 0;
        for (int j = 0; j < folders[fi].n && cnt < FOLDER_APPS; j++)
            for (int i = 0; i < n; i++)
                if (!strcmp(apps[i].ident, folders[fi].idents[j])) {
                    k93_strlcpy(names[cnt], apps[i].name, sizeof names[0]);
                    items[cnt] = names[cnt];
                    appidx[cnt] = i;
                    cnt++;
                    break;
                }
        items[cnt++] = "Back";
        int s = ui_menu(folders[fi].name, items, cnt, 0);
        if (s < 0 || s == cnt - 1) return;
        launch_idx(papps, pn, appidx[s]);
        build_dlist(*papps, *pn);
        if (fi >= nfolders) return;
    }
}

int main(void)
{
    if (ui_init("SpringBoard") < 0) return 1;
    setenv("K93_NO_SUPER_EXIT", "1", 1); /* super key here: tap=page 1,
                                            hold=multitasking tray */
    k93_app *apps = k93_xmalloc(sizeof(k93_app) * 256);
    int n = k93_app_scan(apps, 256);
    k93_log("springboard", "scanned %d application bundles", n);
    folders_load();
    build_dlist(apps, n);
    int sel = 0;
    char hint[96] = "";
    for (;;) {
        if (ndlist == 0) {
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
            if (ui_is_graphical()) { ui_mouse_clicked(0); ui_mouse_clicked(1); }
            int k = ui_getkey(250);
            if (k == UI_KEY_ESC || k == UI_KEY_CTRL_C) break;
            if (k == UI_KEY_F12) break;
            continue;
        }
        if (sel >= ndlist) sel = ndlist - 1;
        if (sel < 0) sel = 0;
        if (ui_is_graphical()) draw_home_gui(apps, n, sel, hint);
        else draw_home(apps, n, sel, hint);
        int k = UI_KEY_NONE;
        if (ui_is_graphical()) {
            if (ui_mouse_clicked(1)) continue;       /* right: nothing to
                                                        go back to */
            if (ui_mouse_clicked(0)) {
                int idx = gui_hit_icon(sel);
                if (idx >= 0) { sel = idx; k = UI_KEY_ENTER; }
            }
        }
        if (k == UI_KEY_NONE) k = ui_getkey(250);
        hint[0] = 0;
        int cols = ui_is_graphical() ? GRID_COLS : 1;
        switch (k) {
        case UI_KEY_UP:   sel -= cols; if (sel < 0) sel = 0; break;
        case UI_KEY_DOWN: sel += cols; if (sel >= ndlist) sel = ndlist - 1; break;
        case UI_KEY_LEFT: if (sel > 0) sel--; break;
        case UI_KEY_RIGHT:if (sel < ndlist - 1) sel++; break;
        case UI_KEY_HOME: sel = 0; break;
        case UI_KEY_PGUP: sel -= cols * 4; if (sel < 0) sel = 0; break;
        case UI_KEY_PGDN: sel += cols * 4; if (sel >= ndlist) sel = ndlist - 1; break;
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
        case UI_KEY_SWITCHER:
            tray_loop(&apps, &n);
            build_dlist(apps, n);
            break;
        case UI_KEY_CHAR_BASE + 'f':
            if (dlist[sel] >= 0) {
                folder_create(&apps, &n, dlist[sel]);
                build_dlist(apps, n);
            }
            break;
        case UI_KEY_ENTER: {
            if (dlist[sel] < 0) {
                folder_view(&apps, &n, -1 - dlist[sel]);
                build_dlist(apps, n);
            } else if (launch_idx(&apps, &n, dlist[sel]) == 2) {
                tray_loop(&apps, &n);
                build_dlist(apps, n);
            }
            break;
        }
        default: break;
        }
    }
    free(apps);
    ui_shutdown();
    return 0;
}
