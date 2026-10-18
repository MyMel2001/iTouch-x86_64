#!/usr/bin/env bash
# Disk images:
#   build/hdd16.img   - 16 GiB sparse data disk (target for InstallToHD)
#   build/osdisk.img  - BIOS-bootable MBR disk: GRUB (rootless manual install)
#                       + ext4 partition at 1 MiB holding the rootfs, kernel
#                       and initrd. Also loopback-mountable at offset 1 MiB.
set -euo pipefail
TOP="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$TOP/build/rootfs"
BUILD="$TOP/build"
OFFSET=1048576   # 1 MiB: MBR boot code + GRUB core.img gap + partition start

[ -d "$ROOT" ] || bash "$TOP/scripts/mkrootfs.sh"
[ -f "$BUILD/initrd.cpio.gz" ] || bash "$TOP/scripts/mkinitrd.sh"

# 16 GiB sparse data disk (no partition table: InstallToHD writes the GPT)
if [ ! -f "$BUILD/hdd16.img" ]; then
    truncate -s 16G "$BUILD/hdd16.img"
fi

GRUBDIR=/usr/lib/grub/i386-pc
KERNEL="$(ls -1 /usr/lib/modules/*/vmlinuz 2>/dev/null | sort -V | tail -1)"

if ! command -v grub2-mkimage >/dev/null || [ -z "$KERNEL" ] || [ ! -f "$GRUBDIR/boot.img" ]; then
    # Fallback: plain ext4 image (no bootloader), still loopback-mountable.
    kb=$(du -sk "$ROOT" | cut -f1)
    mb=$(( kb * 13 / 10 / 1024 + 64 ))
    rm -f "$BUILD/osdisk.img"
    truncate -s "${mb}M" "$BUILD/osdisk.img"
    mke2fs -q -F -t ext4 -L K93OS -d "$ROOT" "$BUILD/osdisk.img"
    echo "osdisk: $BUILD/osdisk.img (${mb}M ext4, NO bootloader: grub2/kernel missing)"
    echo "disk:   $BUILD/hdd16.img (16G sparse)"
    exit 0
fi

# Staging tree = rootfs + /boot (hardlinks: no extra space, build/rootfs untouched
# so the initrd stays kernel+rootfs only)
STAGE="$BUILD/ostage"
rm -rf "$STAGE"
cp -al "$ROOT" "$STAGE"
mkdir -p "$STAGE/boot/grub2"
cp "$KERNEL" "$STAGE/boot/vmlinuz"
cp "$BUILD/initrd.cpio.gz" "$STAGE/boot/initrd.cpio.gz"
cat > "$STAGE/boot/grub2/grub.cfg" <<'CFG'
serial --unit=0 --speed=115200
terminal_output serial console
set timeout=0
set default=0
menuentry "K93OS-x86" {
    linux /boot/vmlinuz console=ttyS0,115200 loglevel=4
    initrd /boot/initrd.cpio.gz
}
CFG

# Partition content size (+30% slack), whole disk = 1 MiB gap + partition
kb=$(du -sk "$STAGE" | cut -f1)
mb=$(( kb * 13 / 10 / 1024 + 64 ))
rm -f "$BUILD/osdisk.img"
truncate -s "$(( OFFSET / 1048576 + mb ))M" "$BUILD/osdisk.img"

# Rootless GRUB: grub2-install/bios-setup need loop+root, so place images by hand.
# boot.img (512 B) carries an empty partition table, so it must go down BEFORE sfdisk.
dd if="$GRUBDIR/boot.img" of="$BUILD/osdisk.img" bs=512 count=1 conv=notrunc status=none
printf 'label: dos\nstart=2048, type=83, bootable\n' | sfdisk -q "$BUILD/osdisk.img" >/dev/null
# core.img fits the 1 MiB gap (sectors 1..2047); its embedded blocklist already
# points at sector 1 (boot.img default), so no patching needed.
grub2-mkimage -O i386-pc -o "$BUILD/grub-core.img" -p '(hd0,msdos1)/boot/grub2' \
    biosdisk part_msdos ext2 normal linux configfile search search_fs_file \
    echo test serial terminal
dd if="$BUILD/grub-core.img" of="$BUILD/osdisk.img" bs=512 seek=1 conv=notrunc status=none

# ext4 inside the partition (mke2fs -E offset: no loop device, no root)
mke2fs -q -F -t ext4 -b 4096 -L K93OS -E offset=$OFFSET -d "$STAGE" \
    "$BUILD/osdisk.img" $(( mb * 256 ))
rm -rf "$STAGE"

echo "disk:   $BUILD/hdd16.img (16G sparse)"
echo "osdisk: $BUILD/osdisk.img (BIOS+GRUB, $(( OFFSET / 1048576 + mb ))M, ext4 partition @1MiB)"
