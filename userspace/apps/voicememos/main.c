/* VoiceMemos.app - clean-room counterpart of iOS 4.0 Voice Memos.
 * Records S16_LE PCM WAV through the ALSA capture node (k93snd), then lists
 * and plays clips. Ceiling: no microphone in the QEMU test VM - recording
 * reports "No capture device" there; playback needs a real sound card. */
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

#define DIRN  "/var/mobile/Library/VoiceMemos"
#define RATE  22050
#define CHANS 1

static void wav_header(unsigned char h[44], unsigned data_bytes, unsigned rate, unsigned chans)
{
    unsigned byte_rate = rate * chans * 2, riff = data_bytes + 36;
    memcpy(h, "RIFF", 4);
    h[4] = riff; h[5] = riff >> 8; h[6] = riff >> 16; h[7] = riff >> 24;
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    h[16] = 16; h[17] = h[18] = h[19] = 0;
    h[20] = 1; h[21] = 0;                       /* PCM */
    h[22] = chans; h[23] = 0;
    h[24] = rate; h[25] = rate >> 8; h[26] = rate >> 16; h[27] = rate >> 24;
    h[28] = byte_rate; h[29] = byte_rate >> 8; h[30] = byte_rate >> 16; h[31] = byte_rate >> 24;
    h[32] = chans * 2; h[33] = 0;               /* block align */
    h[34] = 16; h[35] = 0;                      /* bits per sample */
    memcpy(h + 36, "data", 4);
    h[40] = data_bytes; h[41] = data_bytes >> 8; h[42] = data_bytes >> 16; h[43] = data_bytes >> 24;
}

/* Records until Enter; writes a canonical WAV. Returns 0, or -1 if no capture
 * device. The capture fd is nonblocking, so the UI loop is never stalled. */
static int record_wav(const char *path)
{
    int rate = RATE, chans = CHANS;
    if (k93_snd_cap_open(NULL, &rate, &chans) < 0) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { k93_snd_cap_close(); return -1; }
    unsigned char h[44], buf[8192];
    unsigned total = 0;
    wav_header(h, 0, (unsigned)rate, (unsigned)chans);
    if (write(fd, h, 44) != 44) { close(fd); k93_snd_cap_close(); return -1; }
    ui_statusmsg("Recording ...  Enter stop");
    for (;;) {
        ssize_t n = k93_snd_cap_read(buf, sizeof buf / (size_t)(chans * 2));
        if (n > 0) {
            size_t bytes = (size_t)n * (size_t)(chans * 2);
            size_t off = 0;
            while (off < bytes) {
                ssize_t w = write(fd, buf + off, bytes - off);
                if (w <= 0) goto done;
                off += (size_t)w;
            }
            total += (unsigned)bytes;
        }
        if (ui_getkey(0) == UI_KEY_ENTER) break;
    }
done:
    k93_snd_cap_close();
    wav_header(h, total, (unsigned)rate, (unsigned)chans);   /* patch sizes */
    lseek(fd, 0, SEEK_SET);
    if (write(fd, h, 44) != 44) { /* size patch best-effort */ }
    close(fd);
    return 0;
}

static void play_wav(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { ui_msgbox("Voice Memos", "Cannot open file"); return; }
    unsigned char h[44];
    if (read(fd, h, 44) != 44 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        close(fd);
        ui_msgbox("Voice Memos", "Not a WAV file.");
        return;
    }
    unsigned chans = h[22] | (h[23] << 8);
    unsigned rate  = h[24] | (h[25] << 8) | (h[26] << 16) | (h[27] << 24);
    int r = (int)rate, c = (int)chans;
    if (k93_snd_open(NULL, &r, &c) < 0) {
        close(fd);
        ui_msgbox("Voice Memos", "No sound device.\n\nNeeds a kernel sound card "
                               "(e.g. snd-hda-intel).");
        return;
    }
    if (r != (int)rate || c != (int)chans) {
        k93_snd_close();
        close(fd);
        ui_msgbox("Voice Memos", "Recording rate/channels differ from this sound card.\n\n"
                               "No resampling in this build.");
        return;
    }
    ui_statusmsg("Playing ...  Esc stop");
    char buf[8192];
    size_t frame = 2u * (unsigned)c;         /* S16 = 2 bytes/sample */
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        if (k93_snd_play(buf, (size_t)n / frame) < 0) break;
        if (ui_getkey(0) == UI_KEY_ESC) break;
    }
    k93_snd_close();
    close(fd);
    ui_statusmsg("Stopped");
}

int main(void)
{
    if (ui_init("Voice Memos") < 0) return 1;
    k93_mkdir_p(DIRN, 0777);
    for (;;) {
        char names[64][128];
        const char *items[66];
        int nlist = 0;
        DIR *d = opendir(DIRN);
        struct dirent *e;
        while (d && (e = readdir(d)) && nlist < 64) {
            size_t l = strlen(e->d_name);
            if (l < 5 || strcmp(e->d_name + l - 4, ".wav")) continue;
            k93_strlcpy(names[nlist], e->d_name, 128);
            names[nlist][l - 4] = 0;
            nlist++;
        }
        if (d) closedir(d);

        int n = 0;
        items[n++] = "+ New recording";
        for (int i = 0; i < nlist; i++) items[n++] = names[i];
        items[n++] = "Back";
        int sel = ui_menu("Voice Memos", items, n, 0);
        if (sel < 0 || sel == n - 1) break;
        if (sel == 0) {
            char name[96] = "Memo";
            if (ui_input("Recording name", name, sizeof name) != 0 || !name[0]) continue;
            for (char *p = name; *p; p++)
                if (*p == '/' || *p == '\n' || *p == '\r') *p = '_';
            char path[256];
            snprintf(path, sizeof path, "%s/%s.wav", DIRN, name);
            if (record_wav(path) < 0)
                ui_msgbox("Voice Memos", "No capture device.\n\nNeeds a kernel sound card.");
            else
                ui_statusmsg("recorded");
            continue;
        }
        char path[256];
        snprintf(path, sizeof path, "%s/%s.wav", DIRN, names[sel - 1]);
        const char *acts[] = { "Play", "Delete", "Cancel" };
        int a = ui_menu(names[sel - 1], acts, 3, 0);
        if (a == 0) play_wav(path);
        else if (a == 1 && ui_confirm("Voice Memos", "Delete this recording?")) unlink(path);
    }
    ui_shutdown();
    return 0;
}
