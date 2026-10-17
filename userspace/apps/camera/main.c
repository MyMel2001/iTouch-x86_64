/* Camera.app - clean-room counterpart of Camera.app (4.3.4 rootfs).
 * There is no capture hardware in the target VM, so this app reports what it
 * found: /dev/video* (V4L2) nodes. Ceiling: capture+encode not implemented. */
#define _GNU_SOURCE
#include "k93ui.h"
#include "k93util.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    if (ui_init("Camera") < 0) return 1;
    char body[1024];
    size_t o = (size_t)snprintf(body, sizeof body, "Camera\n\n");
    int found = 0;
    for (int i = 0; i < 4; i++) {
        char p[32];
        snprintf(p, sizeof p, "/dev/video%d", i);
        if (access(p, F_OK) == 0) {
            o += (size_t)snprintf(body + o, sizeof body - o, "found %s\n", p);
            found++;
        }
    }
    if (!found) {
        o += (size_t)snprintf(body + o, sizeof body - o,
                              "No V4L2 capture device present.\n\n"
                              "The QEMU test machine has no camera. On hardware with\n"
                              "/dev/video0 this app would capture frames (V4L2 API);\n"
                              "that driver path is not implemented in this build.\n");
    }
    ui_msgbox("Camera", body);
    ui_shutdown();
    return 0;
}
