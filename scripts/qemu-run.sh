#!/usr/bin/env bash
# Interactive QEMU run: full boot to SpringBoard on the serial console.
#   - serial is your terminal (type to the UI)
#   - graphics: stdvga + bochs-drm (DRM/KMS), viewable with any VNC client at
#     127.0.0.1:5900+K93_VNC_DISPLAY (default :77 -> port 5977)
#   - quit QEMU with Ctrl-A then X
#   - power off from the UI: SpringBoard > Settings > General > Shut Down
set -euo pipefail
TOP="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$TOP/build"
KERNEL="$(ls -1 /usr/lib/modules/*/vmlinuz 2>/dev/null | sort -V | tail -1)"

[ -n "$KERNEL" ] || { echo "no guest kernel found under /usr/lib/modules" >&2; exit 1; }
[ -f "$BUILD/initrd.cpio.gz" ] || bash "$TOP/scripts/mkinitrd.sh"
[ -f "$BUILD/hdd16.img" ]      || bash "$TOP/scripts/mkdisk.sh"

if [ -w /dev/kvm ]; then ACCEL=(-accel kvm -cpu host); else ACCEL=(-accel tcg -cpu max); fi

exec qemu-system-x86_64 \
    -m 1536 -smp 1 -machine pc "${ACCEL[@]}" \
    -kernel "$KERNEL" \
    -initrd "$BUILD/initrd.cpio.gz" \
    -append "console=ttyS0,115200 loglevel=4" \
    -drive "file=$BUILD/hdd16.img,if=virtio,format=raw" \
    -vga std \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
    -device intel-hda -device hda-duplex \
    -display "vnc=127.0.0.1:${K93_VNC_DISPLAY:-77}" \
    -serial mon:stdio -no-reboot
