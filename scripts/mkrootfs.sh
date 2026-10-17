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

# ------------------------------------------------------------- config
cat > "$ROOT/etc/k93/proxy.conf" <<'EOF'
# K93OS HTTP proxy configuration (read by Safari and network tools)
PROXY_ENABLE=0
PROXY_HOST=
PROXY_PORT=0
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
install -m 0644 "$TOP/fixtures/"*.ipa "$TOP/fixtures/"*.dmg "$ROOT/opt/k93/fixtures/"

# runtime placeholder dirs (also created by init at boot)
touch "$ROOT/var/db/k93/apps.log"

echo "rootfs: $ROOT"
du -sh "$ROOT"
