/* k93app.h - application bundle registry + launcher (clean-room SpringBoard
 * support layer). App layout mirrors the iOS convention observed in the
 * supplied rootfs: /Applications/<Name>.app/<Executable> + Info.plist, but the
 * executables are native x86_64 ELF, since the original binaries are ARMv7. */
#ifndef K93APP_H
#define K93APP_H
#include <stddef.h>

typedef struct {
    char name[128];                      /* CFBundleDisplayName or CFBundleName */
    char ident[128];                     /* CFBundleIdentifier */
    char exe[512];                       /* absolute path to executable */
    char dir[512];                       /* .../Foo.app */
    char version[32];
    int  sideloaded;                     /* installed from a third-party .ipa */
} k93_app;

/* scan app bundles under /Applications and the sideload registry; returns count */
int  k93_app_scan(k93_app *out, int max);
/* fork/exec the app in the foreground; returns child exit status */
int  k93_app_launch(const k93_app *a);
/* install a .ipa (K93-compatible zip bundle) into /Applications; 0 on success */
int  k93_app_install_ipa(const char *ipapath, char *err, size_t errlen);
/* uninstall by bundle identifier */
int  k93_app_unregister(const char *ident);
/* path of the app's Info.plist */
void k93_app_info_path(const k93_app *a, char *out, size_t cap);

#define K93_APPS_DIR      "/Applications"
#define K93_REGISTRY      "/var/db/k93/apps.log"
#define K93_DOWNLOADS_DIR "/var/mobile/Media/Downloads"

#endif
