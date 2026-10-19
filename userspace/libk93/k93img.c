/* k93img.c - image file loading for apps: stb_image behind a small libk93
 * API. Decodes PNG/JPG/GIF/BMP/TGA to opaque 0xFFRRGGBB pixels (alpha
 * composited over white), ready for ui_blit. Safari decodes from memory
 * with its own cache and does not use this. */
#include "k93ui.h"
#include "stb_image.h"
#include <stdlib.h>

int ui_image_load(ui_image *im, const char *path)
{
    if (!im || !path) return -1;
    im->px = NULL;
    im->w = im->h = 0;
    int w, h, c;
    unsigned char *s = stbi_load(path, &w, &h, &c, 4);
    if (!s || w <= 0 || h <= 0) { stbi_image_free(s); return -1; }
    uint32_t *px = malloc((size_t)w * (size_t)h * 4);
    if (!px) { stbi_image_free(s); return -1; }
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        const unsigned char *p = s + i * 4;
        uint32_t a = p[3];
        uint32_t r = (p[0] * a + 255 * (255 - a)) / 255;
        uint32_t g = (p[1] * a + 255 * (255 - a)) / 255;
        uint32_t b = (p[2] * a + 255 * (255 - a)) / 255;
        px[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    stbi_image_free(s);
    im->px = px;
    im->w = w;
    im->h = h;
    return 0;
}

void ui_image_free(ui_image *im)
{
    if (!im) return;
    free(im->px);
    im->px = NULL;
    im->w = im->h = 0;
}
