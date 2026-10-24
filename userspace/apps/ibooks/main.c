/* iBooks.app - clean-room counterpart of iOS 4.0 iBooks.
 * iOS 4 iBooks renders PDF/ePub books; this build reads plain-text .txt
 * books only (no PDF/EPUB renderer is shipped). Books live in
 * /var/mobile/Library/iBooks; /var/mobile/Media/Downloads is the source dir. */
#define _GNU_SOURCE
#include "k93ui.h"
#include "k93util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#define DIRN "/var/mobile/Library/iBooks"
#define DOWN "/var/mobile/Media/Downloads"

static int list_books(char names[64][128])
{
    int n = 0;
    DIR *d = opendir(DIRN);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < 64) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".txt")) continue;
        k93_strlcpy(names[n], e->d_name, 128);
        names[n][l - 4] = 0;
        n++;
    }
    if (d) closedir(d);
    return n;
}

static void import_ui(void)
{
    char dnames[64][128];
    const char *items[66];
    int dn = 0;
    DIR *d = opendir(DOWN);
    struct dirent *e;
    while (d && (e = readdir(d)) && dn < 64) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".txt")) continue;
        k93_strlcpy(dnames[dn], e->d_name, 128);
        dn++;
    }
    if (d) closedir(d);
    if (dn == 0) { ui_msgbox("iBooks", "No .txt files in Downloads."); return; }
    for (int i = 0; i < dn; i++) items[i] = dnames[i];
    items[dn] = "Cancel";
    int sel = ui_menu("Import from Downloads", items, dn + 1, 0);
    if (sel < 0 || sel == dn) return;
    char src[1200], dst[1200];
    snprintf(src, sizeof src, "%s/%s", DOWN, dnames[sel]);
    snprintf(dst, sizeof dst, "%s/%s", DIRN, dnames[sel]);
    ui_statusmsg(k93_copy_file(src, dst, 0644) == 0 ? "imported" : "copy failed");
}

int main(void)
{
    if (ui_init("iBooks") < 0) return 1;
    k93_mkdir_p(DIRN, 0777);
    for (;;) {
        char names[64][128];
        const char *items[66];
        int n = list_books(names);
        for (int i = 0; i < n; i++) items[i] = names[i];
        items[n++] = "+ Import from Downloads";
        items[n++] = "Back";
        int sel = ui_menu("iBooks", items, n, 0);
        if (sel < 0 || sel == n - 1) break;
        if (sel == n - 2) { import_ui(); continue; }
        char path[256];
        snprintf(path, sizeof path, "%s/%s.txt", DIRN, names[sel]);
        size_t l = 0;
        char *t = k93_read_file(path, &l);
        if (!t) { ui_msgbox("iBooks", "Cannot read file"); continue; }
        ui_textview(names[sel], t);
        free(t);
    }
    ui_shutdown();
    return 0;
}
