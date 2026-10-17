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
    -netdev user,id=net0 -device e1000,netdev=net0 \
    -device intel-hda -device hda-duplex \
    -display none -serial "file:$LOG" -monitor none -no-reboot
qrc=$?
set -e

echo "--- serial tail ---"
tail -30 "$LOG" 2>/dev/null || true
if ! grep -q "K93-AUTOTEST-RESULT: PASS" "$LOG"; then
    echo "QEMU AUTOTEST FAIL (qemu rc=$qrc, log: $LOG)"
    exit 1
fi
echo "QEMU AUTOTEST PASS (qemu rc=$qrc, log: $LOG)"

# ------------------------------------------------ phase 2: graphics stack
# Boot with a virtio-gpu display and a headless VNC server; validate the
# DRM/KMS stack by taking a QMP screendump of what a VNC viewer would see
# and checking it is rendered (SpringBoard draws via the DRM dumb-buffer
# backend in libk93).
GLOG="$BUILD/serial-graphics.log"
SOCK="$BUILD/qmp.sock"
PPM="$BUILD/screen.ppm"
rm -f "$GLOG" "$SOCK" "$PPM"
qemu-system-x86_64 \
    -m 1536 -smp 1 -machine pc "${ACCEL[@]}" \
    -kernel "$KERNEL" \
    -initrd "$BUILD/initrd.cpio.gz" \
    -append "console=ttyS0,115200 loglevel=4" \
    -drive "file=$BUILD/hdd16.img,if=virtio,format=raw" \
    -vga none -device virtio-gpu-pci -display vnc=127.0.0.1:78 \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
    -qmp "unix:$SOCK,server,nowait" \
    -serial "file:$GLOG" -monitor none -no-reboot &
QPID=$!
echo "graphics VM up (pid $QPID, VNC 127.0.0.1:5978); waiting for SpringBoard ..."
ok_wait=0
for _ in $(seq 1 90); do
    if grep -q "started com.k93.springboard" "$GLOG" 2>/dev/null; then ok_wait=1; break; fi
    kill -0 $QPID 2>/dev/null || break
    sleep 1
done
grc=1
if [ "$ok_wait" = 1 ]; then
    sleep 3                                       # first SpringBoard frame
    python3 "$TOP/tools/qmp-screendump.py" "$SOCK" "$PPM" && grc=0
fi
kill $QPID 2>/dev/null || true
wait $QPID 2>/dev/null || true
# SpringBoard must have used DRM/KMS: if the text-mode UI frames reached the
# serial log, the KMS device never appeared and the VM fell back to text.
if grep -q "SpringBoard - .* apps" "$GLOG" 2>/dev/null; then
    echo "GRAPHICS TEST FAIL: SpringBoard fell back to text mode (no KMS device)"
    exit 1
fi
if [ "$grc" = 0 ]; then
    echo "GRAPHICS TEST PASS (DRM/KMS + SpringBoard rendered: $PPM)"
    exit 0
fi
echo "GRAPHICS TEST FAIL (springboard_seen=$ok_wait, log: $GLOG)"
exit 1
