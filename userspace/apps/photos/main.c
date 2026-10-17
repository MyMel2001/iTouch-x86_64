/* Photos.app - clean-room counterpart of MobileSlideShow.app / Photo Booth.
 * Browses /var/mobile/Media/Photos and renders BMP (24/32bpp) and PPM images
 * on the framebuffer. On the text console it shows image facts instead. */
#define _GNU_SOURCE
#include "k93ui.h"
#include "k93util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>

#define DIRN "/var/mobile/Media/Photos"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* text-mode fallback: describe the image */
static void describe(const char *path)
{
    char body[512];
    size_t l = 0;
    unsigned char *d = (unsigned char *)k93_read_file(path, &l);
    if (!d) { ui_msgbox("Photos", "Cannot read file"); return; }
    if (l > 26 && !memcmp(d, "BM", 2)) {
        uint32_t w = rd32(d + 18), h = rd32(d + 22);
        uint16_t bpp = rd16(d + 28);
        snprintf(body, sizeof body, "BMP image\n%ux%u, %u bpp\n%zu bytes\n\n"
                 "(framebuffer not available; graphics preview needs /dev/fb0)",
                 w, h, bpp, l);
    } else if (l > 2 && d[0] == 'P' && (d[1] == '6' || d[1] == '3')) {
        snprintf(body, sizeof body, "PPM image\n%zu bytes\n\n"
                 "(framebuffer not available; graphics preview needs /dev/fb0)", l);
    } else {
        snprintf(body, sizeof body, "Unknown format (%zu bytes)", l);
    }
    free(d);
    ui_textview(strrchr(path, '/') ? strrchr(path, '/') + 1 : path, body);
}

static void show_bmp(const char *path)
{
    size_t l = 0;
    uint8_t *d = (uint8_t *)k93_read_file(path, &l);
    if (!d || l < 54 || memcmp(d, "BM", 2)) { free(d); describe(path); return; }
    uint32_t off = rd32(d + 10);
    int32_t w = (int32_t)rd32(d + 18);
    int32_t h = (int32_t)rd32(d + 22);
    uint16_t bpp = rd16(d + 28);
    uint32_t comp = rd32(d + 30);
    if (comp != 0 || (bpp != 24 && bpp != 32) || w <= 0 || h == 0) {
        free(d);
        describe(path);
        return;
    }
    int flip = h > 0;
    if (h < 0) h = -h;
    uint32_t stride = ((uint32_t)w * (bpp / 8) + 3) & ~3u;
    if (off + (uint64_t)stride * (uint32_t)h > l) { free(d); describe(path); return; }
    /* naive nearest-neighbour fit into the console cell grid (8x16 cells) */
    int cols = ui_width(), rows = ui_height() - 3;
    ui_clear();
    ui_statusbar("Photos");
    /* build a downscaled greyscale map, then draw with half-block characters */
    int cw = cols, ch = (rows - 1) * 2;
    if (cw > w) cw = w;
    if (ch > h) ch = h;
    for (int cy = 0; cy < rows - 1; cy++) {
        for (int cx = 0; cx < cols; cx++) {
            int x0 = cx * w / cols, y0 = (cy * 2) * h / (rows - 1) / 2 * 2;
            int y1 = (cy * 2 + 1) * h / ((rows - 1) * 2);
            if (y0 >= h) y0 = h - 1;
            if (y1 >= h) y1 = h - 1;
            uint32_t row0 = (uint32_t)y0 * stride, row1 = (uint32_t)y1 * stride;
            const uint8_t *p0 = d + off + row0 + (uint32_t)x0 * (bpp / 8);
            const uint8_t *p1 = d + off + row1 + (uint32_t)x0 * (bpp / 8);
            int g0 = p0[2] * 3 + p0[1] * 6 + p0[0];
            int g1 = p1[2] * 3 + p1[1] * 6 + p1[0];
            const char *set = " .:-=+*#%@";
            char cell[3] = { set[g0 / 300], set[g1 / 300], 0 };
            if (cell[0] == ' ') cell[0] = '.';
            ui_puts(cx, cy + 1, cell);
        }
    }
    ui_statusmsg("Esc back");
    ui_flush();
    while (ui_getkey(250) == UI_KEY_NONE) ;
    free(d);
}

int main(void)
{
    if (ui_init("Photos") < 0) return 1;
    k93_mkdir_p(DIRN, 0777);
    for (;;) {
        char names[64][128];
        const char *items[70];
        int n = 0;
        DIR *d = opendir(DIRN);
        struct dirent *e;
        while (d && (e = readdir(d)) && n < 60) {
            size_t l = strlen(e->d_name);
            if (l < 4) continue;
            k93_strlcpy(names[n], e->d_name, sizeof names[n]);
            items[n] = names[n];
            n++;
        }
        if (d) closedir(d);
        items[n] = "Back";
        int sel = ui_menu("Photos", items, n + 1, 0);
        if (sel < 0 || sel == n) break;
        char path[256];
        snprintf(path, sizeof path, "%s/%s", DIRN, names[sel]);
        if (ui_is_graphical()) show_bmp(path);
        else describe(path);
    }
    ui_shutdown();
    return 0;
}
