# K93OS-x86 App SDK

Build native apps for K93OS-x86 (the clean-room x86_64 OS inspired by the
iPhone OS 4.3.4 interface model) from **Linux, macOS, Haiku, or Windows
(WSL)**, in **C, C++, Zig, or Go**.

No Xcode. No Apple SDK. No signing keys.

## Requirements

| Host       | Toolchain                                                        |
|------------|------------------------------------------------------------------|
| any        | [`zig`](https://ziglang.org) — `brew install zig` / pkg / ziglang.org (C/C++/Zig apps, cross-compiles from every host) |
| Linux x86_64 | host `cc` works too (fallback, no zig needed)                  |
| Go apps    | Go 1.21+ (`CGO_ENABLED=0 GOOS=linux GOARCH=amd64` — handled for you) |
| also       | `python3` (font generation), `make` (first run, kernel headers)  |

## Quick start

```sh
cd iTouch-x86_64/app-sdk
export K93_HOME=$PWD/..          # only needed outside the repo checkout

bin/k93sdk new MyApp             # scaffold ./MyApp/ (main.c + app.conf)
cd MyApp
k93sdk build .                   # -> ./MyApp (static x86_64 ELF)
k93sdk bundle .                  # -> dist/MyApp.ipa + dist/MyApp.deb
```

Copy `dist/MyApp.ipa` (or `.deb`) onto the device and install it with the
**Files** app, or open/download it in **Safari** — both paths install with
one confirmation. The app appears on the SpringBoard grid after relaunch.

## app.conf

```
NAME=MyApp                 # display name
IDENT=com.k93.myapp        # bundle identifier
VERSION=1.0
EXECUTABLE=MyApp           # binary name inside the bundle
```

`k93sdk bundle` generates `Info.plist` from these values (same keys the OS
registry reads: `CFBundleName`, `CFBundleDisplayName`,
`CFBundleIdentifier`, `CFBundleExecutable`, `CFBundleShortVersionString`).

## Languages

### C / Zig — full UI toolkit

`#include "k93ui.h"` gives you the K93OS UI: `ui_init`, `ui_menu`,
`ui_msgbox`, `ui_confirm`, `ui_input`, `ui_textview`, keyboard/mouse input
events. Works on the framebuffer (DRM/KMS) and the serial/VT text console —
same code, backend picked at runtime. Text renders with the system TTF
fonts (UTF-8) on the framebuffer and the embedded 8x16 bitmap font on the
text console. On the framebuffer the shared widgets render a GUI skin
(status/nav bars, grouped white panels, blue selection) automatically; the
text console keeps the plain list look.

For custom drawing on the framebuffer there is a pixel API (no-ops on the
text console, so one binary runs both ways):

```c
if (ui_is_graphical()) {
    ui_fill_grad(0, 0, ui_px_width(), ui_px_height(),
                 ui_rgb(0x1B, 0x2A, 0x4A), ui_rgb(0x0B, 0x11, 0x20));
    ui_roundrect(40, 40, 240, 60, 12, ui_rgb(0x5C, 0x9C, 0xF0),
                 ui_rgb(0x2F, 0x6F, 0xD0));            /* gradient capsule */
    ui_text_sh(60, 60, 1, "Hello", ui_rgb(0xFF, 0xFF, 0xFF), 0x1E4080);
    ui_flush();
}
```

`ui_tint(base, num, den)` derives shades from one base color — use it for
button/label variants instead of shipping per-color art. `ui_blit(x, y, w,
h, pixels)` draws 0xAARRGGBB pixel art (icons); `ui_image_load` decodes
PNG/JPG/GIF/BMP/TGA files to blit-ready pixels.

- C: `examples/c-hello`
- Zig: talk to libk93 through `extern fn` declarations, see `examples/zig-hello`

### C++ — same toolkit

Wrap the header in `extern "C"` (see `examples/cpp-hello`), link with
`k93sdk build` (it uses `zig c++` when zig is installed; otherwise host
`g++`/`clang++` on Linux x86_64).

### Go — terminal apps

Go apps build CGO-free (`GOOS=linux GOARCH=amd64`), so they use the
console/serial UI (`fmt`, `os.Stdin`) instead of the framebuffer toolkit.
They still bundle and sideload like any other app. See `examples/go-hello`.

## What a bundle is

- `.ipa` — zip with `Payload/<Name>.app/{Info.plist,<Executable>}`, the
  iOS layout, but the executable is a native **x86_64 ELF** (ARM Mach-O is
  rejected with a clear error).
- `.deb` — standard Debian binary package containing
  `/Applications/<Name>.app/`; installs via busybox `dpkg-deb`.

There is **no code signing and no notarization** on K93OS-x86 — root access
and third-party installs are features, not jailbreaks. Any x86_64 ELF with
a valid `Info.plist` runs.

## SDK layout

```
app-sdk/
  bin/k93sdk            # the whole toolchain driver (POSIX sh)
  include/              # (repo) userspace/include — k93ui.h, k93util.h,
                        # k93font.h, k93html.h, ...
  examples/c-hello      # C + libk93 menu app
  examples/cpp-hello    # C++ same toolkit
  examples/go-hello     # Go console app
  examples/zig-hello    # Zig + extern C bindings
```

`k93sdk` compiles your `main.*` together with the toolkit sources
(`k93ui.c`, `k93util.c`, `k93font.c`, `k93html.c`, `k93img.c` +
stb_image, generated font) into one static binary — no shared libraries
to ship.

## API reference

Everything below comes from the headers the SDK compiles against
(`userspace/include`); the toolkit sources are linked statically into your
binary.

### Widgets & app lifecycle — `k93ui.h`

| Function | Purpose |
|----------|---------|
| `int ui_init(const char *appname)` | pick backend (framebuffer or text console), 0 ok |
| `void ui_shutdown(void)` | release the display |
| `int ui_is_graphical(void)` | 1 = framebuffer backend active |
| `int ui_px_width() / ui_px_height()` | logical canvas size in pixels |
| `int ui_width() / ui_height()` | size in text cells/rows |
| `void ui_clear() / ui_flush()` | clear canvas / present it |
| `int ui_menu(title, items[], n, sel)` | modal list, returns index or -1 (Esc) |
| `void ui_msgbox(title, body)` | modal message, any key closes |
| `int ui_confirm(title, body)` | yes/no dialog, 1/0 |
| `int ui_input(prompt, buf, cap)` | one-line input, 0 ok / -1 cancel |
| `int ui_textview(title, text)` | scrollable text viewer |
| `int ui_editbox(title, buf, cap)` | multiline editor |
| `int ui_pageview(title, ui_page *, tick, arg)` | styled page view (see below) |
| `void ui_statusbar(title) / ui_statusmsg(msg)` | top/bottom chrome |
| `int ui_display_modes(modes[][32], max)` | connector modes "WxH" |

### Pixel & text drawing — `k93ui.h` (no-ops on the text console)

| Function | Purpose |
|----------|---------|
| `ui_rgb(r,g,b)` / `ui_tint(c,num,den)` | color helpers (tint: derive shades) |
| `ui_fill(x,y,w,h,rgb)` | solid rectangle |
| `ui_fill_grad(x,y,w,h,top,bot)` | vertical gradient |
| `ui_roundrect(x,y,w,h,r,top,bot)` | rounded gradient capsule |
| `ui_frame(x,y,w,h,r,thick,rgb)` | rounded outline |
| `ui_blit(x,y,w,h,px)` / `ui_blit_scaled(...)` | draw 0xAARRGGBB pixels, nearest-neighbour scale |
| `ui_text(x,y,scale,s,fg)` / `ui_text_sh` / `ui_text_bold` | text (UTF-8, TTF; `scale` doubles the 16px cell) |
| `ui_text_width(scale, s)` | advance width in px |
| `ui_set_font(family)` | pick TTF family ("sans", "serif", "mono", family name, or NULL = system default) |

### Image files — `k93ui.h`

```c
ui_image im;
if (ui_image_load(&im, "/Photos/pic.png") == 0) {   /* PNG/JPG/GIF/BMP/TGA */
    ui_blit(10, 40, im.w, im.h, im.px);             /* 0xFFRRGGBB pixels */
    ui_image_free(&im);
}
```

### Input & events — `k93ui.h`

| Function | Purpose |
|----------|---------|
| `int ui_getkey(timeout_ms)` | key or mouse event code, -1 on timeout |
| `int ui_mouse_x() / ui_mouse_y()` | cursor position in px |
| `int ui_mouse_visible(void)` | cursor shown? |
| `int ui_mouse_clicked(btn)` | consumes a pending press (0/1) |
| `int ui_mouse_scroll(void)` | consumes wheel delta (+down/-up) |

### Styled pages — `k93ui.h`

`ui_page` + `ui_pline` + `ui_prun` describe multi-run text pages (like
Safari renders): a page is lines, a line is runs (or one image), a run is
text with color/bold/link/font/underline. `ui_pageview` scrolls it with
link selection and mouse support. Build pages yourself for rich text
views; `ui_page_init`/`ui_page_free` manage the buffers.

### Fonts — `k93font.h`

Clean-room TrueType engine (from-scratch rasterizer; system OFL fonts are
installed to `/usr/share/fonts`). `k93font_init/shutdown/ready`,
`k93font_count/id/name/select/default_id/bold_id`, metrics
(`k93font_px/baseline/width`), direct glyph rasterization
(`k93font_glyph` → alpha coverage bitmap), `k93_utf8_next` decode helper.
Text without a TTF (text console) falls back to the embedded 8x16 bitmap.

### HTML/CSS — `k93html.h`

Clean-room HTML parser + CSS subset (the same engine Safari uses):
`k93html_parse/free`, tree building (`elem/textnode/append/
insert_before/unlink/clone/set_attr`), queries
(`get_id/query/query_all` — tag, `.class`, `#id`, descendant, `>`, `+`,
`~` and attribute selectors), serialization (`inner_html`,
`set_inner_html`, `text_content`), entities (`entity_decode`, UTF-8), and
the CSS API (`k93css_new/add/free/apply`, `k93css_color`) with
specificity ordering, inline runs, underline and font-family mapping.

### Utilities — `k93util.h`

`k93_read_file` / `k93_write_file`, `k93_mkdir_p`, `k93_copy_file` /
`k93_copy_tree` / `k93_rm_tree`, `k93_xmalloc`, `k93_strdup`,
`k93_strlcpy`, `k93_starts_with`, `k93_trim`, `k93_log`,
`k93_cmdline_arg` / `k93_cmdline_flag`, `k93_human_size`.

## Ceiling

- x86_64 only (K93OS runs on x86_64 hardware/VMs; ARM binaries are rejected).
- Go apps: console UI only (framebuffer toolkit is C; a cgo binding is the
  upgrade path).
- No Objective-C/Swift runtime; the OS does not ship one.
- No HTTP in SDK apps yet: the in-tree client (`k93net.h` — proxy support,
  mbedTLS HTTPS) needs mbedtls, which stays an OS-build dependency;
  vendoring mbedtls for the SDK is the upgrade path.
- No italic faces (rasterizer ceiling — upright fallback) and no CSS
  gradients/dasharray in the HTML engine (parse-and-ignore).
