#!/bin/sh
# K93OS-x86 autotest - run by init when the kernel command line contains
# "autotest". Exercises every subsystem that can be validated without a
# human at the console. Exit status 0 = PASS; init prints the result line
# "K93-AUTOTEST-RESULT: PASS|FAIL" to the console and powers off.
FAIL=0
step() { echo "autotest: $*"; }
run() {
    name="$1"; shift
    step "$name"
    if "$@" > /tmp/autotest.out 2>&1; then
        step "  ok"
    else
        step "  FAIL ($*)"
        sed 's/^/    /' /tmp/autotest.out
        FAIL=1
    fi
}

run selfcheck          /usr/bin/k93selfcheck /opt/k93/fixtures
run ipa-list           /usr/bin/k93ipatool list /opt/k93/fixtures/test.ipa
run ipa-install        /usr/bin/k93ipatool inst /opt/k93/fixtures/test.ipa
run ipa-app-registered sh -c '/usr/bin/k93ipatool apps | grep -q com.k93.fixture'
run ipa-arm-rejected   sh -c '! /usr/bin/k93ipatool inst /opt/k93/fixtures/arm.ipa'
run dmg-probe          /usr/bin/k93dmgtool probe /opt/k93/fixtures/test.dmg
run dmg-extract        sh -c '/usr/bin/k93dmgtool x /opt/k93/fixtures/test.dmg /tmp/out.raw'
run dmg-payload        sh -c 'dd if=/tmp/out.raw bs=1 count=10 2>/dev/null | grep -q K93DMGTEST'
run hw-status          /usr/bin/k93smc
run hw-modules         sh -c 'test -s /lib/modules/loadlist && test $(wc -l < /lib/modules/loadlist) -ge 40'
run hw-driver-set      sh -c 'for m in amdgpu nouveau i915 r8169 iwlwifi ath9k snd_hda_intel; do grep -qx "$m" /lib/modules/loadlist || exit 1; done'
run hw-firmware        sh -c 'test -s /lib/firmware/regulatory.db && test -s /lib/firmware/iwlwifi-100-5.ucode.xz && test -d /lib/firmware/amdgpu'
run ca-store           sh -c 'grep -q "BEGIN CERTIFICATE" /etc/ssl/certs/ca-certificates.crt'
run proxy-conf         sh -c 'grep -q PROXY_ENABLE /etc/k93/proxy.conf'
run proxy-write        sh -c 'printf "PROXY_ENABLE=1\nPROXY_HOST=10.0.2.2\nPROXY_PORT=8080\n" > /etc/k93/proxy.conf && grep -q 10.0.2.2 /etc/k93/proxy.conf'
run proxy-restore      sh -c 'printf "PROXY_ENABLE=0\nPROXY_HOST=\nPROXY_PORT=0\n" > /etc/k93/proxy.conf'
run display-conf       sh -c 'grep -q "^MODE=" /etc/k93/display.conf && grep -q "^ORIENTATION=" /etc/k93/display.conf'
run apps-present       sh -c 'test -x /Applications/SpringBoard.app/SpringBoard && test -x /Applications/Files.app/Files && test -x /Applications/Settings.app/Settings'
run app-plists-parse   sh -c 'for p in /Applications/*.app/Info.plist; do grep -q CFBundleIdentifier "$p" || exit 1; done'
run gpt-file           sh -c '/usr/bin/k93selfcheck /opt/k93/fixtures | grep -q "gpt write"'
run launchd-jobs       sh -c 'test -f /etc/launchd/com.k93.springboard.plist'
run shell-present      sh -c '/bin/sh -c "echo busybox-ok" | grep -q busybox-ok'

# networking: kernel NIC driver + udhcpc (QEMU user-mode netdev: gw 10.0.2.2)
if [ -e /sys/class/net/eth0 ]; then
    step "eth0 present (driver: $(sed -n 's/^DRIVER=//p' /sys/class/net/eth0/device/uevent 2>/dev/null))"
    run net-lease       sh -c 'ifconfig eth0 | grep -q "inet addr"'
    run net-ping-gw     sh -c 'ping -c 2 -W 2 10.0.2.2 >/dev/null'
else
    step "no eth0 (net test skipped - VM has no NIC)"
fi

# graphics stack: fbdev (legacy) or DRM/KMS (modern kernels, no CONFIG_FB_DEVICE)
if [ -e /dev/fb0 ]; then
    step "framebuffer present: $(cat /sys/class/graphics/fb0/name 2>/dev/null)"
    run fb-info         sh -c 'grep -q . /sys/class/graphics/fb0/bits_per_pixel'
    run fb-draw-verify  sh -c '/usr/bin/k93selfcheck /opt/k93/fixtures | grep -q "fb pattern write/readback"'
elif [ -e /dev/dri/card0 ]; then
    step "DRM/KMS device present: $(ls /dev/dri 2>/dev/null | tr '\n' ' ')"
    run kms-draw-verify sh -c '/usr/bin/k93selfcheck /opt/k93/fixtures | grep -q "kms dumb buffer write/readback"'
else
    step "no display device (headless text VM)"
fi

# sound: ALSA PCM from the kernel snd stack (QEMU intel-hda / real hardware)
if [ -e /dev/snd/pcmC0D0p ]; then
    step "ALSA PCM device present: $(ls /dev/snd 2>/dev/null | tr '\n' ' ')"
    run alsa-pcm-write  sh -c '/usr/bin/k93selfcheck /opt/k93/fixtures | grep -q "alsa pcm write"'
else
    step "no ALSA PCM device (VM has no sound card)"
fi

# block device present? (InstallToHD target; only when a second disk is attached)
if [ -b /dev/vdb ]; then
    step "second disk /dev/vdb present - InstallToHD can target it"
else
    step "no second disk (fine for initrd-only runs)"
fi

exit $FAIL
