/* k93icons.h - generated app icon artwork (original, clean-room).
 * Glossy rounded-square icons in the iOS-4-era visual style, built from one
 * template + code-computed tints by tools/genicons.py (assets/icons SVG files,
 * then *.png, then build/gen/icons.c). Pixel format: 0xAARRGGBB rows, straight alpha.
 * Not built into app-sdk products (libk93.a only); SDK apps supply own art. */
#ifndef K93ICONS_H
#define K93ICONS_H
#include <stdint.h>

int            k93_icon_count(void);
const char    *k93_icon_name(int i);                 /* "safari", ... */
const uint32_t *k93_icon(int i, int *w, int *h);     /* NULL if i bad */
const uint32_t *k93_icon_by_name(const char *name, int *w, int *h);

#endif
