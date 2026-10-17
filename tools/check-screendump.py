#!/usr/bin/env python3
"""Validate a K93OS SpringBoard screendump (PPM P6) actually rendered the UI.

SpringBoard's app list marks the selected entry with an inverted text row: a
solid white band with black letter cut-outs. In landscape that row is a
horizontal white band near the top; the portrait rot90 blit turns it into a
vertical white band near the right edge. That band is a robust signature of the
rendered framebuffer UI (fbcon glyph garbage or a blank/failed scanout will not
produce a clean, long, white-dominant run), unlike a raw non-black pixel count.

usage: check-screendump.py SCREEN.PPM {landscape|portrait}
exit 0 = expected orientation's UI is present; 1 = not rendered / wrong.
"""
import sys


def load(path):
    d = open(path, "rb").read().split(b"\n", 3)
    if d[0] != b"P6":
        sys.exit(f"not a P6 PPM: {path}")
    w, h = map(int, d[1].split())
    return d[3], w, h


def is_white(buf, w, x, y):
    i = (y * w + x) * 3
    return buf[i] == 255 and buf[i + 1] == 255 and buf[i + 2] == 255


def longest_run(buf, w, h, horizontal, y=None, x=None, step=2, minfrac=0.5):
    """Longest contiguous white run along a row (horizontal) or column."""
    best = cur = 0
    if horizontal:
        rng = range(0, w, step)
        for xx in rng:
            if is_white(buf, w, xx, y):
                cur += step
                best = max(best, cur)
            else:
                cur = 0
    else:
        rng = range(0, h, step)
        for yy in rng:
            if is_white(buf, w, x, yy):
                cur += step
                best = max(best, cur)
            else:
                cur = 0
    return best


def main():
    if len(sys.argv) != 3 or sys.argv[2] not in ("landscape", "portrait"):
        sys.exit("usage: check-screendump.py SCREEN.PPM {landscape|portrait}")
    want = sys.argv[2]
    buf, w, h = load(sys.argv[1])

    # Scan a few candidate rows near the top for a long horizontal white run
    # (the landscape selected-entry band), and a few candidate columns near the
    # right edge for a long vertical run (the portrait band after rot90).
    horiz = max(longest_run(buf, w, h, True, y=y) for y in (32, 40, 48, 56))
    vert = max(longest_run(buf, w, h, False, x=x) for x in (w - 48, w - 40, w - 32))

    ok_h = horiz >= 200
    ok_v = vert >= 200
    if want == "landscape" and ok_h:
        print(f"check-screendump: landscape UI present (h-band {horiz}px)")
        return 0
    if want == "portrait" and ok_v and not ok_h:
        print(f"check-screendump: portrait UI present (v-band {vert}px)")
        return 0
    print(f"check-screendump: FAIL want={want} horiz={horiz} vert={vert}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
