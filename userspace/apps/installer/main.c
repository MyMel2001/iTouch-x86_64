/* Installer.app - third-party .ipa sideloading (clean-room).
 * This replaces MobileStore/AppStore functionality for sources outside any
 * store: point it at an .ipa (from /var/mobile/Media/Downloads or any path),
 * it validates the bundle and installs it into /Applications. No Apple
 * services involved; root access makes this possible by design. */
#define _GNU_SOURCE
#include "k93ui.h"
#include "k93util.h"
#include "k93app.h"
#include "k93zip.h"
#include "k93plist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

static void inspect_ipa(const char *path)
{
    char body[2048];
    k93_zip *z = k93_zip_open(path);
    if (!z) { ui_msgbox("Installer", "Cannot open archive (not a zip?)"); return; }
    int n = k93_zip_count(z);
    int pidx = k93_zip_find(z, "Info.plist");
    size_t o = (size_t)snprintf(body, sizeof body, "Archive: %s\nEntries: %d\n\n", path, n);
    /* list first few Payload entries */
    int shown = 0;
    for (int i = 0; i < n && shown < 12 && o + 160 < sizeof body; i++) {
        char name[256];
        uint64_t a, b; int c, d;
        if (k93_zip_stat(z, i, name, sizeof name, &a, &b, &c, &d) == 0 &&
            k93_starts_with(name, "Payload/")) {
            o += (size_t)snprintf(body + o, sizeof body - o, "  %8llu  %s\n",
                                  (unsigned long long)a, name);
            shown++;
        }
    }
    if (pidx >= 0) {
        char tmp[] = "/tmp/k93-inspect-XXXXXX";
        int tfd = mkstemp(tmp);
        if (tfd >= 0) {
            if (k93_zip_extract_fd(z, pidx, tfd) == 0) {
                close(tfd);
                char err[128];
                k93_plist *p = k93_plist_parse_file(tmp, err, sizeof err);
                if (p && o + 512 < sizeof body) {
                    o += (size_t)snprintf(body + o, sizeof body - o,
                                          "\nBundle info:\n  Name        : %s\n"
                                          "  Identifier  : %s\n  Version     : %s\n  Executable  : %s\n",
                                          k93_plist_dict_str(p, "CFBundleName", "?"),
                                          k93_plist_dict_str(p, "CFBundleIdentifier", "?"),
                                          k93_plist_dict_str(p, "CFBundleShortVersionString", "?"),
                                          k93_plist_dict_str(p, "CFBundleExecutable", "(none)"));
                }
                k93_plist_free(p);
            } else close(tfd);
            unlink(tmp);
        }
    } else if (o + 128 < sizeof body) {
        o += (size_t)snprintf(body + o, sizeof body - o, "\n(no Info.plist found)\n");
    }
    k93_zip_close(z);
    ui_textview("ipa contents", body);
}

int main(void)
{
    if (ui_init("Installer") < 0) return 1;
    char msg[192] = "";
    for (;;) {
        /* gather candidate ipas */
        const char *dirs[] = { K93_DOWNLOADS_DIR, "/var/mobile", "/tmp", "/", NULL };
        char entries[64][256];
        const char *items[70];
        int n = 0;
        for (int di = 0; dirs[di] && n < 60; di++) {
            DIR *d = opendir(dirs[di]);
            if (!d) continue;
            struct dirent *e;
            while ((e = readdir(d)) && n < 60) {
                size_t l = strlen(e->d_name);
                if (l < 5 || strcasecmp(e->d_name + l - 4, ".ipa")) continue;
                snprintf(entries[n], sizeof entries[n], "%s/%s", dirs[di], e->d_name);
                items[n] = entries[n];
                n++;
            }
            closedir(d);
        }
        const char *extra[] = { "Enter a path manually ...", "Back" };
        items[n] = extra[0];
        items[n + 1] = extra[1];
        if (msg[0]) { ui_statusmsg(msg); msg[0] = 0; }

        int sel = ui_menu("Installer - select .ipa", items, n + 2, 0);
        if (sel < 0 || sel == n + 1) break;
        char chosen[512];
        if (sel == n) {
            k93_strlcpy(chosen, K93_DOWNLOADS_DIR "/", sizeof chosen);
            if (ui_input("Path to .ipa", chosen, sizeof chosen) != 0) continue;
        } else {
            k93_strlcpy(chosen, entries[sel], sizeof chosen);
        }

        const char *acts[] = { "Install", "Inspect contents", "Cancel" };
        int a = ui_menu(chosen, acts, 3, 0);
        if (a == 0) {
            ui_statusmsg("Installing ...");
            char err[256] = "";
            if (k93_app_install_ipa(chosen, err, sizeof err) == 0) {
                snprintf(msg, sizeof msg, "installed OK");
                ui_msgbox("Installer", "Installed.\n\nThe app is now on the SpringBoard grid.");
            } else {
                ui_msgbox("Installer failed", err);
            }
        } else if (a == 1) {
            inspect_ipa(chosen);
        }
    }
    ui_shutdown();
    return 0;
}
