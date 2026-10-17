/* Store.app - placeholder for MobileStore (iTunes Store).
 * Deliberately NOT a working store: the brief says no App Store port.
 * Any third-party distribution happens through Installer.app (.ipa sideload). */
#define _GNU_SOURCE
#include "k93ui.h"
#include <stdio.h>

int main(void)
{
    if (ui_init("Store") < 0) return 1;
    ui_msgbox("Store",
              "The iTunes/Mobile Store is not ported to K93OS-x86 by design.\n\n"
              "Install applications from third-party sources with the\n"
              "Installer app (sideloading .ipa bundles), like Android does.\n\n"
              "There is no App Store counterpart and no DRM.");
    ui_shutdown();
    return 0;
}
