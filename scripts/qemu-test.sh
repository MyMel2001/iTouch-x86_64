#!/usr/bin/env bash
# Automated QEMU test: boots the initrd with kernel cmdline "autotest",
# captures the serial console to build/serial.log, and asserts
# "K93-AUTOTEST-RESULT: PASS".
set -euo pipefail
TOP="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$TOP/build"
LOG="$BUILD/serial.log"
KERNEL="$(ls -1 /usr/lib/modules/*/vmlinuz 2>/dev/null | sort -V | tail -1)"

[ -n "$KERNEL" ] || { echo "no guest kernel found under /usr/lib/modules" >&2; exit 1; }
[ -f "$BUILD/initrd.cpio.gz" ] || bash "$TOP/scripts/mkinitrd.sh"
[ -f "$BUILD/hdd16.img" ]      || bash "$TOP/scripts/mkdisk.sh"

if [ -w /dev/kvm ]; then ACCEL=(-accel kvm -cpu host); else ACCEL=(-accel tcg -cpu max); fi

rm -f "$LOG"
set +e
timeout 240 qemu-system-x86_64 \
    -m 1536 -smp 1 -machine pc "${ACCEL[@]}" \
    -kernel "$KERNEL" \
    -initrd "$BUILD/initrd.cpio.gz" \
    -append "console=ttyS0,115200 autotest loglevel=4" \
    -drive "file=$BUILD/hdd16.img,if=virtio,format=raw" \
    -display none -serial "file:$LOG" -monitor none -no-reboot
qrc=$?
set -e

echo "--- serial tail ---"
tail -30 "$LOG" 2>/dev/null || true
if grep -q "K93-AUTOTEST-RESULT: PASS" "$LOG"; then
    echo "QEMU AUTOTEST PASS (qemu rc=$qrc, log: $LOG)"
    exit 0
fi
echo "QEMU AUTOTEST FAIL (qemu rc=$qrc, log: $LOG)"
exit 1
