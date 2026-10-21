#!/usr/bin/env bash
# Assemble the K93OS-x86 root filesystem from build outputs.
# Result: build/rootfs (readable/writable tree used by mkinitrd/mkdisk).
set -euo pipefail
TOP="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$TOP/build"
GBIN="$BUILD/bin"
ROOT="$BUILD/rootfs"
BB="$TOP/third_party/_build/busybox/busybox"

test -x "$BB" || bash "$TOP/scripts/build-deps.sh" busybox
test -x "$GBIN/init" || { echo "run 'make all' first" >&2; exit 1; }

rm -rf "$ROOT"
mkdir -p "$ROOT/bin" "$ROOT/sbin" "$ROOT/usr/bin" "$ROOT/etc/launchd" \
    "$ROOT/etc/ssl/certs" "$ROOT/etc/k93" "$ROOT/system/bin" \
    "$ROOT/opt/k93/fixtures" "$ROOT/Applications" \
    "$ROOT/proc" "$ROOT/sys" "$ROOT/dev" "$ROOT/tmp" "$ROOT/run" "$ROOT/mnt" "$ROOT/root" "$ROOT/home"
mkdir -p "$ROOT/var/db/k93" "$ROOT/var/log" "$ROOT/var/run"
mkdir -p "$ROOT/var/k93/videos" "$ROOT/var/k93/maps" "$ROOT/var/k93/ads" \
    "$ROOT/var/k93/gamecenter" "$ROOT/var/k93/profiles"
mkdir -p "$ROOT/var/mobile/Media/Downloads" "$ROOT/var/mobile/Library/Notes" \
    "$ROOT/var/mobile/Library/Calendar" "$ROOT/var/mobile/Library/Contacts" \
    "$ROOT/var/mobile/Media/Photos" "$ROOT/var/mobile/Media/Music"

# ------------------------------------------------------------- base userland
install -m 0755 "$BB" "$ROOT/bin/busybox"
"$ROOT/bin/busybox" --install -s "$ROOT/bin"        # applet symlinks (all in /bin)
# busybox bakes absolute host paths into the links; make them relative
find "$ROOT/bin" -maxdepth 1 -type l | while read -r l; do ln -sfn busybox "$l"; done
install -m 0755 "$GBIN/init"    "$ROOT/sbin/init"
install -m 0755 "$GBIN/launchd" "$ROOT/sbin/launchd"
ln -sfn sbin/init "$ROOT/init"      # kernel looks for /init inside an initramfs

# Kernel device drivers are modules; the initramfs must carry them plus their
# transitive dependencies. install_module() walks modules.dep so nothing has to
# be hand-listed twice (missing deps previously broke virtio_gpu silently).
# Output is always /lib/modules/<underscore-name>.ko - the exact paths init
# insmods. modules.dep keys use dashes for some files (snd-hda-intel.ko), so
# matching is dash-insensitive on the basename. Each installed module is also
# appended to /lib/modules/loadlist in dependency order (deps before
# dependents), so init does a single ordered insmod pass instead of
# hard-coding per-subsystem lists.
mkdir -p "$ROOT/lib/modules"
DEPFILE="$(echo /usr/lib/modules/*/modules.dep | awk '{print $1}')"
: > "$ROOT/.modstamps"
: > "$ROOT/.mods.loaded"

install_module() {           # install_module <module name, dashes or underscores>
    local want dep key src out
    want="$(echo "$1" | tr - _)"
    [ -n "$DEPFILE" ] || return 0
    grep -qx "$want" "$ROOT/.modstamps" 2>/dev/null && return 0
    echo "$want" >> "$ROOT/.modstamps"
    key="$(awk -F: -v w="$want" \
        '{n=$1; sub(/.*\//,"",n); sub(/\.ko$/,"",n); gsub(/-/,"_",n);
          if (n==w) {print $1; exit}}' "$DEPFILE")"
    if [ -z "$key" ]; then
        echo "  (module $1 not in kernel; skipped)" >&2
        return 0
    fi
    for dep in $(awk -F: -v k="$key" '$1==k {print $2}' "$DEPFILE"); do
        install_module "$(basename "$dep" .ko)"
    done
    src="$(ls /usr/lib/modules/*/"$key" 2>/dev/null | head -1)"
    [ -n "$src" ] || { echo "  ($1 missing on disk; skipped)" >&2; return 0; }
    out="$ROOT/lib/modules/$want.ko"
    case "$src" in
        *.xz)  xz -dc "$src" > "$out" ;;
        *.zst) zstd -dc "$src" > "$out" ;;
        *)     cp "$src" "$out" ;;
    esac
    chmod 0644 "$out"
    echo "$want" >> "$ROOT/.mods.loaded"
}

# graphics: virtio-gpu + bochs (test VMs), i915 (Intel), amdgpu + radeon (AMD),
# nouveau (NVIDIA - kernel side of NVK; NVK/Zinc themselves are Mesa userspace)
for M in virtio-gpu bochs i915 amdgpu radeon nouveau; do install_module "$M"; done
# wired NICs: virtio-net (test VMs), Intel e1000/e1000e/igb, RealTek r8169 + 8139
for M in virtio_net e1000 e1000e igb r8169 8139too 8139cp; do install_module "$M"; done
# Wi-Fi: Intel iwlwifi, RealTek rtl8xxxu (USB) + rtl8187, Atheros ath9k/ath5k,
# Broadcom brcmfmac, Marvell mwifiex (all kernel-native drivers)
for M in iwlwifi rtl8xxxu rtl8187 ath9k ath5k brcmfmac mwifiex; do install_module "$M"; done
# sound: Intel HDA codecs first, controller LAST - the controller probes and
# configures codecs synchronously during insmod, so every codec driver must
# already be registered (the guest has no modprobe to resolve aliases).
for M in snd-hda-codec-generic snd-hda-codec-hdmi snd-hda-codec-realtek-lib \
         snd-hda-scodec-component snd-hda-codec-alc662 snd-hda-codec-alc269 \
         snd-hda-codec-alc882 snd-hda-intel; do install_module "$M"; done
mv "$ROOT/.mods.loaded" "$ROOT/lib/modules/loadlist"
rm -f "$ROOT/.modstamps"

# ---------------------------------------------------------- device firmware
# Real-hardware drivers (iwlwifi, rtl8xxxu, amdgpu, brcmfmac, mwifiex, ...)
# need firmware blobs. Copied from the host's linux-firmware tree at build
# time (never stored in git), the same policy as the CA bundle. The kernel
# firmware loader transparently decompresses .xz/.zst files (FW_LOADER_COMPRESS).
FW=/usr/lib/firmware
mkdir -p "$ROOT/lib/firmware"
# -L dereferences: iwlwifi-*/brcm/mrvl entries are symlinks into intel/,
# cypress/, libertas/ trees that are not copied wholesale.
for pat in "$FW"/iwlwifi-* "$FW"/regulatory.db "$FW"/regulatory.db.p7s; do
    [ -e "$pat" ] && cp -aL "$pat" "$ROOT/lib/firmware/"
done
for d in rtlwifi amdgpu radeon brcm mrvl; do
    [ -d "$FW/$d" ] && cp -aL "$FW/$d" "$ROOT/lib/firmware/"
done
echo "firmware: $(du -sh "$ROOT/lib/firmware" 2>/dev/null | cut -f1)"
for t in k93selfcheck k93dmgtool k93ipatool k93smc; do
    install -m 0755 "$GBIN/$t" "$ROOT/usr/bin/$t"
done

# ------------------------------------------------------------- app bundles
#   <binary> <BundleName>
APPS="
springboard SpringBoard
settings Settings
files Files
terminal Terminal
installer Installer
installtohd InstallToHD
diagnostics Diagnostics
safari Safari
notes Notes
calendar Calendar
contacts Contacts
music Music
photos Photos
camera Camera
store Store
youtube YouTube
maps Maps
gamecenter GameCenter
trustme TrustMe
adsheet AdSheet
"
while read -r app bundle; do
    [ -n "$app" ] || continue
    d="$ROOT/Applications/$bundle.app"
    mkdir -p "$d"
    install -m 0755 "$GBIN/$app" "$d/$bundle"
    ident="com.k93.$(printf '%s' "$bundle" | tr 'A-Z' 'a-z')"
    cat > "$d/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>$bundle</string>
  <key>CFBundleDisplayName</key><string>$bundle</string>
  <key>CFBundleIdentifier</key><string>$ident</string>
  <key>CFBundleVersion</key><string>1.0</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleExecutable</key><string>$bundle</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>K93Platform</key><string>x86_64</string>
</dict>
</plist>
EOF
done <<< "$APPS"

# ------------------------------------------------------------- services
cat > "$ROOT/etc/launchd/com.k93.springboard.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<dict>
  <key>Label</key><string>com.k93.springboard</string>
  <key>ProgramArguments</key>
  <array><string>/Applications/SpringBoard.app/SpringBoard</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
</dict>
</plist>
EOF

cat > "$ROOT/etc/k93/launchdfallback.sh" <<'EOF'
#!/bin/sh
# used by init when /sbin/launchd is missing
echo "K93OS: launchd missing; interactive shell on console"
exec /bin/sh
EOF
chmod 0755 "$ROOT/etc/k93/launchdfallback.sh"

install -m 0755 "$TOP/userspace/rootfs-files/autotest.sh" "$ROOT/system/bin/autotest.sh"

# DHCP lease hook used by busybox udhcpc (invoked by init on eth0)
mkdir -p "$ROOT/usr/share/udhcpc"
install -m 0755 "$TOP/userspace/rootfs-files/udhcpc-default.script" \
    "$ROOT/usr/share/udhcpc/default.script"

# ------------------------------------------------------------- system fonts
# Copy the first known OPEN-LICENSE TTF trio (Sans/Serif/Mono) found on the
# build host into the OS. Candidate list is limited to fonts whose licenses
# are non-proprietary (OFL: Liberation/Noto, Bitstream Vera: DejaVu,
# GPL+FE: FreeFont, Apache-2.0: Droid) - never anything proprietary, never
# downloaded: detection only. A host with none of them simply leaves the
# embedded 8x16 font as the default (k93font falls back).
FONTD="$ROOT/usr/share/fonts/k93"
mkdir -p "$FONTD"
try_font() {                       # $1 = target base name, $2/$3 = srcs
    [ -f "$2" ] || return 1
    cp -f "$2" "$FONTD/$1-Regular.ttf"
    [ -f "$3" ] && cp -f "$3" "$FONTD/$1-Bold.ttf"
    echo "k93rootfs: font installed: $1"
    return 0
}
try_sans() {
    try_font LiberationSans /usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf /usr/share/fonts/liberation-sans-fonts/LiberationSans-Bold.ttf ||
    try_font LiberationSans /usr/share/fonts/liberation2-sans-fonts/LiberationSans-Regular.ttf /usr/share/fonts/liberation2-sans-fonts/LiberationSans-Bold.ttf ||
    try_font DejaVuSans /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf /usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf ||
    try_font DejaVuSans /usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf /usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf ||
    try_font FreeSans /usr/share/fonts/truetype/freefont/FreeSans.ttf /usr/share/fonts/truetype/freefont/FreeSansBold.ttf ||
    try_font DroidSans /usr/share/fonts/google-droid-sans-fonts/DroidSans.ttf ""
}
try_serif() {
    try_font LiberationSerif /usr/share/fonts/liberation-serif-fonts/LiberationSerif-Regular.ttf /usr/share/fonts/liberation-serif-fonts/LiberationSerif-Bold.ttf ||
    try_font LiberationSerif /usr/share/fonts/liberation2-serif-fonts/LiberationSerif-Regular.ttf /usr/share/fonts/liberation2-serif-fonts/LiberationSerif-Bold.ttf ||
    try_font DejaVuSerif /usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf /usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf ||
    try_font FreeSerif /usr/share/fonts/truetype/freefont/FreeSerif.ttf /usr/share/fonts/truetype/freefont/FreeSerifBold.ttf
}
try_mono() {
    try_font LiberationMono /usr/share/fonts/liberation-mono-fonts/LiberationMono-Regular.ttf /usr/share/fonts/liberation-mono-fonts/LiberationMono-Bold.ttf ||
    try_font LiberationMono /usr/share/fonts/liberation2-mono-fonts/LiberationMono-Regular.ttf /usr/share/fonts/liberation2-mono-fonts/LiberationMono-Bold.ttf ||
    try_font DejaVuSansMono /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf /usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf ||
    try_font FreeMono /usr/share/fonts/truetype/freefont/FreeMono.ttf /usr/share/fonts/truetype/freefont/FreeMonoBold.ttf ||
    try_font DroidSansMono /usr/share/fonts/google-droid-sans-mono-fonts/DroidSansMono.ttf ""
}
try_sans || true
try_serif || true
try_mono || true

# ------------------------------------------------------------- config
cat > "$ROOT/etc/k93/proxy.conf" <<'EOF'
# K93OS HTTP proxy configuration (read by Safari and network tools)
PROXY_ENABLE=0
PROXY_HOST=
PROXY_PORT=0
EOF

# display: landscape default (spare-PC monitor stays physically landscape);
# static orientation - no auto-rotate. Settable in Settings > Display.
cat > "$ROOT/etc/k93/display.conf" <<'EOF'
# K93OS display configuration (read by every graphical app)
MODE=auto
ORIENTATION=landscape
EOF

cat > "$ROOT/etc/passwd" <<'EOF'
root:x:0:0:root:/root:/bin/sh
mobile:x:501:501:mobile user:/var/mobile:/bin/sh
EOF
cat > "$ROOT/etc/group" <<'EOF'
root:x:0:
mobile:x:501:
EOF
echo "k93os" > "$ROOT/etc/hostname"
cat > "$ROOT/etc/hosts" <<'EOF'
127.0.0.1 localhost
::1       localhost
EOF
echo "nameserver 8.8.8.8" > "$ROOT/etc/resolv.conf"
echo "K93OS-x86 - clean-room tablet OS on Linux (serial console)" > "$ROOT/etc/issue"
cat > "$ROOT/etc/os-release" <<'EOF'
NAME="K93OS-x86"
VERSION="1.0"
ID=k93os
PRETTY_NAME="K93OS-x86 1.0"
EOF

# modern CA store (from the host's maintained bundle)
for CABUNDLE in /etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem \
                /etc/pki/tls/certs/ca-bundle.crt \
                /etc/ssl/certs/ca-certificates.crt; do
    [ -s "$CABUNDLE" ] && break
done
test -s "$CABUNDLE" || { echo "no host CA bundle found" >&2; exit 1; }
install -m 0644 "$CABUNDLE" "$ROOT/etc/ssl/certs/ca-certificates.crt"

# ------------------------------------------------------------- fixtures
if [ ! -f "$TOP/fixtures/test.ipa" ]; then
    python3 "$TOP/tools/mkfixtures.py" "$TOP/fixtures"
fi
install -m 0644 "$TOP/fixtures/"*.ipa "$TOP/fixtures/"*.dmg "$TOP/fixtures/"*.deb "$ROOT/opt/k93/fixtures/"
install -m 0644 "$TOP/fixtures/"*.html "$ROOT/opt/k93/fixtures/" 2>/dev/null || true

# SDK smoke artifact: build + bundle the C example so the guest autotest can
# sideload a real app-sdk product (skip gracefully without zig/cc)
if "$TOP/app-sdk/bin/k93sdk" build "$TOP/app-sdk/examples/c-hello" >/dev/null 2>&1 && \
   "$TOP/app-sdk/bin/k93sdk" bundle "$TOP/app-sdk/examples/c-hello" >/dev/null 2>&1; then
    install -m 0644 "$TOP/app-sdk/examples/c-hello/dist/Hello.ipa" "$ROOT/opt/k93/fixtures/"
    install -m 0644 "$TOP/app-sdk/examples/c-hello/dist/Hello.deb" "$ROOT/opt/k93/fixtures/hello-sdk.deb"
else
    echo "mkrootfs: SDK example build skipped (no toolchain)"
fi

# runtime placeholder dirs (also created by init at boot)
touch "$ROOT/var/db/k93/apps.log"

echo "rootfs: $ROOT"
du -sh "$ROOT"
