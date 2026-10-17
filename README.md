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
* Apps: Settings (incl. **HTTP Proxy** pane), Files (root access, zip/dmg
  unpack), Terminal, Installer (.ipa sideloading), InstallToHD (GPT disk
  prep: 1 GiB swap + rest rootfs, with warnings + verification), Notes,
  Calendar, Contacts, Music, Photos, Camera, Diagnostics (k93smc hardware
  status), Safari (clean-room HTTP/HTTPS client on mbedTLS, honors the
  proxy + modern CA store), Store/YouTube/Maps/GameCenter/TrustMe/AdSheet
* `.ipa` sideloading (ZIP + XML plist + ELF check) and `.dmg` reading
  (UDZO/zlib, koly trailer, mish block table) — all clean-room parsers
* Modern CA store: `/etc/ssl/certs/ca-certificates.crt` copied from the
  host's maintained bundle at build time (the 2011-era roots in the
  original rootfs are not used)
* Serial-console-first UI toolkit. Graphics stack is DRM/KMS (clean-room
  client in `libk93/k93ui.c`): init waits for `/dev/dri/card0`, loads
  `bochs-drm` (QEMU stdvga), `virtio-gpu`, or `i915` (real Intel GPUs),
  SpringBoard creates a dumb buffer, maps it, sets a mode and notifies
  damage via `DIRTYFB`. ANSI text console is the fallback when no KMS
  device appears. evdev + tty input, embedded GPL font
* Guest networking: virtio-net (QEMU) plus common real-hardware wired NIC
  drivers in the initrd (`e1000`, `e1000e`, `r8169`, `8139too`/`8139cp`);
  init auto-loads them and DHCPs the first interface via busybox udhcpc.
  In QEMU user-mode networking the host is reachable at `10.0.2.2`
  (Safari's proxy/TLS path is live-testable)
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
third_party/         submodules (analysis/reference only, never linked):
                       apple-oss @ ios-433, linux @ v7.2
                     vendored build deps in _build/: zlib 1.3.1, mbedTLS 3.6.4, busybox
tools/, fixtures/    fixture generator + generated test .ipa/.dmg
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

## Installing a .ipa

Third-party .ipa from any source → `Installer.app`, or:

```sh
k93ipatool list app.ipa     # inspect bundle metadata
k93ipatool inst app.ipa     # install into /Applications (needs root)
k93ipatool apps             # list installed bundles
```

Requirements for an installable .ipa: ZIP with `Payload/<Name>.app/`,
XML `Info.plist` (binary plists are rejected with an error), and a native
x86_64 ELF executable. ARM Mach-O payloads are refused with an explanation.

## Known ceilings

* Real-hardware graphics covers Intel (`i915` is packaged and loaded by
  init), but Intel GPU firmware blobs (`/lib/firmware/i915/*.bin` from the
  `linux-firmware` package) are not redistributed here — some generations
  want DMC/GuC firmware for full display; copy them onto the target machine
  if the probe complains. AMD (`amdgpu`) / NVIDIA (`nouveau`) drivers are
  not packaged (large dependency trees).
* Sound covers HDA only (Intel controller + common Realtek/HDMI codecs);
  no USB audio. Wireless NICs are not packaged (USB input works once the
  kernel's usbhid probes).
* DMG: UDZO/UDIF only (no sparse bundles, no encryption) — rejected
  clearly otherwise.
* VM boots via QEMU `-kernel`/`-initrd`; bare-metal boot would need a
  bootloader on `osdisk.img`.
* Camera/YouTube/Maps/AdSheet are honest local-content stubs (no capture
  hardware / no tracking clients by design).

## Legal

Clean-room implementation. `third_party/apple-oss` and `third_party/linux`
are reference submodules only; no Apple-originated code is compiled, linked,
or shipped. The embedded 8x16 console font is generated from the Linux
kernel's GPL-2.0 `font_8x16.c` via `tools/genfont.py` (GPL-2.0, attributed
in the generated file). See `NOTES-BY-AGENT.md` for the analysis trail.
