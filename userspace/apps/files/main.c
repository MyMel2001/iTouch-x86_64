/* Files.app - K93OS-x86 file browser with full root access (clean-room
 * counterpart of the file browsing users expect from a desktop OS; iOS 4.3.4
 * had no such app, which is one of the freedoms this OS adds on purpose).
 * Can also install .ipa bundles and convert .dmg images to raw disks. */
#define _GNU_SOURCE
#include "k93ui.h"
#include "k93util.h"
#include "k93app.h"
#include "k93zip.h"
#include "k93dmg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define MAXENT 512

struct ent { char name[256]; int isdir; long long size; };

static int cmp_ent(const void *a, const void *b)
{
    const struct ent *x = a, *y = b;
    if (x->isdir != y->isdir) return y->isdir - x->isdir;
    return strcasecmp(x->name, y->name);
}

static int read_dir(const char *path, struct ent *out, int max, int *showhidden)
{
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) && n < max) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!*showhidden && e->d_name[0] == '.') continue;
        k93_strlcpy(out[n].name, e->d_name, sizeof out[n].name);
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(full, &st) == 0) {
            out[n].isdir = S_ISDIR(st.st_mode);
            out[n].size = (long long)st.st_size;
        }
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, cmp_ent);
    return n;
}

static void view_file(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) { ui_msgbox("Files", "Cannot stat file"); return; }
    if (st.st_size > 512 * 1024) { ui_msgbox("Files", "File too large to display (>512 KiB)"); return; }
    size_t len = 0;
    char *txt = k93_read_file(path, &len);
    if (!txt) { ui_msgbox("Files", "Cannot read file"); return; }
    int binary = 0;
    for (size_t i = 0; i < len && i < 4096; i++)
        if ((unsigned char)txt[i] < 9 || ((unsigned char)txt[i] > 13 && (unsigned char)txt[i] < 32)) { binary = 1; break; }
    if (binary) {
        char body[2048];
        char hex[1024] = "";
        size_t o = 0;
        for (size_t i = 0; i < len && i < 256 && o + 4 < sizeof hex; i++)
            o += (size_t)snprintf(hex + o, sizeof hex - o, "%02x ", (unsigned char)txt[i]);
        snprintf(body, sizeof body, "Binary file (%lld bytes). First bytes:\n\n%s", (long long)st.st_size, hex);
        ui_textview(path, body);
    } else {
        ui_textview(path, txt);
    }
    free(txt);
}

static void install_ipa_ui(const char *path)
{
    ui_statusmsg("Installing ipa ...");
    char err[256] = "";
    int rc = k93_app_install_ipa(path, err, sizeof err);
    if (rc == 0) ui_msgbox("Installer", "Application installed to /Applications.\nIt appears on the SpringBoard grid.");
    else ui_msgbox("Installer failed", err);
}

static void install_deb_ui(const char *path)
{
    ui_statusmsg("Installing deb ...");
    char err[256] = "";
    int rc = k93_app_install_deb(path, err, sizeof err);
    if (rc == 0) ui_msgbox("Installer", "Debian package extracted into /.\nFiles land where the package places them.");
    else ui_msgbox("Installer failed", err);
}

/* dispatch on extension: .ipa -> app bundle install, .deb -> package extract */
static void install_ui(const char *path)
{
    size_t l = strlen(path);
    if (l > 4 && !strcasecmp(path + l - 4, ".ipa")) install_ipa_ui(path);
    else if (l > 4 && !strcasecmp(path + l - 4, ".deb")) install_deb_ui(path);
    else ui_msgbox("Installer", "Unknown package type.\nSupported: .ipa (app), .deb (package)");
}

static void install_dmg_ui(const char *path)
{
    char out[1024];
    snprintf(out, sizeof out, "%s.rawimg", path);
    char q[256];
    snprintf(q, sizeof q, "Convert DMG to raw image?\n\nfrom: %s\nto  : %s\n\nThe raw image can be written to a disk\nwith InstallToHD.", path, out);
    if (!ui_confirm("Disk Image", q)) return;
    ui_statusmsg("Converting dmg (this can take a while) ...");
    k93_dmg *d = k93_dmg_open(path);
    if (!d) { ui_msgbox("Disk Image", "Cannot open dmg (only UDZO images are supported)"); return; }
    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { k93_dmg_close(d); ui_msgbox("Disk Image", "Cannot create output file"); return; }
    int rc = k93_dmg_extract(d, fd);
    close(fd);
    k93_dmg_close(d);
    if (rc == 0) {
        char msg[512];
        struct stat st;
        stat(out, &st);
        char sz[32];
        k93_human_size(sz, sizeof sz, (unsigned long long)st.st_size);
        snprintf(msg, sizeof msg, "Wrote %s (%s)", out, sz);
        ui_msgbox("Disk Image", msg);
    } else {
        ui_msgbox("Disk Image", "Conversion failed (unsupported block codec?)");
    }
}

int main(int argc, char **argv)
{
    if (ui_init("Files") < 0) return 1;
    char cur[1024] = "/var/mobile";
    if (argc > 1) k93_strlcpy(cur, argv[1], sizeof cur);
    struct ent *ents = k93_xmalloc(sizeof(struct ent) * MAXENT);
    int showhidden = 1;
    char msg[128] = "";

    for (;;) {
        int n = read_dir(cur, ents, MAXENT, &showhidden);
        if (n < 0) { ui_msgbox("Files", "Cannot open directory"); k93_strlcpy(cur, "/", sizeof cur); continue; }
        char title[128];
        snprintf(title, sizeof title, "Files: %s", cur);
        const char **items = k93_xmalloc(sizeof(char *) * (size_t)(n + 2));
        for (int i = 0; i < n; i++) {
            items[i] = k93_xmalloc(320);
            char sz[24] = "";
            if (!ents[i].isdir) {
                char h[24];
                k93_human_size(h, sizeof h, (unsigned long long)ents[i].size);
                snprintf(sz, sizeof sz, " %10s", h);
            } else {
                snprintf(sz, sizeof sz, " %10s", "<dir>");
            }
            ((char *)items[i])[0] = 0;
            snprintf((char *)items[i], 320, "%s%s", ents[i].name, sz);
        }
        items[n] = k93_strdup("..  (up one level)");
        items[n + 1] = k93_strdup("+ New folder    * Delete    ~ Hidden files toggle");

        if (msg[0]) { ui_statusmsg(msg); msg[0] = 0; }
        int sel = ui_menu(title, items, n + 2, 0);
        for (int i = 0; i < n + 2; i++) free((void *)items[i]);
        free(items);

        if (sel < 0 || sel == n + 1) { /* footer action row */
            if (sel == n + 1) {
                const char *acts[] = { "Cancel", "New folder", "Toggle hidden files" };
                int a = ui_menu("Actions", acts, 3, 0);
                if (a == 1) {
                    char name[128] = "New Folder";
                    if (ui_input("Folder name", name, sizeof name) == 0) {
                        char p[1200];
                        snprintf(p, sizeof p, "%s/%s", cur, name);
                        if (mkdir(p, 0755) < 0) snprintf(msg, sizeof msg, "mkdir failed: %s", strerror(errno));
                        else snprintf(msg, sizeof msg, "created %s", name);
                    }
                } else if (a == 2) {
                    showhidden = !showhidden;
                }
            } else {
                break;
            }
            continue;
        }
        if (sel == n) {                                   /* up */
            char *slash = strrchr(cur, '/');
            if (slash && slash != cur) *slash = 0;
            else k93_strlcpy(cur, "/", sizeof cur);
            continue;
        }
        char full[2048];
        snprintf(full, sizeof full, "%s/%s", cur, ents[sel].name);
        if (ents[sel].isdir) {
            k93_strlcpy(cur, full, sizeof cur);
            continue;
        }
        /* file action menu */
        const char *fa[] = { "Open / View", "Install (ipa/deb)", "Convert (dmg)", "Delete", "Cancel" };
        int f = ui_menu(ents[sel].name, fa, 5, 0);
        if (f == 0) view_file(full);
        else if (f == 1) install_ui(full);
        else if (f == 2) install_dmg_ui(full);
        else if (f == 3) {
            char q[512];
            snprintf(q, sizeof q, "Really delete?\n%s", full);
            if (ui_confirm("Delete", q)) {
                if (k93_rm_tree(full) < 0) snprintf(msg, sizeof msg, "delete failed: %s", strerror(errno));
                else snprintf(msg, sizeof msg, "deleted %s", ents[sel].name);
            }
        }
    }
    free(ents);
    ui_shutdown();
    return 0;
}
