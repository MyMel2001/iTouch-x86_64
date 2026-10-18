/* K93OS Maps - web maps via the system browser (clean-room).
 * Primary target is https://maps.google.com/ opened in Safari; any custom
 * map URL works the same way. Offline slippy-map tile directories
 * (/var/k93/maps, z/x/y.png) are still honored locally: the tile reader is
 * PNG-based and needs an image decoder this build does not ship - dropping
 * tiles there is documented ceiling, not a renderer. */
#define _GNU_SOURCE
#include <k93ui.h>
#include <k93util.h>
#include <k93app.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#define WEB_MAP "https://maps.google.com/"

static void offline_info(void)
{
    char msg[768];
    const char *tile_dir = "/var/k93/maps";
    DIR *d = opendir(tile_dir);
    int tiles = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)))
            if (e->d_name[0] != '.') tiles++;
        closedir(d);
    }
    if (tiles > 0)
        snprintf(msg, sizeof msg,
                 "Map tiles found: %d entries in %s\n\n"
                 "Tiles follow the standard slippy-map scheme (z/x/y.png).\n"
                 "This build ships no PNG decoder, so Maps hands map data\n"
                 "to Safari instead of rendering tiles itself.",
                 tiles, tile_dir);
    else
        snprintf(msg, sizeof msg,
                 "No map data in %s.\n\n"
                 "For local tiles, drop z/x/y.png images from any open map\n"
                 "tile source into that directory.", tile_dir);
    ui_msgbox("Offline tiles", msg);
}

int main(void)
{
    if (ui_init("Maps") < 0) return 1;
    const char *items[] = {
        "Google Maps (web)",
        "Custom map URL...",
        "Offline tiles (local)",
    };
    for (;;) {
        int sel = ui_menu("Maps", items, 3, 0);
        if (sel < 0) break;
        if (sel == 0) {
            ui_msgbox("Maps", "Opening maps.google.com in Safari...");
            k93_app_open_url(WEB_MAP);
        } else if (sel == 1) {
            char url[512] = "https://";
            if (ui_input("Map URL", url, sizeof url) == 0 && url[0])
                k93_app_open_url(url);
        } else {
            offline_info();
        }
    }
    ui_shutdown();
    return 0;
}
