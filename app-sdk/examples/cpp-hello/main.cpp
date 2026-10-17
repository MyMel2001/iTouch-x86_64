/* hello - K93OS app in C++ using the same libk93 toolkit. */
extern "C" {
#include "k93ui.h"
}
#include <string>
#include <vector>

int main()
{
    if (ui_init("HelloCpp") < 0) return 1;
    std::vector<std::string> msgs = {
        "Hello from C++ on K93OS-x86!",
        "ui_menu + ui_msgbox are plain C functions -",
        "any language that can call C can build K93OS apps.",
    };
    const char *items[] = { "Show messages", "Quit" };
    for (;;) {
        int sel = ui_menu("HelloCpp", items, 2, 0);
        if (sel < 0 || sel == 1) break;
        for (size_t i = 0; i < msgs.size(); i++)
            ui_msgbox("HelloCpp", msgs[i].c_str());
    }
    ui_shutdown();
    return 0;
}
