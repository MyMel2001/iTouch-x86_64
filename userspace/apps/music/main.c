/* Music.app - clean-room counterpart of MobileMusicPlayer.app.
 * The VM has no audio device; this app browses /var/mobile/Media/Music,
 * shows metadata-ish info (size, name) and "plays" by streaming to OSS
 * /dev/dsp when present, else to the ALSA PCM node via k93snd (real
 * hardware sound cards). Ceiling: no audio in the QEMU test VM. */
#define _GNU_SOURCE
#include "k93ui.h"
#include "k93snd.h"
#include "k93util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#define DIRN "/var/mobile/Media/Music"

/* ALSA playback fallback: modern kernels have no OSS (/dev/dsp), only the
 * ALSA PCM node. Negotiate the WAV's own rate/channels, then stream frames. */
static int play_alsa(int fd, unsigned rate, unsigned chans)
{
    int r = (int)rate, c = (int)chans;
    if (k93_snd_open(NULL, &r, &c) < 0) return -1;
    if (r != (int)rate || c != (int)chans) {
        /* file rate differs from what the device gave us; the client does no
         * resampling, so tell the user rather than play at the wrong pitch */
        k93_snd_close();
        errno = EINVAL;
        return -1;
    }
    char buf[8192];
    size_t frame = 2u * (unsigned)c;   /* S16 = 2 bytes/sample */
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        if (k93_snd_play(buf, (size_t)n / frame) < 0) break;
        if (ui_getkey(0) == UI_KEY_ESC) break;
    }
    k93_snd_close();
    return 0;
}

static void play_wav(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { ui_msgbox("Music", "Cannot open file"); return; }
    unsigned char hdr[44];
    if (read(fd, hdr, 44) != 44 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        close(fd);
        ui_msgbox("Music", "Only PCM WAV files can be played back.\n"
                           "This build has no decoder for MP3/AAC (clean-room scope).");
        return;
    }
    /* canonical WAV: "fmt " at 12, PCM tag at 20, channels at 22, rate at 24 */
    if (memcmp(hdr + 12, "fmt ", 4) != 0 || hdr[20] != 1) {
        close(fd);
        ui_msgbox("Music", "Only uncompressed PCM WAV files can be played.");
        return;
    }
    unsigned chans = hdr[22] | (hdr[23] << 8);
    unsigned rate  = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
    int dsp = open("/dev/dsp", O_WRONLY);
    if (dsp < 0) dsp = open("/dev/dsp0", O_WRONLY);
    if (dsp < 0) {
        /* no OSS emulation in this kernel: try the ALSA PCM node instead */
        ui_statusmsg("Playing (ALSA) ...");
        if (play_alsa(fd, rate, chans) < 0) {
            char msg[256];
            snprintf(msg, sizeof msg,
                     "Valid WAV (%u Hz, %u ch) but no sound device.\n\n"
                     "Needs a kernel sound card (e.g. snd-hda-intel).",
                     rate, chans);
            ui_msgbox("Music", msg);
        }
        close(fd);
        ui_statusmsg("Stopped");
        return;
    }
    /* trivial "now playing": stream raw samples, no resampling */
    ui_statusmsg("Playing ...");
    char buf[8192];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) {
        char *p = buf;
        while (r > 0) {
            ssize_t w = write(dsp, p, (size_t)r);
            if (w <= 0) break;
            p += w; r -= w;
        }
        if (ui_getkey(0) == UI_KEY_ESC) break;
    }
    close(dsp);
    close(fd);
    ui_statusmsg("Stopped");
}

int main(void)
{
    if (ui_init("Music") < 0) return 1;
    k93_mkdir_p(DIRN, 0777);
    for (;;) {
        char names[64][128];
        const char *items[70];
        int n = 0;
        DIR *d = opendir(DIRN);
        struct dirent *e;
        while (d && (e = readdir(d)) && n < 60) {
            size_t l = strlen(e->d_name);
            if (l < 4) continue;
            k93_strlcpy(names[n], e->d_name, sizeof names[n]);
            items[n] = names[n];
            n++;
        }
        if (d) closedir(d);
        items[n] = "Back";
        int sel = ui_menu("Music library", items, n + 1, 0);
        if (sel < 0 || sel == n) break;
        char path[256];
        snprintf(path, sizeof path, "%s/%s", DIRN, names[sel]);
        struct stat st;
        char info[256];
        if (stat(path, &st) == 0) {
            char h[32];
            k93_human_size(h, sizeof h, (unsigned long long)st.st_size);
            snprintf(info, sizeof info, "%s\n%s\n\nPlay this file?", names[sel], h);
        } else {
            snprintf(info, sizeof info, "%s\n\nPlay this file?", names[sel]);
        }
        if (ui_confirm("Music", info)) play_wav(path);
    }
    ui_shutdown();
    return 0;
}
