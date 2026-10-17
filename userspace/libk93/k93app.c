/* k93app.c - bundle scan/launch/sideload (clean-room). */
#define _GNU_SOURCE
#include "k93app.h"
#include "k93util.h"
#include "k93plist.h"
#include "k93zip.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

void k93_app_info_path(const k93_app *a, char *out, size_t cap)
{
    snprintf(out, cap, "%s/Info.plist", a->dir);
}

static int load_info(const char *dir, k93_app *out)
{
    char ip[600];
    snprintf(ip, sizeof ip, "%s/Info.plist", dir);
    char err[128];
    k93_plist *p = k93_plist_parse_file(ip, err, sizeof err);
    if (!p) return -1;
    const char *name = k93_plist_dict_str(p, "CFBundleDisplayName", NULL);
    if (!name) name = k93_plist_dict_str(p, "CFBundleName", NULL);
    if (!name) name = "Unknown";
    const char *ident = k93_plist_dict_str(p, "CFBundleIdentifier", "com.k93.unknown");
    const char *exe = k93_plist_dict_str(p, "CFBundleExecutable", NULL);
    const char *ver = k93_plist_dict_str(p, "CFBundleShortVersionString", "1.0");
    k93_strlcpy(out->name, name, sizeof out->name);
    k93_strlcpy(out->ident, ident, sizeof out->ident);
    k93_strlcpy(out->version, ver, sizeof out->version);
    k93_strlcpy(out->dir, dir, sizeof out->dir);
    if (exe) snprintf(out->exe, sizeof out->exe, "%s/%s", dir, exe);
    else out->exe[0] = 0;
    k93_plist_free(p);
    return out->exe[0] ? 0 : -1;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(((const k93_app *)a)->name, ((const k93_app *)b)->name);
}

int k93_app_scan(k93_app *out, int max)
{
    int n = 0;
    DIR *d = opendir(K93_APPS_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < max) {
            size_t l = strlen(e->d_name);
            if (l < 5 || strcmp(e->d_name + l - 4, ".app")) continue;
            char dir[512];
            snprintf(dir, sizeof dir, "%s/%s", K93_APPS_DIR, e->d_name);
            struct stat st;
            if (stat(dir, &st) < 0 || !S_ISDIR(st.st_mode)) continue;
            k93_app *a = &out[n];
            memset(a, 0, sizeof *a);
            if (load_info(dir, a) == 0) {
                /* flag sideloaded apps via registry */
                char *reg = k93_read_file(K93_REGISTRY, NULL);
                if (reg && strstr(reg, a->ident)) a->sideloaded = 1;
                free(reg);
                n++;
            }
        }
        closedir(d);
    }
    qsort(out, (size_t)n, sizeof(k93_app), by_name);
    return n;
}

int k93_app_launch(const k93_app *a)
{
    struct stat st;
    if (!a->exe[0] || stat(a->exe, &st) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setenv("K93_APP", a->name, 1);
        setenv("K93_BUNDLE_ID", a->ident, 1);
        /* foreground apps get a fresh session so Ctrl-C reaches them */
        setsid();
        execl(a->exe, a->exe, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) ;
    return status;
}

int k93_app_unregister(const char *ident)
{
    k93_app apps[256];
    int n = k93_app_scan(apps, 256);
    for (int i = 0; i < n; i++) {
        if (!strcmp(apps[i].ident, ident)) {
            if (k93_rm_tree(apps[i].dir) < 0) return -1;
            k93_log("installer", "uninstalled %s (%s)", apps[i].name, ident);
            return 0;
        }
    }
    return -1;
}

/* magic sniffing so we can explain ARM iOS binaries clearly */
static int elf_or_macho(const char *path, char *what, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    unsigned char m[8] = {0};
    ssize_t r = read(fd, m, 8);
    close(fd);
    if (r < 4) return -1;
    uint32_t mag32 = (uint32_t)m[0] | ((uint32_t)m[1] << 8) | ((uint32_t)m[2] << 16) | ((uint32_t)m[3] << 24);
    if (mag32 == 0x464C457Fu) { k93_strlcpy(what, "ELF", cap); return 0; }
    if (mag32 == 0xFEEDFACEu || mag32 == 0xFEEDFACFu || mag32 == 0xCEFAEDFEu || mag32 == 0xCFFAEDFEu) {
        k93_strlcpy(what, "Mach-O", cap);
        return 1;
    }
    if (mag32 == 0xCAFEBABEu || mag32 == 0xBEBAFECAu) { k93_strlcpy(what, "Mach-O fat", cap); return 1; }
    k93_strlcpy(what, "unknown", cap);
    return 2;
}

int k93_app_install_ipa(const char *ipapath, char *err, size_t errlen)
{
    k93_zip *z = k93_zip_open(ipapath);
    if (!z) { snprintf(err, errlen, "not a readable zip/ipa: %s", ipapath); return -1; }

    int pidx = -1;
    for (int i = 0; i < k93_zip_count(z); i++) {
        char name[512];
        uint64_t a, b; int c, d;
        if (k93_zip_stat(z, i, name, sizeof name, &a, &b, &c, &d) == 0 &&
            k93_starts_with(name, "Payload/") && strstr(name, ".app/")) {
            pidx = i; break;
        }
    }
    if (pidx < 0) {
        snprintf(err, errlen, "archive has no Payload/*.app (not an ipa)");
        k93_zip_close(z);
        return -1;
    }

    /* extract into a staging dir first so failures never half-install */
    const char *stage = "/var/db/k93/staging";
    k93_rm_tree(stage);
    k93_mkdir_p(stage, 0755);
    int n = k93_zip_extract_all(z, stage);
    k93_zip_close(z);
    if (n < 0) { snprintf(err, errlen, "zip extraction failed"); return -1; }

    /* find the .app dir (inside Payload/ for a real ipa, or at the root) */
    char appdir[600] = "";
    char payload[640];
    snprintf(payload, sizeof payload, "%s/Payload", stage);
    const char *scanroots[] = { payload, stage };
    for (int r = 0; r < 2 && !appdir[0]; r++) {
        DIR *d = opendir(scanroots[r]);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l > 4 && !strcmp(e->d_name + l - 4, ".app")) {
                snprintf(appdir, sizeof appdir, "%s/%s", scanroots[r], e->d_name);
                break;
            }
        }
        closedir(d);
    }
    if (!appdir[0]) { snprintf(err, errlen, "no .app bundle inside Payload"); return -1; }

    k93_app info;
    memset(&info, 0, sizeof info);
    if (load_info(appdir, &info) < 0) {
        snprintf(err, errlen, "bundle has no valid Info.plist");
        k93_rm_tree(stage);
        return -1;
    }
    if (info.exe[0]) {
        char what[32];
        int kind = elf_or_macho(info.exe, what, sizeof what);
        if (kind == 1) {
            snprintf(err, errlen,
                     "%s bundle is %s (ARM iOS). K93OS runs native x86_64 ELF; "
                     "repackage with a native binary to sideload here.",
                     what, what);
            k93_rm_tree(stage);
            return -1;
        }
        if (kind != 0) {
            snprintf(err, errlen, "bundle executable is not ELF x86_64");
            k93_rm_tree(stage);
            return -1;
        }
    }

    char dest[600];
    snprintf(dest, sizeof dest, "%s/%s", K93_APPS_DIR, strrchr(appdir, '/') + 1);
    k93_rm_tree(dest);
    if (k93_copy_tree(appdir, dest) < 0) {
        snprintf(err, errlen, "cannot copy into %s (read-only root?)", K93_APPS_DIR);
        k93_rm_tree(stage);
        return -1;
    }
    /* ensure executables are executable */
    if (info.exe[0]) {
        char final_exe[640];
        snprintf(final_exe, sizeof final_exe, "%s/%s", dest, strrchr(info.exe, '/'));
        chmod(final_exe, 0755);
    }
    k93_rm_tree(stage);

    /* registry entry (provenance: third-party source) */
    FILE *f = fopen(K93_REGISTRY, "a");
    if (f) {
        fprintf(f, "<!-- %s %s from %s -->\n", info.ident, info.version, ipapath);
        fclose(f);
    }
    k93_log("installer", "installed %s (%s) from %s", info.name, info.ident, ipapath);
    return 0;
}

/* install a Debian binary package: validate the ar archive, extract the
 * payload into a staging dir, then merge into / - the open-by-design model
 * as .ipa sideloading (no maintainer scripts, no signature checks).
 * Staging also sidesteps busybox dpkg-deb quirks (no parent-dir creation,
 * "./"-prefixed members). */
int k93_app_install_deb(const char *debpath, char *err, size_t errlen)
{
    /* validate ar magic before touching the fs */
    int fd = open(debpath, O_RDONLY);
    if (fd < 0) { snprintf(err, errlen, "cannot open %s", debpath); return -1; }
    char hdr[8] = {0};
    ssize_t r = read(fd, hdr, 8);
    close(fd);
    if (r < 8 || memcmp(hdr, "!<arch>\n", 8)) {
        snprintf(err, errlen, "not a Debian archive (missing ar magic)");
        return -1;
    }
    const char *stage = "/var/db/k93/debstaging";
    k93_rm_tree(stage);
    k93_mkdir_p(stage, 0755);
    pid_t pid = fork();
    if (pid < 0) { snprintf(err, errlen, "fork failed"); return -1; }
    if (pid == 0) {
        execl("/bin/busybox", "busybox", "dpkg-deb", "-x", debpath, stage, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) ;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        snprintf(err, errlen, "dpkg-deb extraction failed (corrupt package?)");
        k93_rm_tree(stage);
        return -1;
    }
    if (k93_copy_tree(stage, "/") < 0) {
        snprintf(err, errlen, "cannot merge package into / (read-only root?)");
        k93_rm_tree(stage);
        return -1;
    }
    k93_rm_tree(stage);
    FILE *f = fopen(K93_REGISTRY, "a");
    if (f) { fprintf(f, "<!-- deb installed from %s -->\n", debpath); fclose(f); }
    k93_log("installer", "installed deb %s", debpath);
    return 0;
}
