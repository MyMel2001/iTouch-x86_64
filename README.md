# K93OS-x86 ("iTouch x86_64")

A totally original, clean-room x86_64 tablet OS inspired by the *interface
model* of iPhone OS 4.3.4 (iPad, build 8K2), running on a Linux 7.2 kernel.
No Apple code, strings, or assets are used — the supplied rootfs was studied
as a read-only analysis artifact (inventory + behaviour notes in
`NOTES-BY-AGENT.md`), and everything here is written from scratch.

This is **not a jailbreak** and not a port of any Apple binary: root access,
filesystem browsing, and `.ipa` sideloading are *built-in* features of this
OS. Original ARM binaries are rejected by the installer with a clear
explanation (K93OS runs native x86_64 ELF only).

## What you get

* SpringBoard-style home screen (20 app bundles in `/Applications/*.app`,
  `Info.plist` + native ELF, same layout convention as observed in the rootfs)
* launchd-style service supervisor (`/etc/launchd/*.plist`: Label,
  ProgramArguments, RunAtLoad, KeepAlive) — modelled on the observed
  PID-1 `/sbin/launchd` role, implemented with original POSIX code
* Apps: Settings (incl. **HTTP Proxy** and **Display** panes), Files (root
  access, zip/dmg unpack, .ipa/.deb install), Terminal, Installer (.ipa
  sideloading), InstallToHD (GPT disk prep: 1 GiB swap + rest rootfs, with
  warnings + verification), Notes, Calendar, Contacts, Music, Photos, Camera,
  Diagnostics (k93smc hardware status), Safari (clean-room HTTP/HTTPS client
  on mbedTLS, honors the proxy + modern CA store, downloads+installs
  .ipa/.deb), Store/YouTube/Maps/GameCenter/TrustMe/AdSheet
* `.ipa` sideloading (ZIP + XML plist + ELF check), `.deb` installation
  (Debian ar + tar payload extracted into `/` via busybox `dpkg-deb`), and
  `.dmg` reading (UDZO/zlib, koly trailer, mish block table) — all
  clean-room parsers. Installable from Files.app or Safari; there is **no
  code-signing or app-signature check anywhere in this OS** — by design
  (open system; the Settings "Root Access" pane explains the model)
* Modern CA store: `/etc/ssl/certs/ca-certificates.crt` copied from the
  host's maintained bundle at build time (the 2011-era roots in the
  original rootfs are not used)
* Serial-console-first UI toolkit. Graphics stack is DRM/KMS (clean-room
  client in `libk93/k93ui.c`): init waits for `/dev/dri/card0`, loads the
  GPU driver, SpringBoard becomes DRM master, creates its own dumb buffer
  at the active CRTC's mode, maps it, and hands the CRTC to it with an
  atomic commit. Each flush re-commits the buffer (mmap + `DIRTYFB` alone
  never reach the host: they don't drive the plane's `atomic_update`, so the
  host scanout never switches). ANSI text console is the fallback when no
  KMS device appears. evdev + tty input, embedded GPL font
* Display configuration (`/etc/k93/display.conf`: `MODE=auto|WxH`,
  `ORIENTATION=landscape|portrait`), settable in **Settings > Display**
  (resolution picker from the connector's mode list, orientation, restart
  SpringBoard). Landscape is the default and orientation is **static** —
  no auto-rotate. Portrait builds a swapped logical canvas and
  rotation-blits it into the landscape scanout (DRM path)
* Guest networking: virtio-net (QEMU) plus real-hardware wired NIC drivers
  in the initrd (`e1000`, `e1000e`, `r8169`, `8139too`/`8139cp`, `igb`);
  init auto-loads them (ordered `/lib/modules/loadlist`) and DHCPs the
  first interface via busybox udhcpc, holding boot until the lease lands.
  In QEMU user-mode networking the host is reachable at `10.0.2.2`
  (Safari's proxy/TLS path is live-testable)
* Wi-Fi: `iwlwifi` (Intel), `rtl8xxxu`/`rtl8187` (RealTek), `ath9k`/`ath5k`
  (Atheros), `brcmfmac` (Broadcom), `mwifiex` (Marvell) modules + firmware
  are packaged; association is open-network/manual for now (no
  wpa_supplicant in the guest — see ceilings)
* Sound: Intel HDA through the kernel ALSA stack (`snd-hda-intel` +
  generic/Realtek/HDMI codec drivers; QEMU `intel-hda`/`hda-duplex`
  tested). `libk93/k93snd.c` is a clean-room PCM playback client speaking
  the kernel UAPI directly (no libasound); Music.app plays PCM WAV via
  OSS `/dev/dsp` when present, else via ALSA

## Layout

```
userspace/libk93/    clean-room core library (plist, zip, dmg, ui, app, gpt, util)
userspace/sbin/      init (PID 1) + launchd (service supervisor)
userspace/apps/      one dir per app bundle
userspace/tools/     k93selfcheck, k93dmgtool, k93ipatool, k93smc
userspace/tests/     selfcheck (host + guest runnable, no framework)
scripts/             build-deps, mkrootfs, mkinitrd, mkdisk, qemu-test, qemu-run
app-sdk/             app toolchain: k93sdk (new/build/bundle), examples (C/C++/Go/Zig)
touchdova/           HTML-app scaffolder (Cordova-style container + k93:// bridge)
third_party/         submodules (analysis/reference only, never linked):
                       apple-oss @ ios-433, linux @ v7.2
                     vendored build deps in _build/: zlib 1.3.1, mbedTLS 3.6.4, busybox
tools/, fixtures/    fixture generator + generated test .ipa/.dmg/.deb
```

Toolchain: `musl-gcc -static` for everything that runs in the guest; zlib and
mbedTLS are vendored and linked static. The guest kernel is the host kernel
package's `vmlinuz` (config: EXT4=y, VIRTIO_BLK=y, INITRD=y, SERIAL_8250=y),
so no kernel build is required.

## Build

```sh
sudo rpm-ostree install musl-gcc musl-static glibc-static cpio e2fsprogs \
    || brew install ...          # host deps (already satisfied on this host)
bash scripts/build-deps.sh zlib  # once
bash scripts/build-deps.sh mbedtls
bash scripts/build-deps.sh busybox
make all                        # 25 static binaries
make hostselfcheck              # fast validation loop on the host (41 checks)
make qemu-test                  # full QEMU autotest over serial
make run                        # interactive QEMU boot to SpringBoard
```

`make qemu-test` boots the initrd with kernel cmdline `autotest`, captures
the serial console to `build/serial.log`, and asserts
`K93-AUTOTEST-RESULT: PASS`. Test VM: 1 core, 1.5 GB RAM, 16 GB sparse
virtio data disk (`build/hdd16.img`), KVM when available, TCG otherwise.

Interactive run: serial console is your terminal (Up/Down/Enter/Esc,
printable keys). Quit QEMU with `Ctrl-A` `X`.

## Images

* `build/initrd.cpio.gz` — the full rootfs as initramfs (what QEMU boots)
* `build/osdisk.img` — ext4 "OS disk image", populated offline with
  `mke2fs -d` (no root needed); mountable loopback, installable to media
* `build/hdd16.img` — 16 GB sparse data disk; target for **InstallToHD**,
  which writes a fresh GPT (1 MiB aligned, Linux root + swap) and verifies
  it by re-reading

## Installing packages

Third-party `.ipa` or `.deb` from any source → `Files.app` (file action
menu), `Safari.app` (downloading a package link offers install), or:

```sh
k93ipatool list app.ipa     # inspect bundle metadata
k93ipatool inst app.ipa     # install into /Applications (needs root)
k93ipatool deb  pkg.deb     # extract a Debian package into /
k93ipatool apps             # list installed bundles
```

Requirements for an installable .ipa: ZIP with `Payload/<Name>.app/`,
XML `Info.plist` (binary plists are rejected with an error), and a native
x86_64 ELF executable. ARM Mach-O payloads are refused with an explanation.
`.deb` packages are extracted (payload only — no maintainer scripts run).

## App SDK + touchdova

```sh
cd app-sdk && bin/k93sdk new MyApp && cd MyApp
../app-sdk/bin/k93sdk build . && ../app-sdk/bin/k93sdk bundle .
# dist/MyApp.ipa + dist/MyApp.deb — install from Files or Safari
```

Works from Linux, macOS, Haiku, and Windows (WSL) with `zig` installed
(C/C++/Go/Zig; details in `app-sdk/README.md`). `touchdova/bin/touchdova
new MyApp` scaffolds an HTML-subset app with a `k93://` native bridge
(`touchdova/README.md`).

## Known ceilings

* Real-hardware graphics: Intel (`i915`), AMD (`amdgpu`, `radeon`) and
  NVIDIA (`nouveau`) kernel drivers are packaged and loaded by init, but
  GPU firmware blobs (`/lib/firmware/{i915,amdgpu,nouveau}` from the host's
  `linux-firmware`) are copied at build time and **not redistributed** —
  some generations want DMC/GuC (Intel) or SEC (AMD) firmware for full
  acceleration; copy them onto the target machine if the probe complains.
  Mesa-side NVK/Zinc are userspace — the kernel `nouveau`/`amdgpu` drivers
  here cover their GPU side, but no Mesa is shipped in the guest.
* Wi-Fi association is open-network/manual (the guest ships no
  `wpa_supplicant`); `ath10k` firmware is not shipped.
* Display: orientation/resolution are per-boot settings (conf file +
  Settings app), applied when apps restart; the legacy fbdev backend stays
  landscape-only, and rotation requires the 32bpp DRM path.
* Sound covers HDA only (Intel controller + common Realtek/HDMI codecs);
  no USB audio.
* DMG: UDZO/UDIF only (no sparse bundles, no encryption) — rejected
  clearly otherwise.
* VM boots via QEMU `-kernel`/`-initrd`; bare-metal boot would need a
  bootloader on `osdisk.img`.
* No code-signing exists at all (not as a check to bypass — as a design
  choice; see Settings > Root Access).
* touchdova: no JavaScript engine (static HTML + `k93://` bridge only).
* Camera/YouTube/Maps/AdSheet are honest local-content stubs (no capture
  hardware / no tracking clients by design).

## Legal

Clean-room implementation. `third_party/apple-oss` and `third_party/linux`
are reference submodules only; no Apple-originated code is compiled, linked,
or shipped. The embedded 8x16 console font is generated from the Linux
kernel's GPL-2.0 `font_8x16.c` via `tools/genfont.py` (GPL-2.0, attributed
in the generated file). See `NOTES-BY-AGENT.md` for the analysis trail.
