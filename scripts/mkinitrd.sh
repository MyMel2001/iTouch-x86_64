#!/usr/bin/env bash
# Pack build/rootfs into a gzip-compressed cpio initramfs (build/initrd.cpio.gz).
set -euo pipefail
TOP="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$TOP/build/rootfs"
OUT="$TOP/build/initrd.cpio.gz"

[ -d "$ROOT" ] || bash "$TOP/scripts/mkrootfs.sh"
( cd "$ROOT" && find . -print0 | cpio --null -o -H newc --quiet ) | gzip -9 > "$OUT"
echo "initrd: $OUT ($(du -h "$OUT" | cut -f1))"
