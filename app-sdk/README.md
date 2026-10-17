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
`ui_msgbox`, `ui_confirm`, `ui_input`, `ui_textview`, keyboard/input events.
Works on the framebuffer (DRM/KMS, 8x16 font) and the serial/VT text
console — same code, backend picked at runtime.

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
  include/              # (repo) userspace/include — k93ui.h, k93util.h
  examples/c-hello      # C + libk93 menu app
  examples/cpp-hello    # C++ same toolkit
  examples/go-hello     # Go console app
  examples/zig-hello    # Zig + extern C bindings
```

`k93sdk` compiles your `main.*` together with the toolkit sources
(`k93ui.c`, `k93util.c`, generated font) into one static binary — no shared
libraries to ship.

## Ceiling

- x86_64 only (K93OS runs on x86_64 hardware/VMs; ARM binaries are rejected).
- Go apps: console UI only (framebuffer toolkit is C; a cgo binding is the
  upgrade path).
- No Objective-C/Swift runtime; the OS does not ship one.
