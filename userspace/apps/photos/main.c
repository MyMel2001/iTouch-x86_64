/* Photos.app - clean-room counterpart of MobileSlideShow.app / Photo Booth.
 * Browses /var/mobile/Media/Photos and renders BMP (24/32bpp) and PPM (P6)
 * images as pixels on the graphical backend (DRM dumb-buffer or fb0, whichever
 * ui_init picked), aspect-fit. On the text console it shows image facts. */
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
                 "(text console: no pixel preview here)",
                 w, h, bpp, l);
    } else if (l > 2 && d[0] == 'P' && (d[1] == '6' || d[1] == '3')) {
        snprintf(body, sizeof body, "PPM image\n%zu bytes\n\n"
                 "(text console: no pixel preview here)", l);
    } else {
        snprintf(body, sizeof body, "Unknown format (%zu bytes)", l);
    }
    free(d);
    ui_textview(strrchr(path, '/') ? strrchr(path, '/') + 1 : path, body);
}

/* ---- pixel preview: decode BMP(24/32) or PPM(P6) to RGBA, aspect-fit ---- */
/* returns 1 on success. Fits the decoded image into the pixel canvas,
 * preserving aspect (letterboxed), nearest-neighbour downscale. */
static int show_generic(const char *path)
{
    size_t l = 0;
    uint8_t *d = (uint8_t *)k93_read_file(path, &l);
    if (!d) return 0;

    /* decode -> (w,h,src RGB24 rows top-down) */
    int w = 0, h = 0;
    uint8_t *rgb = NULL;                 /* w*h*3 */
    if (l > 54 && !memcmp(d, "BM", 2)) {
        uint32_t off = rd32(d + 10);
        int32_t bw = (int32_t)rd32(d + 18);
        int32_t bh = (int32_t)rd32(d + 22);
        uint16_t bpp = rd16(d + 28);
        uint32_t comp = rd32(d + 30);
        if (comp == 0 && (bpp == 24 || bpp == 32) && bw > 0 && bh != 0) {
            int flip = bh > 0;
            if (bh < 0) bh = -bh;
            uint32_t stride = ((uint32_t)bw * (bpp / 8) + 3) & ~3u;
            if (off + (uint64_t)stride * (uint32_t)bh <= l) {
                w = bw; h = bh;
                rgb = malloc((size_t)w * h * 3);
                if (rgb) {
                    for (int y = 0; y < h; y++) {
                        int sy = flip ? (h - 1 - y) : y;
                        const uint8_t *src = d + off + (uint32_t)sy * stride;
                        uint8_t *dst = rgb + (size_t)y * w * 3;
                        for (int x = 0; x < w; x++) {
                            const uint8_t *p = src + (uint32_t)x * (bpp / 8);
                            dst[x * 3] = p[2];           /* R (BMP is BGRA) */
                            dst[x * 3 + 1] = p[1];
                            dst[x * 3 + 2] = p[0];
                        }
                    }
                }
            }
        }
    } else if (l > 2 && d[0] == 'P' && d[1] == '6') {
        /* header: "P6\n<w> <h>\n<max>\n" then binary RGB */
        const uint8_t *p = d + 2;
        const uint8_t *end = d + l;
        long vals[3];
        int got = 0;
        while (p < end && got < 3) {                 /* skip ws, read 3 ints */
            while (p < end && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) p++;
            if (p < end && *p == '#') { while (p < end && *p != '\n') p++; continue; }
            if (p >= end || *p < '0' || *p > '9') break;
            long v = 0;
            while (p < end && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
            vals[got++] = v;
        }
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) p++;
        if (got == 3 && vals[0] > 0 && vals[1] > 0 && vals[2] == 255) {
            w = (int)vals[0]; h = (int)vals[1];
            size_t need = (size_t)w * h * 3;
            if ((size_t)(end - p) >= need) {
                rgb = malloc(need);
                if (rgb) memcpy(rgb, p, need);
            } else { w = h = 0; }
        }
    }
    free(d);
    if (!rgb || w <= 0 || h <= 0) { free(rgb); return 0; }

    /* aspect-fit into the pixel canvas (leave the status + hint bars) */
    int CW = ui_px_width(), CH = ui_px_height() - 20 - 24;
    int dw = CW, dh = (int)((long)CW * h / w);
    if (dh > CH) { dh = CH; dw = (int)((long)CH * w / h); }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    uint32_t *fb = malloc((size_t)dw * dh * 4);
    if (!fb) { free(rgb); return 0; }
    for (int y = 0; y < dh; y++) {
        int sy = (int)((long)y * h / dh);
        const uint8_t *src = rgb + (size_t)sy * w * 3;
        uint32_t *dst = fb + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            int sx = (int)((long)x * w / dw);
            const uint8_t *q = src + (size_t)sx * 3;
            dst[x] = 0xFF000000u | ((uint32_t)q[0] << 16) |
                     ((uint32_t)q[1] << 8) | q[2];
        }
    }
    free(rgb);

    ui_clear();
    ui_statusbar("Photos");
    ui_fill(0, 20, CW, CH, ui_rgb(0x10, 0x14, 0x1C));
    ui_blit((CW - dw) / 2, 20 + (CH - dh) / 2, dw, dh, fb);
    char cap[160];
    snprintf(cap, sizeof cap, "%s  %dx%d", strrchr(path, '/') + 1, w, h);
    ui_statusmsg(cap);
    ui_flush();
    free(fb);
    while (ui_getkey(250) == UI_KEY_NONE) ;
    return 1;
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
        if (!show_generic(path)) describe(path);
    }
    ui_shutdown();
    return 0;
}
