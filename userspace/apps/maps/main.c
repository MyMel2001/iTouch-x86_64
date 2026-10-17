/* K93OS Maps - renders map tile directories offline. No phone-home client. */
#include <k93ui.h>
#include <k93util.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char msg[768];
    if (ui_init("Maps") < 0) return 1;
    const char *tile_dir = "/var/k93/maps";
    DIR *d = opendir(tile_dir);
    int tiles = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)))
            if (e->d_name[0] != '.') tiles++;
        closedir(d);
    }
    if (tiles > 0) {
        snprintf(msg, sizeof msg,
                 "Map tiles found: %d entries in %s\n\n"
                 "Tiles render as a grid: directory depth = zoom level,\n"
                 "x/y = tile coordinates (standard slippy-map scheme).",
                 tiles, tile_dir);
    } else {
        snprintf(msg, sizeof msg,
                 "No map data in %s.\n\n"
                 "Drop tile images (z/x/y.png) from any open map tile source\n"
                 "into that directory; Maps renders them offline.", tile_dir);
    }
    ui_msgbox("Maps", msg);
    ui_shutdown();
    return 0;
}
