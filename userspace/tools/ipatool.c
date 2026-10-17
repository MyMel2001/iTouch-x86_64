/* k93ipatool - list/inspect/install .ipa bundles. Uses libk93 app registry. */
#include <k93app.h>
#include <k93plist.h>
#include <k93util.h>
#include <k93zip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int usage(void)
{
    fprintf(stderr,
        "usage: k93ipatool list  <file.ipa>      show bundle metadata\n"
        "       k93ipatool inst  <file.ipa>      install into /Applications\n"
        "       k93ipatool deb   <file.deb>      extract Debian package into /\n"
        "       k93ipatool apps                   list installed bundles\n");
    return 2;
}

static int list(const char *path)
{
    k93_zip *z = k93_zip_open(path);
    if (!z) { fprintf(stderr, "k93ipatool: cannot open %s\n", path); return 1; }
    int idx = k93_zip_find(z, "Info.plist");
    if (idx < 0) { fprintf(stderr, "k93ipatool: no Info.plist inside\n"); k93_zip_close(z); return 1; }

    /* pull the plist through a temp file (small) */
    char tmp[] = "/tmp/k93ipaXXXXXX";
    /* extract into memory via zip to a temp file */
    int fd = mkstemp(tmp);
    if (fd < 0) { perror("mkstemp"); k93_zip_close(z); return 1; }
    int rc = k93_zip_extract_fd(z, idx, fd);
    close(fd);
    if (rc != 0) { fprintf(stderr, "k93ipatool: extract failed\n"); unlink(tmp); k93_zip_close(z); return 1; }

    size_t len = 0;
    char *text = k93_read_file(tmp, &len);
    unlink(tmp);
    if (!text) { k93_zip_close(z); return 1; }

    struct k93_plist *pl;
    char perr[128] = "";
    pl = k93_plist_parse(text, len, perr, sizeof perr);
    if (!pl) { fprintf(stderr, "k93ipatool: malformed or binary plist (binary plists unsupported): %s\n",
                       perr);
               free(text); k93_zip_close(z); return 1; }
    const char *keys[] = { "CFBundleIdentifier", "CFBundleDisplayName", "CFBundleVersion",
                           "CFBundleExecutable", "MinimumOSVersion" };
    printf("file: %s\nentries: %d\n", path, k93_zip_count(z));
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const char *v = k93_plist_dict_str(pl, keys[i], NULL);
        if (v) printf("  %-22s%s\n", keys[i], v);
    }
    k93_plist_free(pl);
    free(text);
    k93_zip_close(z);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return usage();
    if (!strcmp(argv[1], "list") && argc == 3) return list(argv[2]);
    if (!strcmp(argv[1], "inst") && argc == 3) {
        char err[256] = "";
        if (k93_app_install_ipa(argv[2], err, sizeof(err)) != 0) {
            fprintf(stderr, "k93ipatool: install failed: %s\n", err[0] ? err : "unknown error");
            return 1;
        }
        printf("installed %s\n", argv[2]);
        return 0;
    }
    if (!strcmp(argv[1], "deb") && argc == 3) {
        char err[256] = "";
        if (k93_app_install_deb(argv[2], err, sizeof(err)) != 0) {
            fprintf(stderr, "k93ipatool: deb install failed: %s\n", err[0] ? err : "unknown error");
            return 1;
        }
        printf("installed deb %s\n", argv[2]);
        return 0;
    }
    if (!strcmp(argv[1], "apps")) {
        k93_app apps[128];
        int n = k93_app_scan(apps, 128);
        for (int i = 0; i < n; i++)
            printf("%-28s%-24s%s%s\n", apps[i].ident, apps[i].name, apps[i].version,
                   apps[i].sideloaded ? "  [sideloaded]" : "");
        return 0;
    }
    return usage();
}
