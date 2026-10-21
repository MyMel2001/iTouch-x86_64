# NOTES-BY-AGENT.md — reverse-engineering + clean-room rewrite log

Target: `/var/home/rocketbob/Code/ios-fs/rootfs/Durango8K2.K93OS`
= iPhone OS 4.3.4 (build 8K2) **iPad** root filesystem (ProductVersion 4.3.4,
ProductBuildVersion 8K2, ProductName "iPhone OS", copyright 1983-2011 Apple).

Goal: totally original x86_64 OS ("iTouch x86_64" / internal name **K93OS-x86**)
built clean-room from observation of these binaries, running on Linux 7.2
(torvalds/linux v7.2 tree), installable to a hard drive, booting in QEMU.

---

## 1. Inventory findings (observation)

Root layout (BSD/Darwin heritage, symlink farm):

```
Applications/            28 .app bundles (SpringBoard apps)
bin/ sbin/ usr/          Darwin userland (bash, sh, ls, ... all Mach-O armv7)
System/Library/          Frameworks, PrivateFrameworks, Extensions, CoreServices,
                         LaunchDaemons, PreferenceBundles, SpringBoardPlugins,
                         Filesystems, Fonts, Caches, Lockdown, ...
Library/                 LaunchAgents/Daemons, Preferences, Ringtones, Dictionaries, ...
private/etc/             BSD etc: fstab, passwd, master.passwd, group, ttys,
                         services, protocols, networks, notify.conf, ppp/, racoon/
private/var/             mobile/, root/, db/, Keychains/, log/, vm/, folders/
etc -> private/etc, var -> private/var, tmp -> private/var/tmp
```

Key observations:

* Only 5 KEXTs ship in the *rootfs* (`AppleHDQGasGaugeControl`,
  `AppleMultitouchSPI`, `IOHIDFamily`, `IOUSBFamily`, `IOUSBDeviceFamily`) plus
  `IMGSGX543GLDriver.bundle` (PowerVR SGX543 GLES driver) and PPP plugins.
  Everything else (including the **SMC / AppleSMC** driver, iBoot, LLB, iBSS,
  iBEC, device-tree, kernelcache) lives in the **IPSW firmware image**, not in
  this rootfs dump. So: no iBoot binary present to RE here — documented as
  "absent from supplied artifact", not "does not exist".
* `System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv7` (147 MB) is the
  ARMv7 dyld shared cache — every system framework is prelinked into it.
* `/usr/sbin/fairplayd.K93` (6 MB) = FairPlay DRM daemon ("do not steal" text
  check is inside FairPlay codec/DRM paths — see RE notes below).
* `SystemVersion.plist` confirms 4.3.4 / 8K2.
* SpringBoard app list (Applications/): AdSheet, AppStore, Camera, Contacts~ipad,
  DataActivation, DemoApp, FieldTest, Game Center~ipad, iOS Diagnostics,
  iPodOut, Maps~ipad, MobileCal, MobileMail, MobileMusicPlayer, MobileNotes,
  MobilePhone, MobileSafari, MobileSlideShow, MobileStore, Photo Booth,
  Preferences, TrustMe, Utilities, Web, WebSheet, YouTube.
* CoreServices: SpringBoard.app, mDNSResponder.bundle, MobileStorageMounter.app,
  MobileCoreTypes.bundle, RawCamera.bundle, VoiceOverTouch.app, DumpPanic,
  ReportCrash, SystemVersion.plist.
* Launch model = `launchd` + `LaunchDaemons`/*.plist (sockets, WatchPaths,
  KeepAlive). All daemons are Mach-O armv7 → cannot be reused on x86_64, but the
  *semantics* (socket activation, KeepAlive, RunAtLoad, plist config) are
  reimplemented clean-room.

## 2. Legal / clean-room discipline (this project's rules)

* Apple binaries are treated as **read-only analysis artifacts**. No Apple code,
  no Apple resources (icons/artwork/sounds), no Apple text strings are copied
  into this repo.
* Every file under `userspace/` and `apps/` is written from observed behaviour +
  public documentation (POSIX, Mach-O/dyld format specs, Apple's own *open*
  XNU/darwin sources under `third_party/apple-oss` for API *semantics* only).
* Third-party components used are permissive/licensed OSS, added as git
  submodules under `third_party/` pinned to the exact tree the user named:
  * `third_party/apple-oss` -> apple-oss-distributions/distribution-iOS @ ios-433
    (reference for Darwin API semantics: XNU, libdispatch, dyld, Launchd, WebKit)
  * `third_party/linux`     -> torvalds/linux @ v7.2 (the kernel we boot)
* No jailbreak angle: root access, sideloading, filesystem access are *features*
  of this original OS, not circumvention of anything.

## 3. Engineering plan (how the rewrite is structured)

Because the supplied artifacts are 32-bit ARM Mach-O, a bit-for-bit port is
impossible on x86_64. The rewrite is a **behavioural clean-room reimplementation**
of the iOS 4.3.4 iPad *user experience* on Linux:

| iOS 4.3.4 part                  | K93OS-x86 clean-room part            |
|---------------------------------|--------------------------------------|
| XNU kernel + IOKit              | Linux 7.2 (torvalds/linux v7.2)      |
| launchd + LaunchDaemons plists  | `userspace/launchd.c` (PID 1, socket activation, KeepAlive, plist config) |
| SpringBoard + SpringBoardPlugins| `apps/springboard/` (framebuffer UI + serial TUI fallback, icon grid, app launcher) |
| UIKit-ish toolkit               | `userspace/libk93/` (window/list/text widgets, fb + serial backends, 8x16 font) |
| MobileSafari / WebSheet         | `apps/safari/` (clean-room DOM/CSS-subset/JS/image engine: libk93 `k93html.c` + QuickJS 2025-04-26 + stb_image v2.30, both fetched pinned by `scripts/build-deps.sh`; mbedTLS + proxy, modern CA store at /etc/ssl/certs) |
| Preferences.app                 | `apps/settings/` incl. **HTTP Proxy** pane (user requirement NOTE 3) |
| MobileStore / AppStore          | `apps/installer/` — **third-party .ipa sideloader** (App Store itself deliberately NOT ported) |
| MobileNotes, MobileCal, Contacts| `apps/notes`, `apps/calendar`, `apps/contacts` |
| MobileMusicPlayer, MobileSlideShow, Photo Booth | `apps/music`, `apps/photos` |
| Camera                          | `apps/camera` (V4L2 YUYV capture, zoom/AF/snapshot/AVI video+audio; synthetic test pattern when no `/dev/video*`) |
| FieldTest, iOS Diagnostics, TrustMe | `apps/diagnostics` (root shell / sysinfo) |
| hdiutil/InstallToHD             | `apps/installtohd` (format drive + install OS, 1 GiB swap, rest rootfs) |
| lockdown / MobileStorageMounter | `userspace/mounter.c` (mounts .dmg/.ipa, block devices) |
| SMC (firmware, not in rootfs)   | `userspace/smc.c` — clean-room "SMC" sysfs/ACPI abstraction; note: original SMC is a firmware coprocessor, impossible+pointless on PC hardware, replaced by ACPI |

Formats: `.ipa` = ZIP (inflate + XML plist parse + optional CMS/sig check) and
`.dmg` (UDZO/sparse bundle → raw image) both supported in-guest.

## 4. Progress log

* [x] Inventory of rootfs, SystemVersion, KEXT list, app list.
* [x] REA passes over key binaries — **ceiling hit**: REA's `inspect_macho`
  provider is `native-macos` ("Native macOS utilities require macOS");
  on this Linux host only `open_binary` works (identity: Mach-O arm,
  sha256 recorded for SpringBoard `3dde3c75...`). Ghidra headless is the
  upgrade path for decompilation-level findings.
* [x] Ghidra headless Mach-O inspection of SpringBoard (armv7): 310 own
  classes recovered from `__objc_classlist`; icon model, UIKit-owned
  statusbar, SBAway lock layer, SBWiFiManager/SBMediaController confirmed
  (see section 5). Method-level metadata + decompilation passes pending.
* [x] Repo skeleton, submodules, busybox static base.
* [x] Clean-room userspace: init/launchd, libk93, springboard.
* [x] Apps: all 20 ported (settings incl. HTTP proxy pane, files, terminal,
  installer, installtohd, notes, calendar, contacts, music, photos, camera,
  diagnostics, safari (mbedTLS + proxy), store stub, youtube, maps,
  gamecenter, trustme, adsheet).
* [x] ipa + dmg installers, root access, HTTP proxy settings, modern CA bundle.
* [x] QEMU image + serial autotest — **AUTOTEST PASS** (2026-10-16).
* [x] Graphics stack: DRM/KMS dumb-buffer backend in `libk93/k93ui.c`
  (`/dev/fb0` unavailable on this kernel — `CONFIG_FB_DEVICE=n`); init waits
  for `/dev/dri/card0` + loads the GPU driver; SpringBoard becomes DRM
  master, creates its **own** dumb buffer at the active CRTC's mode and
  hands the CRTC to it with an **atomic commit**, re-committed every flush.
  `DIRTYFB`/mmap alone never reach the host (they don't drive the plane's
  `atomic_update`, so `set_scanout`/`resource_flush` never fire and the host
  keeps scanning fbcon's buffer). A second bug scrambled the font: the
  generator regex also matched the per-glyph hex index inside
  `font_8x16.c` comments (`0x00`..`0xff`), splicing 256 index bytes into
  the 4096-byte bitmap — `tools/genfont.py` now strips comments first.
  `make qemu-test` phase 2/3 (QMP screendump pixel check via
  `tools/check-screendump.py`) — **GRAPHICS TEST PASS** (2026-10-17).
* [x] Guest networking: udhcpc + clean-room hook; virtio-net (QEMU) and
  real-hardware wired NIC drivers (`e1000`/`e1000e`/`r8169`/
  `8139too`/`8139cp`) in the initrd.
* [x] Real-hardware support wired up: ALSA Intel HDA sound (initrd codec
  chain + clean-room PCM client `libk93/k93snd.c` + Music.app playback),
  `i915` GPU driver chain, NIC drivers above. Phase-1 autotest VM now runs
  `-device e1000 -device intel-hda -device hda-duplex` — **AUTOTEST PASS**
  with `alsa-pcm-write ok` (2026-10-17).
* [x] Hardware drivers (TODO 1+2): AMD (`amdgpu`, `radeon`), NVIDIA
  (`nouveau`), `igb` ethernet, and Wi-Fi (`iwlwifi`, `rtl8xxxu`, `rtl8187`,
  `ath9k`, `ath5k`, `brcmfmac`, `mwifiex`) modules + firmware packaged;
  init now loads modules from a generated dependency-ordered
  `/lib/modules/loadlist` (replacing hand-coded arrays); firmware copied at
  build time from the host (`cp -aL` — symlink deref mattered: dangling
  iwlwifi/rtlwifi symlinks broke guest stat); DHCP lease wait fixes the
  autotest race. Autotest steps `hw-modules`/`hw-driver-set`/`hw-firmware`.
  Ceiling: no wpa_supplicant (open/manual Wi-Fi association); firmware not
  redistributed. Commit `2611f61`.
* [x] Display orientation + resolution (TODO 3+4): `/etc/k93/display.conf`
  (`MODE=auto|WxH`, `ORIENTATION=landscape|portrait`; `k93.mode=`,
  `k93.orientation=` cmdline overrides for tests), landscape default,
  static orientation (no auto-rotate). Portrait = swapped logical canvas +
  90°-CW rotate-blit into the landscape scanout (DIRTYFB clip fixed to
  physical dims); `ui_display_modes()` API; Settings > Display pane
  (resolution picker, orientation, restart SpringBoard). `make qemu-test`
  phase 3 boots `k93.orientation=portrait` and verifies the rotated canvas
  over the serial log + QMP screendump — **PORTRAIT TEST PASS**. Gotcha:
  bash `PPID` is readonly — orphaned QEMU held the serial pipe and hung
  the harness until timeout. Commit `796acc0`.
* [x] Package install from Files + Safari (TODO 5+6): `k93_app_install_deb`
  (ar-magic validation, busybox `dpkg-deb -x` to staging, merge into `/`;
  no maintainer scripts), Files.app install action dispatches on extension
  (.ipa/.deb), Safari downloads package links to `Media/Downloads` and
  offers install (binary-safe body length). Audit confirmed **no
  code-signing/signature checks exist anywhere in userspace** — item 6 is
  a design statement, documented in README + Settings Root Access pane.
  Fixture `test.deb` (ar + control/data tar.gz with parent dir entries —
  busybox needs them). Commit `f148a22`.
* [x] App SDK (TODO 7): `app-sdk/bin/k93sdk` (POSIX sh) new/build/bundle;
  `zig cc -target x86_64-linux-musl` preferred (cross from Linux/macOS/
  Haiku/WSL), host cc fallback; Go via `CGO_ENABLED=0 GOOS=linux
  GOARCH=amd64`; manifest generated from `app.conf`; bundler emits .ipa and
  .deb using zip/tar/ar (python3 fallback). Examples in C, C++ (`extern
  "C"`), Zig (`extern fn` bindings), Go (console backend). mkrootfs builds
  the C example; autotest `sdk-ipa-install`/`sdk-deb-install` sideload the
  real SDK product in the guest. Commit `684cb32`.
* [x] touchdova (TODO 8): `touchdova/bin/touchdova new` scaffolds an HTML
  app — `www/index.html` + generated native container (`main.c`, ~120
  lines: HTML-subset renderer, link menu, `k93://alert/`, `k93://quit`
  bridge, http(s) links open in Safari). `k93sdk bundle` carries `www/`
  into the bundle. Ceiling: no JS engine (documented upgrade path).
* [x] README.md updated (drivers, display config, SDK, touchdova,
  package install, ceilings).

## 5. Build/validation log (2026-10-16/17)

* `make all`: 25 binaries, musl-gcc -static, zero errors (warnings only).
* `make hostselfcheck`: 41 checks pass, incl. new GPT write/verify +
  protective-MBR check.
* `make qemu-test` (kernel 7.2.9-ogc3.1.fc44.x86_64, 1 core, 1.5G RAM,
  TCG or KVM, 16G virtio data disk): `K93-AUTOTEST-RESULT: PASS`.
  Steps covered: libk93 selfcheck, .ipa list/install/ARM-rejection,
  .dmg probe/extract/payload verify, SMC sysfs status, CA store,
  proxy config write/restore, app bundle presence + plist parse,
  GPT check, launchd job presence, shell.
* Interactive boot smoke test: SpringBoard renders on serial
  (`console=ttyS0`) with 20-app grid + statusbar (clock/net state).

Bugs found and fixed during validation (clean-room code, our own):
* `k93_gpt_verify` recomputed the header CRC over the header *including*
  the stored CRC field (UEFI spec excludes it) → every verify failed.
* `k93_app_install_ipa` searched for the `.app` bundle at the staging
  root instead of `Payload/` → all real .ipa installs failed.
* `selfcheck` synthetic-bundle test lacked `CFBundleExecutable`.
* initramfs needs a `/init`; kernel otherwise panics mounting real root.
* busybox: musl needs kernel UAPI headers (`CPATH=build/uapi`, mtd link);
  `yes | make defconfig` breaks under `pipefail` (SIGPIPE).
* UI console order now `/dev/console` → `ttyS0` → `tty1`, and apps spawned
  detached by launchd take input from the serial console (`infd`).

Known ceilings (documented, not bugs):
* Real-hardware graphics = Intel only (`i915` packaged; Intel GPU firmware
  blobs from `linux-firmware` not redistributed — add `/lib/firmware/i915`
  on the target if probe asks; `amdgpu`/`nouveau` not packaged). Sound =
  HDA only (no USB audio). Wired NICs only (no wireless drivers).
* DMG: UDZO/koly/mish only; sparse bundles + encrypted images rejected
  with clear errors. Binary plists rejected with clear errors.
* VM tests boot via QEMU `-kernel`/`-initrd`; `osdisk.img` is BIOS-bootable:
  MBR + GRUB core.img at sector 1 + ext4 partition at 1 MiB (rootfs, /boot
  with vmlinuz+initrd), installed rootless by `mkdisk.sh` (`boot.img` via dd,
  `sfdisk` table, `grub2-mkimage`, `mke2fs -E offset`; grub2-install needs
  loop+root, so it is never used). Covered by `make qemu-test` phase 4.
  UEFI/real-hardware firmware untested.
* REA Mach-O deep inspection needs macOS or Ghidra headless (see above).
* Graphics stack is DRM/KMS, not fbdev: this kernel is built with
  `CONFIG_FB_DEVICE=n` (`/dev/fb0` + `/sys/class/graphics/fb0` never exist —
  the fbdev core keeps framebuffers kernel-internal, fbcon-only). The UI
  therefore renders through `/dev/dri/card0` (clean-room KMS client in
  `libk93/k93ui.c`): dumb buffer + `mmap` + `SETCRTC`, damage notified via
  `DIRTYFB`. `bochs-drm` (QEMU stdvga) is the primary display;
  `virtio-gpu-pci` also works (needs `virtio_dma_buf.ko` + `virtio_gpu.ko`).
  Ceiling: single dumb buffer, no compositor, no GPU accel, no vsync/page
  flip. Text-console fallback when no KMS device probes. Validated by a
  QMP-screendump graphics test (SpringBoard rendered, `nonblack_px>500`).

### Networking + real-hardware support (2026-10-17)

* Networking: udhcpc hook `userspace/rootfs-files/udhcpc-default.script`
  (ifconfig + default route + resolv.conf, installed to
  `/usr/share/udhcpc/default.script`); init loads the NIC driver chain and
  DHCPs the first interface. QEMU user-mode netdev: gw `10.0.2.2`.
* initrd module packaging refactored: `install_module()` in
  `scripts/mkrootfs.sh` walks `modules.dep` and installs each module with
  its transitive deps to `/lib/modules/<underscore-name>.ko` (the exact
  paths init insmods). modules.dep keys are matched by basename with
  dash-insensitive compare — some files are `snd-hda-intel.ko` (dashes)
  while others are `virtio-gpu.ko`; both install as `<name>.ko`.
* Real-hardware drivers in the initrd: NICs `e1000`/`e1000e`/`r8169`/
  `8139too`/`8139cp` (+`mii`); GPU `i915` (+`wmi`/`video`/`i2c-algo-bit`/
  `cec`/`ttm`/`drm_buddy`/`drm_display_helper`, loaded as a dep group
  before the probe loop); sound `snd-hda-intel` + generic/Realtek
  (alc269/alc662/alc882)/HDMI codecs.
* Gotcha (sound): there is no modprobe/udev in the guest, and
  `insmod snd-hda-intel.ko` probes AND configures codecs synchronously —
  the codec drivers (`snd-hda-codec-generic`, realtek-lib, alc*, ...) must
  be insmod'd BEFORE the controller, or `azx_codec_configure()` fails
  every codec and the driver gives up with "Cannot probe codecs, giving
  up" (reproduced with QEMU `intel-hda` + `hda-duplex`).
* Gotcha: `alc269` uniquely depends on `snd-hda-scodec-component.ko`.
* `userspace/libk93/k93snd.c`: clean-room ALSA PCM playback client (S16_LE
  interleaved, kernel UAPI ioctls via vendored `linux/asound.h`; this
  kernel has no OSS emulation — `CONFIG_SND_OSSEMUL=n`, so `/dev/dsp`
  does not exist). Trap: in `hw_params`, an all-zero `snd_interval` means
  "must equal 0" — every unset parameter must be `[0, UINT_MAX]` or
  `SNDRV_PCM_IOCTL_HW_PARAMS` refines to `-EINVAL`.
* Wiring: Music.app plays PCM WAV through OSS `/dev/dsp` when present,
  else through `k93_snd_*` (negotiates the file's rate/channels, no
  resampling; mismatched rates refuse rather than play at wrong pitch).
* Validation: selfcheck gained "alsa pcm write" (open + 4096 silent
  frames); autotest gained `alsa-pcm-write` and reports the bound NIC
  driver (`eth0 present (driver: e1000)`). Phase-1 QEMU now runs
  `-netdev user -device e1000` + `-device intel-hda -device hda-duplex`
  (real-hardware drivers, same as on bare metal; virtio-net/gpu stay in
  phase 2 + `qemu-run.sh`). Result: **AUTOTEST PASS** (alsa pcm
  `controlC0 hwC0D0 pcmC0D0c pcmC0D0p timer`) + **GRAPHICS TEST PASS**
  (2026-10-17); host selfcheck 44 checks green (host ALSA negotiated
  44100 Hz/2ch — real hardware path works).

### Ghidra headless Mach-O inspection (2026-10-17)

Replaces the REA `inspect_macho` ceiling (macOS-only provider). Tooling:
Ghidra 12.1.4 `support/analyzeHeadless` (Java 27), project in
`/tmp/opencode/ghidra-k93`, analysis scripts `k93_dump.java`
(inventory) + `k93_objc_classes.java` (own-class recovery). Note: headless
has no PyGhidra ("Python is not available") — scripts must be Java.

Target: `SpringBoard.app/SpringBoard` (Mach-O armv7, v8 base, PIE).
Findings (behaviour study for clean-room fidelity only):
* 10679 functions, 516 referenced ObjC classes (imports), 310 OWN classes
  recovered by walking `__DATA,__objc_classlist` (armv7: `class_t.data`
  at +16, `class_ro_t.name` at +16).
* Icon system: `SBIconModel` + `SBIconController` + `SBIconScrollView`
  (paged grid) + `SBIconListView`/`SBDockIconListView` + `SBApplicationIcon`
  + `SBIconImageView` + `SBBadgeLabel`/`SBIconBadge` — confirms K93OS
  SpringBoard's modelled layout (icon grid + dock + badges).
* Statusbar is UIKit-owned in the original (`UIStatusBar`,
  `UIStatusBarWindow`, `UIStatusBarServer` are imports, no `SBStatusBar`
  class) — matches our choice to draw the bar in libk93 `ui_statusbar`.
* Lock/away layer: `SBAwayController`/`SBAwayModel`/`SBAwayView`/
  `SBAwayDateView` + `SBDeviceLockKeypad` (with `...Wildcat` iPad variant)
  + `SBSIMLockManager`. K93OS boots straight to SpringBoard — no lock
  screen by design (root tablet OS), documented divergence.
* Networking/media state lives inside SpringBoard: `SBWiFiManager`,
  `SBWiFiSignalStrength`, `SBMediaController`, `SBNowPlayingAlertItem`,
  `SBAwayMediaControlsView`; alerts are an `SBAlertItem` class family.
  K93OS models network state in the statusbar and audio via the ALSA path.
* Ceilings: ObjC METHOD names were not recovered (Ghidra's method-level
  objc metadata was not materialised — class-pointer level only); no
  decompilation pass over selected functions yet (rerun headless against
  the saved project to extend); `fairplayd.K93` was not found under
  `/usr/libexec` on this rootfs.

## 6. TUI -> GUI pass (2026-10-17)

Goal: make the text-menu apps look like a period GUI on the framebuffer
without touching the serial paths that autotest greps.

* `k93ui.c` grew a pixel layer: `ui_rgb`/`ui_tint` (shade derivation from
  one base color — code instead of per-color assets), `ui_fill`,
  `ui_fill_grad`, `ui_roundrect`, `ui_frame`, `ui_blit` (straight-alpha
  0xAARRGGBB), `ui_text{,_sh,_bold}` with scale, `ui_text_width`,
  `ui_px_width/height`. All no-op off the FB backend. `ui_clear` fills a
  dark page gradient and glyph cells inherit it, so unmodified text UIs
  sit on shaded chrome instead of black.
* Widget GUI skins live in the toolkit (one place): status/hint bars,
  beveled nav bar, grouped white panels, blue selection capsule +
  caret, alert panel with wrapped body and a blue OK, two-button
  confirm, input field, document page for `ui_textview`. Text backend
  branches keep the old cell rendering byte-for-byte.
* Icons: `tools/genicons.py` emits one rounded-square glossy SVG per app
  (original geometry; base hue per app, gradient stops/ink from
  `tint()`), rasterises via ImageMagick/librsvg to committed 80x80 PNGs
  in `assets/icons/`, and compiles `build/gen/icons.c` (linked into
  libk93.a, like `font8x16.o`). `k93_icon_by_name` resolves bare names,
  bundle idents (`com.k93.files` -> `files`) and substrings, falling
  back to the generic tile. No runtime decoder (SDK has no zlib use for
  it) — ceiling documented, upgrade path = decode PNGs at boot.
* SpringBoard home: wallpaper + status bar + icon grid + labels +
  selection capsule + launch splash; 2D arrow navigation (cols=1 in the
  text backend so UP/DOWN stay linear).
* Screendump check rewritten (`tools/check-screendump.py`): longest
  pure-white horizontal vs vertical run, >=200px in the expected
  orientation and <200 in the other. The signature is the status bar's
  full-width white gloss line (rotates with the canvas in portrait).
  fbcon garbage (~16px runs) fails it, as before.
* New `tools/qmp-input.py` (send-key drive-by) verified launched apps
  visually: Music menu skin and AdSheet alert panel captured via QMP
  screendump. `make qemu-test` all four phases green after the change;
  SDK (`k93sdk new` template) and `touchdova new` scaffolds build
  against the new toolkit; touchdova pages get the GUI document skin
  via `ui_textview` with no app-side change.

## 7. Real apps pass (2026-10-17)

Goal: turn the Maps/YouTube/Camera/Photos stubs into real apps while
keeping AdSheet provider-neutral.

* `k93_app_open_url()` (`libk93/k93app.c`): fork/execl `com.k93.safari`
  with the URL, waitpid, re-init. It does `ui_shutdown()` before the
  fork and `ui_init(NULL)` after — the display/input must be released
  by the caller or Safari's `drmSetMaster`/fb0 open fails and it renders
  blind (validated live: without the dance, Safari gets
  "setmaster: Resource busy" and never becomes visible).
  `ui_init(NULL)` now keeps the previous app name, so the caller's next
  draw shows its own statusbar title again.
* Safari handoff: `if (argc > 1) browse(url)` before the address loop.
  Redirects were dead (headers were stripped before the `Location:`
  branch could see them) — `http_get` now saves `Location:` into
  `http_location` and `browse_d(url, depth)` follows it with a depth-8
  guard. Verified live: `maps.google.com` -> 302 -> `www.google.com/maps`
  renders its no-JS text; `yewtu.be/trending` fetched and linked.
* Safari engine notes (new-engine pass over `apps/safari/{main,render,
  js,stbi}.c` + `libk93/k93html.c`): `<title>` is a rawtext element —
  `document.title=` must clear `node->text` as well as children or
  `text_content` keeps returning the parsed raw text.
  `url_absolutize` must keep non-default ports (`scheme://host[:port]`)
  or relative image/link URLs silently resolve to port 80. The
  `__k93_click` dispatch must free its JSValue or every click leaks a
  GC object and `JS_FreeRuntime` asserts (`list_empty(&rt->gc_obj_list)`)
  at exit. `<button>` renders as a selectable block registered like an
  anchor with an empty URL so keyboard Enter fires its onclick; line
  activation picks the first link run on a line.
* Maps menu: "Google Maps (web)", "Custom map URL…", "Offline tiles
  (local)" info. YouTube menu: Browse/Trending/Search/Change
  instance/About, instance from `/etc/k93/youtube.conf`
  (`INSTANCE=`, default `https://yewtu.be`) — all server-rendered HTML
  through Safari (which now also runs page JS via QuickJS; ceiling:
  no in-app video decode).
* Camera: V4L2 mmap streaming, YUYV-only (MJPEG rejected, documented),
  digital zoom 1/2/4/8 (crop + nearest), AF via `V4L2_CID_FOCUS_AUTO`,
  `C` cycles cameras, ENTER snapshots PPM to `/var/mobile/Media/Photos`,
  `R` records AVI (YUY2 video + ALSA PCM capture in a hand-written
  RIFF, in-memory idx1, fps patched on close); labeled synthetic
  640x480 test pattern when no `/dev/video*` so the test VM exercises
  the full UI/recording path. `k93_snd_cap_open/read/close` added to
  `k93snd.c` (raw UAPI on `/dev/snd/pcmC0D0c`); video-only fallback.
* Photos: half-block BMP renderer replaced by `show_generic()` —
  BMP 24/32 + PPM P6 decode to RGBA, aspect-fit `ui_blit` with
  letterbox; `describe()` fallback on the text console.
* Input note: `ui_shutdown()` now closes the evdev fd (it leaked across
  SpringBoard launches). SpringBoard's `esc` = quit is pre-existing —
  one key per step with screendumps between when driving via QMP.
* Mouse + super key (this round): libk93 `k93ui.c` opens all non-keyboard
  relative evdev nodes into `mfd[8]` and `mouse_pump()` sums deltas /
  ORs buttons+wheel across them, so PS/2 (2/3-button) and USB HID
  (3-button + wheel) coexist in one guest. Left click is synthesized to
  ENTER inside `ui_getkey` after the app consumed the click counter, so
  existing key-driven widgets keep working; right = ESC, middle = home,
  wheel = UP/DOWN or one list line per 250 ms in text lists. Cursor:
  12x16 arrow `cursor_stamp()`ed into the shadow copy on every
  `fb_render_all` (`cursor_scanout()`, rot90-aware) — live motion, not
  only on redraws. Position persists across app switches through a tiny
  `/run/k93.cursor` file (else the fresh process respawns the pointer at
  canvas center and it "teleports"). Super/Windows key is intercepted in
  the evdev loop: tap = home, hold >= 5 s = `UI_KEY_SWITCHER`;
  `K93_NO_SUPER_EXIT` (SpringBoard) turns both into synthetic keys, a
  frontmost app does real `exit(0)`/`exit(2)` and SpringBoard's
  `launch_sel` opens the RAM-only recents tray on status 2.
  QEMU v11.1.2 drive-by gotchas, all validated by `build/scratch/drive3.py`
  screenshots (m00..m15): do NOT pass `-mouse none` (v11 refuses/ignores
  when `usb-mouse` exists), qcodes for wheel are btn events
  (`scroll +1/-1`), super = `meta_l` (one word, not `meta-l`), and the
  Fedora host kernel `CONFIG_MOUSE_PS2_VMMOUSE=y` autograbs PS/2 — boot
  with `-machine pc,vmport=off` or QEMU's `current:true` pointer is the
  invisible vmmouse and every injected rel/btn event is dropped.
  `usb-mouse` stores buttons as LEVELS: press+release inside one
  `input-send-event` (or <1 HID frame apart) collapses and the click is
  lost — `tools/qmp-mouse.py click` now splits them 80 ms apart (PS/2
  would queue both packets; the wheel survives batching because QEMU
  converts wheel buttons to queued REL events immediately).
  QMP accepts only one live client per socket: close before spawning
  `qmp-screendump.py`, or its connect hangs forever.

---

## 8. Fonts / SVG / CSS+JS / SDK API pass (2026-10-18/19)

* TTF engine `libk93/k93font.c` + `include/k93font.h`, clean-room from the
  published TrueType/OpenType format docs: sfnt tables head/maxp/cmap
  (formats 4 + 12, scored subtable pick)/loca/glyf (simple + composite)/
  hhea/hmtx/kern (MS format 0), 4x4-supersampled nonzero-winding scanline
  rasterizer, bold variants, kerning, `k93_utf8_next`. UI integration:
  `ui_set_font` family picker, per-run font/underline in `ui_prun`
  (pageview + Safari line renderer), underline rule at baseline+1. Build
  copies known open-license fonts found on the host (Liberation/DejaVu/
  Droid/... via a `/usr/share/fonts` scan in `scripts/mkrootfs.sh`) so the
  guest defaults to a real system font; the 8x16 bitmap font remains the
  text-console fallback (`k93font_ready()==0`).
* Cursor: 9px-radius inverted disc (`CUR_R`) stamped into the shadow —
  samples the framebuffer under it and writes inverted pixels, so it is
  visible on any background.
* SVG `libk93/k93svg.c` from scratch: XML-ish tokenizer, rect/circle/
  ellipse/line/polyline/polygon/path (M L H V C S Q T A Z, rel variants,
  comma/space separators), nonzero + evenodd fills, stroke, opacity,
  solid fills -> RGBA raster -> own PNG encoder (stored-deflate, own
  crc32/adler32) -> vendored stb_image decode in Safari. Wired in as
  linked `<img>` (content sniffing) and inline `<svg>` blocks.
* CSS: specificity computed per selector (id*100 + class/attr*10 + tag),
  combinators `>` `+` `~`, attribute selectors `[a] [a=v] ^= $= *= ~=]`,
  margin/padding shorthands, text-decoration underline, font-family ->
  `k93font_id`; matching decls applied in stable specificity order.
* JS (QuickJS bindings): classList, dataset + style camelCase Proxies,
  DOMContentLoaded/load dispatch after scripts with readyState, rAF via
  16ms timers, outerHTML get/set, replaceChild/cloneNode/contains/
  hasChildNodes, document-level listener recording.
* `libk93/k93net.c` + `include/k93net.h`: Safari's network stack extracted
  for general use — url_parse/absolutize, /etc/k93/proxy.conf +
  `k93net_set_proxy` (config priority) + HTTP(S)_PROXY, mbedTLS w/ system
  CA (lazy init), chunked decode, redirect-following `k93net_get`; low-
  level open/read_http/close kept for custom flows. Safari main.c is now
  a thin shell around it.
* `libk93/k93img.c`: `ui_image_load/free` (file -> opaque 0xFFRRGGBB,
  alpha over white); `stbi.c` moved apps/safari -> libk93 so any app
  links the decoder. SDK: `k93sdk` LIBC_SRC += k93html/k93img/stbi with
  an `ensure_stb` step; app-sdk/README.md gained the complete API
  reference (widgets, pixel, images, events, pages, fonts, HTML/CSS,
  utils) + ceilings (no SDK HTTP: k93net needs mbedTLS).
* Validation round found + fixed real bugs:
  - `k93html_parse` text buffer `tbuf[4096]` was uninitialized; the text
    branch did `strlen(tbuf)` on stack garbage so text landed at a random
    offset (manifested as pointer-byte prefixes under -Os). Fixed with
    `tbuf[0] = 0`; root-caused via a glibc -Os repro + gdb (clean at -O0).
  - `render.c pool_put` still replaced UTF-8 multibyte with '?' (stale
    ASCII ceiling) — the TTF cmap itself is fine (host probe rasterizes
    é © · € — 中). Now memcpy verbatim.
  - `k93svg path_add` aborted path parsing at comma-separated numbers
    (the per-value loop didn't skip separators; `strtod` fails at ',').
    Fixed: skip space+comma before each value; selfcheck covers a cubic
    with commas.
  - Named entity table lacked the Latin-1 accents (eacute etc.): 21
    added.
  `make selfcheck` 0 failures; `make qemu-test` all phases PASS; QEMU
  drive6 screenshots (`build/scratch/tv6/`) show the full page: TTF
  unicode line "é · © € — done", centered red h1, underline, serif run,
  24px bold class run, linked SVG `<img>`, inline SVG incl. comma cubic,
  and the DOM line "cls=z ds=42 st=#00aa00 oh=true clone=true
  contains=true hcn=true repl=NEW dcl=complete raf=true".

## Safari no-stubs sweep + file:// (2026-10-20)

Goal: replace Safari placeholder paths with real implementations.

* New element renderers (`apps/safari/render.c`): `<img>` fetch-by-URL
  (`render_img_url`), `<iframe>`/`<frame>` (inline sub-document, depth-
  capped 2, srcdoc parses the attribute; src absolutizes + `sf_fetch`),
  `<object>`/`<embed>`/`<video>`/`<audio>`/`<source>` (media label with
  MIME + honest "playback unsupported (no codec)"; object falls back to
  its children on load failure), `<details>`/`<summary>` (closed hides
  body, ☐/▼ marker; summary is hit-testable -> `sf_details_toggle`),
  `<canvas>` placeholder box. Canvas 2D subset in `js.c`: fillRect/
  strokeRect/clearRect, beginPath/moveTo/lineTo/closePath/stroke/fill,
  fillStyle/strokeStyle/lineWidth, fillText (built-in font),
  getImageData/putImageData, toDataURL ceiling = NULL; getContext("webgl")
  returns null honestly. `main.c` summary-click toggles + re-render.
* `file://` support: `sf_fetch` + `url_ok` in main.c read the file via
  open/read (status 200); `k93net.c url_absolutize` joins relative paths
  onto the base dir for file: URLs. iframe src gate extended to file:
  (render.c one-liner). Fixtures installed to `/opt/k93/fixtures/` by
  mkrootfs.sh; `w3.html` extended with the new-elements section +
  `w3-frame.html`.
* Bugs found by driving the VM (QMP + screenshots):
  - Safari address dialog never exited on Esc: `ui_input` returns 1 on
    cancel but main.c checked `< 0`, so Esc re-browsed the prefilled URL.
    Fixed `!= 0` (main.c:351).
  - `render_iframe` rejected file: srcs after file:// was added to
    sf_fetch; gate now allows all three schemes.
* Guest validation (1280x800 screenshots): fixture loads via typed
  `file:///opt/k93/fixtures/w3.html` (typed via QMP; `:` = shift+semicolon
  combo — QEMU has no colon qcode); media labels render, missing img shows
  alt, details closed ☐ hides body, click toggles to ▼ + body visible,
  srcdoc iframe parses inline, object/embed fallback children render,
  canvas 2D draws fillRect/stroke/fillText ("K93 canvas2D"), script
  reports webgl=null. ser.log: 0 faults.
* Ceilings (documented, not bugs): canvas = 2D subset (no WebGL/paths
  clip/save/transform), iframe = inline sub-document with scripts inert,
  media = labels only (no codecs), `toDataURL` = null, file: iframe +
  main doc share the same 1280-char URL buffer.
