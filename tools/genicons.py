#!/usr/bin/env python3
"""Generate K93OS app icon artwork.

One rounded-square template in the glossy iOS-4-era style; per-app color is a
single base hue, every other shade (gradient stops, inner shading, glyph ink)
is computed from it by tint(). Glyphs are original geometry (plain shapes).
No Apple artwork is used or derived.

Pipeline (host-side only; runtime never decodes images):
  assets/icons/<name>.svg  --written here
  assets/icons/<name>.png  --80x80 via ImageMagick (librsvg), committed
  build/gen/icons.c        --0xAARRGGBB rows for ui_blit, linked into libk93.a

usage: genicons.py <assets_out_dir> <icons_c_out>
"""
import os
import struct
import subprocess
import sys

SIZE = 80
RX = 17  # corner radius (~22% of 80, like-era icon squares)


def hex2rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def write_png(path, w, h, rgba):
    """Minimal PNG writer (RGBA8, filter 0) so assets ship real .png files."""
    import zlib

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    raw = b"".join(b"\x00" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def rgb2hex(c):
    return "#%02x%02x%02x" % tuple(max(0, min(255, int(v))) for v in c)


def tint(hexc, f):
    """Scale a base hue; this is the one code path for all color variants."""
    return rgb2hex([v * f for v in hex2rgb(hexc)])


# ---------------------------------------------------------------- glyphs
# Each glyph receives base (hue) and ink (dark shade drawn on white shapes).
# White shapes sit on the colored tile; "ink" shapes sit on white shapes.
def g_envelope(base, ink):
    return (f'<rect x="19" y="28" width="42" height="27" rx="3" fill="#fff"/>'
            f'<path d="M21,31 L40,45 L59,31" stroke="{ink}" stroke-width="3.5" '
            f'fill="none" stroke-linejoin="round"/>')


def g_calendar(base, ink):
    dots = "".join(
        f'<circle cx="{26 + 14 * i}" cy="{42 + 8 * j}" r="2.4" fill="{ink}"/>'
        for j in range(2) for i in range(3))
    return (f'<rect x="24" y="17" width="4" height="9" rx="2" fill="#fff"/>'
            f'<rect x="52" y="17" width="4" height="9" rx="2" fill="#fff"/>'
            f'<rect x="17" y="21" width="46" height="42" rx="5" fill="#fff"/>'
            f'<path d="M17,26 a5,5 0 0 1 5,-5 h36 a5,5 0 0 1 5,5 v7 h-46 z" fill="{ink}"/>'
            + dots)


def g_camera(base, ink):
    return (f'<rect x="13" y="29" width="54" height="31" rx="6" fill="#fff"/>'
            f'<rect x="29" y="21" width="17" height="9" rx="3" fill="#fff"/>'
            f'<circle cx="40" cy="45" r="11" fill="{ink}"/>'
            f'<circle cx="40" cy="45" r="5.5" fill="#fff"/>'
            f'<circle cx="59" cy="35" r="2.4" fill="{ink}"/>')


def g_contacts(base, ink):
    return (f'<rect x="17" y="21" width="46" height="40" rx="6" fill="#fff"/>'
            f'<circle cx="40" cy="36" r="8" fill="{ink}"/>'
            f'<path d="M26,55 a14,12 0 0 1 28,0 z" fill="{ink}"/>')


def g_gauge(base, ink):
    return (f'<circle cx="40" cy="42" r="17" fill="none" stroke="#fff" stroke-width="4.5"/>'
            f'<path d="M40,42 L53,29" stroke="#fff" stroke-width="4.5" stroke-linecap="round"/>'
            f'<circle cx="40" cy="42" r="4" fill="#fff"/>'
            f'<rect x="38.5" y="20" width="3" height="6" fill="#fff"/>'
            f'<rect x="56" y="40.5" width="6" height="3" fill="#fff"/>'
            f'<rect x="18" y="40.5" width="6" height="3" fill="#fff"/>')


def g_folder(base, ink):
    return (f'<path d="M15,30 h17 l5,7 h28 a3,3 0 0 1 3,3 v18 a4,4 0 0 1 -4,4 h-45 '
            f'a4,4 0 0 1 -4,-4 v-24 a4,4 0 0 1 4,-4 z" fill="#fff"/>'
            f'<path d="M15,37 h50 v3 h-50 z" fill="{ink}" opacity="0.18"/>')


def g_dice(base, ink):
    pips = "".join(
        f'<circle cx="{x}" cy="{y}" r="3.4" fill="{ink}"/>'
        for x, y in ((28, 28), (52, 28), (40, 40), (28, 52), (52, 52)))
    return f'<rect x="18" y="18" width="44" height="44" rx="9" fill="#fff"/>' + pips


def g_drive(base, ink):
    return (f'<path d="M40,17 v13 M32,24 l8,9 l8,-9" stroke="#fff" stroke-width="5" '
            f'fill="none" stroke-linecap="round" stroke-linejoin="round"/>'
            f'<rect x="13" y="40" width="54" height="22" rx="6" fill="#fff"/>'
            f'<circle cx="40" cy="51" r="8.5" fill="none" stroke="{ink}" stroke-width="4"/>'
            f'<circle cx="40" cy="51" r="2.6" fill="{ink}"/>')


def g_package(base, ink):
    return (f'<rect x="17" y="31" width="46" height="31" rx="5" fill="#fff"/>'
            f'<rect x="17" y="31" width="46" height="10" rx="5" fill="{ink}"/>'
            f'<rect x="36" y="31" width="8" height="31" fill="{ink}"/>')


def g_map(base, ink):
    pin = (f'<path d="M40,45 l-6,-10 h12 z" fill="{ink}"/>'
           f'<circle cx="40" cy="32" r="6.5" fill="{ink}"/>')
    return (f'<rect x="15" y="21" width="50" height="38" rx="5" fill="#fff"/>'
            f'<path d="M31,21 v38 M49,21 v38" stroke="{ink}" stroke-width="2" opacity="0.55"/>'
            + pin)


def g_note(base, ink):
    lines = "".join(
        f'<rect x="27" y="{30 + 8 * i}" width="26" height="3" rx="1.5" fill="{ink}"/>'
        for i in range(4))
    return (f'<rect x="21" y="17" width="38" height="46" rx="5" fill="#fff"/>' + lines)


def g_photo(base, ink):
    return (f'<rect x="15" y="21" width="50" height="38" rx="5" fill="#fff"/>'
            f'<circle cx="49" cy="32" r="5" fill="{ink}"/>'
            f'<path d="M15,55 L31,39 L43,51 L53,43 L65,55 v2 a2,2 0 0 1 -2,2 h-46 '
            f'a2,2 0 0 1 -2,-2 z" fill="{ink}"/>')


def g_compass(base, ink):
    return (f'<circle cx="40" cy="40" r="23" fill="#fff"/>'
            f'<circle cx="40" cy="40" r="18" fill="none" stroke="{ink}" stroke-width="2"/>'
            f'<rect x="38" y="14" width="4" height="7" fill="{ink}"/>'
            f'<rect x="38" y="59" width="4" height="7" fill="{ink}"/>'
            f'<path d="M52,28 L44,44 L28,52 L36,36 Z" fill="{ink}"/>')


def g_gear(base, ink):
    teeth = "".join(
        f'<rect x="37" y="13" width="6" height="11" rx="2" fill="#fff" '
        f'transform="rotate({a} 40 40)"/>' for a in range(0, 360, 45))
    return (teeth +
            f'<circle cx="40" cy="40" r="15" fill="#fff"/>'
            f'<circle cx="40" cy="40" r="6.5" fill="{ink}"/>')


def g_grid(base, ink):
    return "".join(
        f'<rect x="{20 + 23 * i}" y="{20 + 23 * j}" width="17" height="17" rx="4" fill="#fff"/>'
        for j in range(2) for i in range(2))


def g_bag(base, ink):
    return (f'<path d="M31,31 v-6 a9,9 0 0 1 18,0 v6" fill="none" stroke="#fff" stroke-width="4.5"/>'
            f'<path d="M21,30 h38 l4,31 a3,3 0 0 1 -3,3 h-40 a3,3 0 0 1 -3,-3 z" fill="#fff"/>')


def g_prompt(base, ink):
    return (f'<rect x="17" y="25" width="46" height="32" rx="5" fill="#fff"/>'
            f'<path d="M23,33 l9,8 l-9,8" stroke="{ink}" stroke-width="4" fill="none" '
            f'stroke-linecap="round" stroke-linejoin="round"/>'
            f'<rect x="37" y="45" width="18" height="4.5" rx="2" fill="{ink}"/>')


def g_shield(base, ink):
    return (f'<path d="M40,17 l19,7 v13 c0,13 -8,21 -19,26 c-11,-5 -19,-13 -19,-26 v-13 z" '
            f'fill="#fff"/>'
            f'<path d="M31,40 l7,7 l13,-14" stroke="{ink}" stroke-width="4.5" fill="none" '
            f'stroke-linecap="round" stroke-linejoin="round"/>')


def g_play(base, ink):
    return (f'<rect x="14" y="25" width="52" height="32" rx="9" fill="#fff"/>'
            f'<path d="M34,33 L51,41 L34,49 z" fill="{ink}"/>')


def g_generic(base, ink):
    return (f'<rect x="22" y="22" width="36" height="36" rx="10" fill="none" '
            f'stroke="#fff" stroke-width="5.5"/>'
            f'<circle cx="40" cy="40" r="6" fill="#fff"/>')


def g_calc(base, ink):
    keys = "".join(
        f'<rect x="{23 + 10 * i}" y="{41 + 6 * j}" width="6" height="4" rx="1" fill="{ink}"/>'
        for j in range(2) for i in range(4))
    return (f'<rect x="22" y="16" width="36" height="48" rx="5" fill="#fff"/>'
            f'<rect x="26" y="21" width="28" height="10" rx="2" fill="{ink}"/>'
            + keys)


def g_book(base, ink):
    return (f'<rect x="18" y="20" width="44" height="40" rx="4" fill="#fff"/>'
            f'<path d="M40,20 v40" stroke="{ink}" stroke-width="2.5"/>'
            f'<rect x="23" y="27" width="12" height="2.5" rx="1.2" fill="{ink}"/>'
            f'<rect x="23" y="33" width="12" height="2.5" rx="1.2" fill="{ink}"/>'
            f'<rect x="45" y="27" width="12" height="2.5" rx="1.2" fill="{ink}"/>'
            f'<rect x="45" y="33" width="12" height="2.5" rx="1.2" fill="{ink}"/>')


def g_memo(base, ink):
    return (f'<rect x="34" y="18" width="12" height="26" rx="6" fill="#fff"/>'
            f'<path d="M34,32 h12 M34,38 h12" stroke="{ink}" stroke-width="2"/>'
            f'<path d="M29,30 a11,11 0 0 0 22,0" fill="none" stroke="#fff" stroke-width="4"/>'
            f'<path d="M40,44 v7 M32,51 h16" stroke="#fff" stroke-width="4" fill="none" '
            f'stroke-linecap="round"/>')


# name -> (base hue, glyph fn)
ICONS = {
    "adsheet":     ("#8899aa", g_envelope),
    "calculator":  ("#e8862b", g_calc),
    "calendar":    ("#d94f3d", g_calendar),
    "camera":      ("#556270", g_camera),
    "contacts":    ("#3f8fd8", g_contacts),
    "diagnostics": ("#707880", g_gauge),
    "files":       ("#d8a520", g_folder),
    "gamecenter":  ("#9b59b6", g_dice),
    "ibooks":      ("#a9744a", g_book),
    "installtohd": ("#4a90d9", g_drive),
    "installer":   ("#2f9e55", g_package),
    "maps":        ("#3aa66f", g_map),
    "music":       ("#f0721e", g_note),
    "notes":       ("#e8b800", g_note),
    "photos":      ("#1f9e8e", g_photo),
    "safari":      ("#2f7fd0", g_compass),
    "settings":    ("#808a96", g_gear),
    "springboard": ("#3b5aa0", g_grid),
    "store":       ("#c0508f", g_bag),
    "terminal":    ("#37424f", g_prompt),
    "trustme":     ("#34495e", g_shield),
    "voicememos":  ("#2f7fd0", g_memo),
    "youtube":     ("#d33b3b", g_play),
    "generic":     ("#5b6470", g_generic),
}
# notes tile: white lined page reads better than a dark glyph on yellow


def svg_for(base, glyph):
    light, dark = tint(base, 1.22), tint(base, 0.55)
    ink = tint(base, 0.62)
    g = glyph(base, ink)
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{SIZE}" height="{SIZE}" viewBox="0 0 {SIZE} {SIZE}">
<defs>
<linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">
<stop offset="0" stop-color="{light}"/><stop offset="1" stop-color="{dark}"/>
</linearGradient>
<linearGradient id="gloss" x1="0" y1="0" x2="0" y2="1">
<stop offset="0" stop-color="#ffffff" stop-opacity="0.5"/>
<stop offset="1" stop-color="#ffffff" stop-opacity="0.04"/>
</linearGradient>
</defs>
<rect x="1" y="1" width="78" height="78" rx="{RX}" ry="{RX}" fill="url(#bg)"/>
<path d="M1,18 A{RX},{RX} 0 0 1 18,1 H62 A{RX},{RX} 0 0 1 79,18 V38 C58,51 22,51 1,38 Z" fill="url(#gloss)"/>
{g}
<rect x="1.5" y="1.5" width="77" height="77" rx="{RX - 0.5}" ry="{RX - 0.5}" fill="none" stroke="#ffffff" stroke-opacity="0.35"/>
<rect x="1.5" y="1.5" width="77" height="77" rx="{RX - 0.5}" ry="{RX - 0.5}" fill="none" stroke="#000000" stroke-opacity="0.15" transform="translate(0,1)"/>
</svg>
'''


C_FMT = """/* GENERATED by tools/genicons.py - do not edit.
 * Original artwork: one rounded-square glossy template + tint() shades.
 */
#include "k93icons.h"
#include <string.h>

%s
static const char *const names[] = { %s };
static const uint32_t *const imgs[] = { %s };

int k93_icon_count(void) { return %d; }
const char *k93_icon_name(int i) { return (unsigned)i < (unsigned)%d ? names[i] : NULL; }
const uint32_t *k93_icon(int i, int *w, int *h)
{
    if ((unsigned)i >= (unsigned)%d) return NULL;
    if (w) *w = %d;
    if (h) *h = %d;
    return imgs[i];
}
/* Accepts a bare icon name ("settings"), a bundle identifier
 * ("com.k93.settings"), or any string that merely contains the icon name as a
 * word ("Safari.app"). The last-dot-component of a dotted ident is matched
 * exactly first, so com.k93.files resolves to "files" and not a substring of
 * some other ident. */
const uint32_t *k93_icon_by_name(const char *name, int *w, int *h)
{
    if (!name) return NULL;
    const char *dot = strrchr(name, '.');
    const char *leaf = dot && dot[1] ? dot + 1 : name;
    for (int i = 0; i < %d; i++)
        if (!strcmp(names[i], leaf)) return k93_icon(i, w, h);
    for (int i = 0; i < %d; i++)
        if (strstr(name, names[i])) return k93_icon(i, w, h);
    return k93_icon(%d, w, h);              /* generic fallback */
}
"""


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    assets, out_c = sys.argv[1], sys.argv[2]
    os.makedirs(assets, exist_ok=True)
    os.makedirs(os.path.dirname(out_c) or ".", exist_ok=True)
    pngs = []
    for name, (base, glyph) in sorted(ICONS.items()):
        svg = os.path.join(assets, name + ".svg")
        png = os.path.join(assets, name + ".png")
        with open(svg, "w") as f:
            f.write(svg_for(base, glyph))
        r = subprocess.run(
            ["convert", svg, "-background", "none", "-resize", f"{SIZE}x{SIZE}",
             "-depth", "8", "rgba:-"], capture_output=True)
        want = SIZE * SIZE * 4
        if r.returncode != 0 or len(r.stdout) != want:
            sys.exit(f"{name}: convert failed rc={r.returncode} "
                     f"got {len(r.stdout)} want {want}\n{r.stderr.decode()}")
        with open(png, "wb") as f:
            write_png(png, SIZE, SIZE, r.stdout)
        pngs.append((name, r.stdout))
    arrays, refs = [], []
    for name, raw in pngs:
        # PNG rows are RGBA8888; pack to 0xAARRGGBB (matches shadow-buffer
        # little-endian byte order: B,G,R,A).
        px = [f"0x{raw[i+3]:02x}{raw[i]:02x}{raw[i+1]:02x}{raw[i+2]:02x}"
              for i in range(0, len(raw), 4)]
        arr = (f"static const uint32_t img_{name}[{SIZE * SIZE}] = {{\n" +
               ",".join(px) + "\n};\n")
        arrays.append(arr)
        refs.append(f"img_{name}")
    names = ", ".join(f'"{n}"' for n, _ in pngs) + ","
    refstr = ", ".join(refs) + ","
    with open(out_c, "w") as f:
        f.write(C_FMT % ("\n".join(arrays), names, refstr,
                         len(pngs), len(pngs), len(pngs), SIZE, SIZE,
                         len(pngs), len(pngs), len(pngs) - 1))
    print(f"genicons: {len(pngs)} icons -> {out_c}")


if __name__ == "__main__":
    main()
