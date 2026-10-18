#!/usr/bin/env python3
"""Validate a K93OS SpringBoard screendump (PPM P6) rendered the GUI home.

The GUI home draws a status bar with a full-width pure-white 2px gloss line
right under it. In landscape that is a horizontal white run near the top of the
frame; the portrait rot90 blit turns the same line into a vertical white run
near one side. That run is a robust signature of the drawn framebuffer (fbcon
glyph garbage or a blank/failed scanout will not produce one clean, long,
white-dominant run).

usage: check-screendump.py SCREEN.PPM {landscape|portrait}
exit 0 = expected orientation's GUI is present; 1 = not rendered / wrong.
"""
import sys

MIN_RUN = 200


def load(path):
    d = open(path, "rb").read().split(b"\n", 3)
    if d[0] != b"P6":
        sys.exit(f"not a P6 PPM: {path}")
    w, h = map(int, d[1].split())
    return d[3], w, h


def is_white(buf, w, x, y):
    i = (y * w + x) * 3
    return buf[i] == 255 and buf[i + 1] == 255 and buf[i + 2] == 255


def max_run(buf, w, h, horizontal):
    """Longest pure-white run scanning every row (horizontal) or column."""
    best = 0
    if horizontal:
        for y in range(h):
            run = 0
            base = y * w * 3
            for x in range(w):
                i = base + x * 3
                if buf[i] == 255 and buf[i + 1] == 255 and buf[i + 2] == 255:
                    run += 1
                    if run > best:
                        best = run
                else:
                    run = 0
    else:
        for x in range(w):
            run = 0
            for y in range(h):
                i = (y * w + x) * 3
                if buf[i] == 255 and buf[i + 1] == 255 and buf[i + 2] == 255:
                    run += 1
                    if run > best:
                        best = run
                else:
                    run = 0
    return best


def main():
    if len(sys.argv) != 3 or sys.argv[2] not in ("landscape", "portrait"):
        sys.exit(__doc__)
    path, orient = sys.argv[1], sys.argv[2]
    buf, w, h = load(path)
    horiz = max_run(buf, w, h, True)
    vert = max_run(buf, w, h, False)
    print(f"{path} {w}x{h} orient={orient} max_h_white_run={horiz} "
          f"max_v_white_run={vert} (need >= {MIN_RUN} in expected axis, "
          f"< {MIN_RUN} in other)")
    if orient == "landscape":
        ok = horiz >= MIN_RUN and vert < MIN_RUN
    else:
        ok = vert >= MIN_RUN and horiz < MIN_RUN
    if not ok:
        sys.exit(f"FAIL: expected orientation {orient} white run not found")
    print("PASS")


if __name__ == "__main__":
    main()
