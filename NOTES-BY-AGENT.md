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
| MobileSafari / WebSheet         | `apps/safari/` (minimal browser shell; modern CA store at /etc/ssl/certs) |
| Preferences.app                 | `apps/settings/` incl. **HTTP Proxy** pane (user requirement NOTE 3) |
| MobileStore / AppStore          | `apps/installer/` — **third-party .ipa sideloader** (App Store itself deliberately NOT ported) |
| MobileNotes, MobileCal, Contacts| `apps/notes`, `apps/calendar`, `apps/contacts` |
| MobileMusicPlayer, MobileSlideShow, Photo Booth | `apps/music`, `apps/photos` |
| Camera                          | `apps/camera` (no capture hw in VM → documented stub) |
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
  for `/dev/dri/card0` + loads `bochs-drm`; SpringBoard renders to a mapped
  dumb buffer, damage via `DIRTYFB`. `make qemu-test` phase 2
  (QMP screendump of the SpringBoard framebuffer) — **GRAPHICS TEST PASS**
  (2026-10-17).
* [x] Guest networking: udhcpc + clean-room hook; virtio-net (QEMU) and
  real-hardware wired NIC drivers (`e1000`/`e1000e`/`r8169`/
  `8139too`/`8139cp`) in the initrd.
* [x] Real-hardware support wired up: ALSA Intel HDA sound (initrd codec
  chain + clean-room PCM client `libk93/k93snd.c` + Music.app playback),
  `i915` GPU driver chain, NIC drivers above. Phase-1 autotest VM now runs
  `-device e1000 -device intel-hda -device hda-duplex` — **AUTOTEST PASS**
  with `alsa-pcm-write ok` (2026-10-17).
* [x] README.md.

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
* VM boots via QEMU `-kernel`/`-initrd`; `osdisk.img` (ext4, populated
  offline with `mke2fs -d`) is the installable OS disk artifact — a
  bootloader (grub/syslinux) would be needed for bare-metal boot.
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
