#!/usr/bin/env bash
# Disk images:
#   build/hdd16.img   - 16 GiB sparse data disk (target for InstallToHD)
#   build/osdisk.img  - ext4 "OS disk image": the rootfs populated offline
#                       (mountable loopback; mke2fs -d needs no root)
set -euo pipefail
TOP="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$TOP/build/rootfs"
BUILD="$TOP/build"

[ -d "$ROOT" ] || bash "$TOP/scripts/mkrootfs.sh"

# 16 GiB sparse data disk (no partition table: InstallToHD writes the GPT)
if [ ! -f "$BUILD/hdd16.img" ]; then
    truncate -s 16G "$BUILD/hdd16.img"
fi

# OS disk image: ext4 with the whole rootfs, sized to fit
kb=$(du -sk "$ROOT" | cut -f1)
mb=$(( kb * 13 / 10 / 1024 + 64 ))
rm -f "$BUILD/osdisk.img"
truncate -s "${mb}M" "$BUILD/osdisk.img"
mke2fs -q -F -t ext4 -L K93OS -d "$ROOT" "$BUILD/osdisk.img"

echo "disk:   $BUILD/hdd16.img (16G sparse)"
echo "osdisk: $BUILD/osdisk.img (${mb}M ext4, populated from rootfs)"
