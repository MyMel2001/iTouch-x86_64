/* init.c - K93OS-x86 PID 1 (clean-room).
 * Responsibilities (the parts of launchd/Apple's init we observed in the
 * supplied 4.3.4 rootfs: /sbin/launchd is PID 1 and supervises LaunchDaemons):
 *   - mount kernel filesystems, set up /dev, /run, /tmp
 *   - bring up loopback (+ eth0 via DHCP when present)
 *   - hand off to launchd (service supervisor) and respawn it if it dies
 *   - autotest mode (kernel cmdline "autotest"): run self-tests, then poweroff
 * Everything here is original code; the behavioural model comes from public
 * POSIX and from observing launchd's role, not from Apple's sources. */
#define _GNU_SOURCE
#include "k93util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static volatile sig_atomic_t got_child = 0;
static void on_sigchld(int s) { (void)s; got_child = 1; }

static void try_mount(const char *src, const char *tgt, const char *fstype,
                      unsigned long flags, const void *data)
{
    if (mount(src, tgt, fstype, flags, data) < 0 && errno != EBUSY)
        k93_log("init", "mount %s on %s (%s): %s", src, tgt, fstype, strerror(errno));
}

static void ifup(const char *ifname, const char *ip, const char *mask)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    k93_strlcpy(ifr.ifr_name, ifname, IFNAMSIZ);
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, ip, &sin->sin_addr);
    if (ioctl(s, SIOCSIFADDR, &ifr) < 0)
        k93_log("init", "SIOCSIFADDR %s: %s", ifname, strerror(errno));
    inet_pton(AF_INET, mask, &sin->sin_addr);
    ioctl(s, SIOCSIFNETMASK, &ifr);
    ioctl(s, SIOCGIFFLAGS, &ifr);
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0)
        k93_log("init", "SIOCSIFFLAGS %s: %s", ifname, strerror(errno));
    close(s);
}

static int run_cmd(char *const argv[])
{
    pid_t p = fork();
    if (p == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    if (p > 0) waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void poweroff_now(void)
{
    k93_log("init", "powering off");
    sync();
    reboot(RB_POWER_OFF);
    /* fallbacks if the kernel refuses */
    reboot(RB_AUTOBOOT);
    for (;;) pause();
}

static void autotest(void)
{
    k93_log("init", "autotest mode requested on kernel command line");
    char *argv[] = { "/bin/busybox", "sh", "/system/bin/autotest.sh", NULL };
    int rc = run_cmd(argv);
    printf("K93-AUTOTEST-RESULT: %s (%d)\n", rc == 0 ? "PASS" : "FAIL", rc);
    fflush(stdout);
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        dprintf(fd, "K93-AUTOTEST-RESULT: %s (%d)\n", rc == 0 ? "PASS" : "FAIL", rc);
        close(fd);
    }
    sleep(1);
    poweroff_now();
}

int main(void)
{
    setsid();
    signal(SIGPIPE, SIG_IGN);

    /* standard mounts (order matters: proc before anything reads it) */
    try_mount("proc", "/proc", "proc", 0, NULL);
    try_mount("sysfs", "/sys", "sysfs", 0, NULL);
    try_mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    k93_mkdir_p("/dev/pts", 0755);
    try_mount("devpts", "/dev/pts", "devpts", 0, "mode=0620,gid=5");
    k93_mkdir_p("/dev/shm", 0755);
    try_mount("tmpfs", "/dev/shm", "tmpfs", 0, "mode=1777");
    k93_mkdir_p("/run", 0755);
    try_mount("tmpfs", "/run", "tmpfs", 0, "mode=0755,size=32M");
    k93_mkdir_p("/tmp", 0777);
    try_mount("tmpfs", "/tmp", "tmpfs", 0, "mode=1777,size=64M");
    k93_mkdir_p("/var/log", 0755);
    k93_mkdir_p("/var/run", 0755);
    k93_mkdir_p("/var/db/k93", 0755);
    k93_mkdir_p("/var/mobile/Media/Downloads", 0777);
    k93_mkdir_p("/var/mobile/Library", 0777);
    k93_mkdir_p("/var/mobile/Library/Notes", 0777);
    k93_mkdir_p("/var/mobile/Library/Calendar", 0777);
    k93_mkdir_p("/var/mobile/Library/Contacts", 0777);
    k93_mkdir_p("/var/mobile/Media/Photos", 0777);
    k93_mkdir_p("/var/mobile/Media/Music", 0777);

    int hfd = open("/proc/sys/kernel/hostname", O_WRONLY);
    if (hfd >= 0) { dprintf(hfd, "k93os\n"); close(hfd); }

    /* first console banner */
    const char *banner =
        "K93OS-x86 (" __DATE__ ") - clean-room x86_64 OS inspired by the\n"
        "iPhone OS 4.3.4 (iPad, build 8K2) interface model. Linux 7.2 kernel.\n";
    int cfd = open("/dev/console", O_WRONLY);
    if (cfd >= 0) { write(cfd, banner, strlen(banner)); close(cfd); }
    printf("%s", banner);
    k93_log("init", "PID 1 starting, pid=%d", getpid());

    /* network: loopback always; first ethernet up + DHCP when a cable exists.
     * Load the virtio chain (test VMs) and the common real-hardware wired NIC
     * drivers; insmod of a driver whose device is absent is harmless. mii must
     * precede the 8139 drivers (insmod resolves no dependencies). */
    ifup("lo", "127.0.0.1", "255.0.0.0");
    if (access("/sys/class/net/eth0", F_OK) != 0) {
        const char *netmods[] = { "/lib/modules/failover.ko",
                                  "/lib/modules/net_failover.ko",
                                  "/lib/modules/virtio_net.ko",
                                  "/lib/modules/e1000.ko",
                                  "/lib/modules/e1000e.ko",
                                  "/lib/modules/r8169.ko",
                                  "/lib/modules/mii.ko",
                                  "/lib/modules/8139too.ko",
                                  "/lib/modules/8139cp.ko", NULL };
        for (int i = 0; netmods[i]; i++) {
            if (access(netmods[i], F_OK) != 0) continue;
            char *na[] = { "/bin/busybox", "insmod", (char *)netmods[i], NULL };
            run_cmd(na);
        }
        for (int j = 0; j < 30 && access("/sys/class/net/eth0", F_OK) != 0; j++)
            usleep(100000);
    }
    {
        const char *devs[] = { "eth0", "enp0s3", "ens3", NULL };
        for (int i = 0; devs[i]; i++) {
            char p[128];
            snprintf(p, sizeof p, "/sys/class/net/%s", devs[i]);
            if (access(p, F_OK) == 0) {
                ifup(devs[i], "0.0.0.0", "0.0.0.0");
                char *argv[] = { "/bin/busybox", "udhcpc", "-i", (char *)devs[i],
                                 "-n", "-q", "-t", "5", "-s",
                                 "/usr/share/udhcpc/default.script", NULL };
                pid_t p2 = fork();
                if (p2 == 0) { execvp(argv[0], argv); _exit(127); }
                break;
            }
        }
    }

    /* graphics bring-up: kernels without CONFIG_FB_DEVICE (e.g. Fedora's 7.2
     * config) never create /dev/fb0; the UI then uses DRM/KMS through
     * /dev/dri/card0. Deps first (insmod resolves nothing), then the actual
     * device drivers: virtio-gpu + bochs for the test VMs, i915 for real
     * Intel GPUs. A driver whose device is absent binds nothing and is harmless. */
    if (access("/dev/dri/card0", F_OK) != 0) {
        static const char *drmdeps[] = { "/lib/modules/virtio_dma_buf.ko",
                                         "/lib/modules/wmi.ko",
                                         "/lib/modules/video.ko",
                                         "/lib/modules/i2c_algo_bit.ko",
                                         "/lib/modules/cec.ko",
                                         "/lib/modules/ttm.ko",
                                         "/lib/modules/drm_buddy.ko",
                                         "/lib/modules/drm_display_helper.ko",
                                         NULL };
        for (int i = 0; drmdeps[i]; i++) {
            if (access(drmdeps[i], F_OK) != 0) continue;
            char *da[] = { "/bin/busybox", "insmod", (char *)drmdeps[i], NULL };
            run_cmd(da);
        }
        const char *mods[] = { "/lib/modules/virtio_gpu.ko",
                               "/lib/modules/bochs.ko",
                               "/lib/modules/i915.ko", NULL };
        for (int i = 0; mods[i] && access("/dev/dri/card0", F_OK) != 0; i++) {
            if (access(mods[i], F_OK) != 0) continue;
            char *insargv[] = { "/bin/busybox", "insmod", (char *)mods[i], NULL };
            run_cmd(insargv);
            for (int j = 0; j < 30 && access("/dev/dri/card0", F_OK) != 0; j++)
                usleep(100000);
        }
    }
    for (int i = 0; i < 150 && access("/dev/dri/card0", F_OK) != 0; i++)
        usleep(100000);
    k93_log("init", "graphics: %s",
            access("/dev/dri/card0", F_OK) == 0
                ? "DRM device ready (/dev/dri/card0)"
                : "no display (serial text UI)");

    /* sound: Intel HDA codec drivers FIRST, controller LAST. snd-hda-intel
     * probes and configures codecs synchronously during insmod, so the codec
     * drivers must be registered before it loads (there is no modprobe in the
     * guest to resolve codec aliases). QEMU's intel-hda and real hardware
     * both expose the ALSA nodes under /dev/snd; Music.app plays via them. */
    {
        static const char *sndmods[] = { "/lib/modules/soundcore.ko",
                                         "/lib/modules/snd.ko",
                                         "/lib/modules/snd_timer.ko",
                                         "/lib/modules/snd_pcm.ko",
                                         "/lib/modules/snd_hwdep.ko",
                                         "/lib/modules/snd_hda_core.ko",
                                         "/lib/modules/snd_intel_sdw_acpi.ko",
                                         "/lib/modules/snd_intel_dspcfg.ko",
                                         "/lib/modules/snd_hda_codec.ko",
                                         "/lib/modules/snd_hda_codec_generic.ko",
                                         "/lib/modules/snd_hda_codec_hdmi.ko",
                                         "/lib/modules/snd_hda_codec_realtek_lib.ko",
                                         "/lib/modules/snd_hda_scodec_component.ko",
                                         "/lib/modules/snd_hda_codec_alc662.ko",
                                         "/lib/modules/snd_hda_codec_alc269.ko",
                                         "/lib/modules/snd_hda_codec_alc882.ko",
                                         "/lib/modules/snd_hda_intel.ko",
                                         NULL };
        for (int i = 0; sndmods[i]; i++) {
            if (access(sndmods[i], F_OK) != 0) continue;
            char *sa[] = { "/bin/busybox", "insmod", (char *)sndmods[i], NULL };
            run_cmd(sa);
        }
        for (int j = 0; j < 30 && access("/dev/snd/controlC0", F_OK) != 0; j++)
            usleep(100000);
        k93_log("init", "sound: %s",
                access("/dev/snd/controlC0", F_OK) == 0
                    ? "ALSA device ready (/dev/snd)"
                    : "no sound card");
    }

    if (k93_cmdline_flag("autotest")) {
        /* skip UI services entirely in test mode */
        autotest();
        return 0;
    }

    /* supervise launchd (service manager) like the original init -> launchd hop */
    signal(SIGCHLD, on_sigchld);
    for (;;) {
        pid_t pid = fork();
        if (pid == 0) {
            execl("/sbin/launchd", "launchd", (char *)NULL);
            /* fall back to busybox sh if our supervisor is missing */
            execl("/bin/busybox", "busybox", "sh", "/etc/k93/launchdfallback.sh", NULL);
            _exit(127);
        }
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
        k93_log("init", "launchd exited (status %d), restarting in 2s", st);
        sleep(2);
    }
    return 0;
}
