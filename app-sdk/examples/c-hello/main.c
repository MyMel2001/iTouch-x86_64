/* hello - smallest possible K93OS app (C).
 * Build:  k93sdk build . && k93sdk bundle . */
#include "k93ui.h"

int main(void)
{
    if (ui_init("Hello") < 0) return 1;
    const char *items[] = { "Say hello", "Quit" };
    for (;;) {
        int sel = ui_menu("Hello", items, 2, 0);
        if (sel < 0 || sel == 1) break;
        ui_msgbox("Hello", "Hello from C on K93OS-x86!");
    }
    ui_shutdown();
    return 0;
}
