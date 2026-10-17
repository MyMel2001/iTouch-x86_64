#!/bin/bash
# Repro: initrd-only QEMU, virtio-gpu, screendump, QEMU virtio-gpu trace.
# Usage: repro-graphics.sh [vga-mode]   (vga-mode: virtgpu | std)
set -u
cd "$(dirname "$0")/.."
QEMU=/home/linuxbrew/.linuxbrew/bin/qemu-system-x86_64
MODE="${1:-virtgpu}"
KERNEL="$(ls -1 /usr/lib/modules/*/vmlinuz | sort -V | tail -1)"
if [ -w /dev/kvm ]; then ACCEL=(-accel kvm -cpu host); else ACCEL=(-accel tcg -cpu max); fi

if [ "$MODE" = std ]; then
  VGA=(-vga std)
else
  VGA=(-vga none -device virtio-gpu-pci)
fi

rm -f build/serial-fb.log build/qmp-fb.sock build/screen-fb.ppm build/vgtrace.log build/qemu-stderr.log
mkdir -p build
"$QEMU" -m 1536 -smp 1 -machine pc "${ACCEL[@]}" \
    -kernel "$KERNEL" -initrd build/initrd.cpio.gz \
    -append "console=ttyS0,115200 loglevel=4" \
    -drive "file=build/hdd16.img,if=virtio,format=raw" \
    "${VGA[@]}" -display none \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
    -qmp "unix:build/qmp-fb.sock,server,nowait" \
    -serial "file:build/serial-fb.log" -monitor none -no-reboot \
    -trace enable=virtio_gpu_cmd_set_scanout \
    -trace enable=virtio_gpu_cmd_res_flush \
    -trace enable=virtio_gpu_cmd_res_xfer_toh_2d \
    -trace enable=virtio_gpu_cmd_res_back_attach \
    -trace enable=virtio_gpu_cmd_res_create_2d \
    -D build/vgtrace.log 2>build/qemu-stderr.log &
echo $! > /tmp/opencode/qfb.pid
for i in $(seq 1 90); do
  grep -q "started com.k93.springboard" build/serial-fb.log 2>/dev/null && break
  kill -0 "$(cat /tmp/opencode/qfb.pid)" 2>/dev/null || break
  sleep 1
done
sleep 8
python3 tools/qmp-screendump.py build/qmp-fb.sock build/screen-fb.ppm
kill "$(cat /tmp/opencode/qfb.pid)" 2>/dev/null
wait "$(cat /tmp/opencode/qfb.pid)" 2>/dev/null

echo "=== guest k93ui probes ==="
grep -aE "k93ui:" build/serial-fb.log | tail -8
echo "=== green pixel count + nonblack ==="
python3 - <<'PY'
d=open('build/screen-fb.ppm','rb').read().split(b'\n',3)[3]
W,H=1280,800
def px(x,y):
    i=(y*W+x)*3; return d[i],d[i+1],d[i+2]
green=sum(1 for y in range(0,H,2) for x in range(0,W,2) if px(x,y)==(0,255,0))
nonblack=sum(1 for y in range(0,H,2) for x in range(0,W,2) if px(x,y)!=(0,0,0))
print(f"green={green} nonblack={nonblack} (sampled 1/4)")
PY
echo "=== QEMU scanout summary ==="
grep -cE "set_scanout id 0, res 0x3" build/vgtrace.log 2>/dev/null
echo "last set_scanout:"; grep -E "set_scanout" build/vgtrace.log | tail -2
