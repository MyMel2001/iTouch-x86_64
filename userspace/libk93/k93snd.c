/* k93snd.c - minimal clean-room ALSA PCM playback client.
 *
 * Modern kernels ship no OSS emulation (CONFIG_SND_OSSEMUL=n), so /dev/dsp
 * does not exist. Real audio (Intel HDA, and anything else the snd-* drivers
 * bind to) is reached through the ALSA PCM node /dev/snd/pcmC*D*p using the
 * kernel UAPI ioctls directly - no libasound, all original code.
 *
 * Scope: S16_LE interleaved stereo, blocking writes. Enough to play a PCM WAV
 * or synthesise a tone. No resampling, mixing, mmap ring, or compressed input. */
#include "k93util.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
/* Kernel UAPI annotation macros are GCC address/attribute spaces that do not
 * exist in userspace headers; define them empty so the header compiles. */
#ifndef __user
#define __user
#endif
#ifndef __force
#define __force
#endif
#ifndef __bitwise
#define __bitwise
#endif
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#include <linux/asound.h>

static int snd_fd = -1;

/* hw_params stores masks/intervals in arrays indexed by
 * (param - FIRST_MASK) and (param - FIRST_INTERVAL), not by the raw param
 * number. */
static struct snd_interval *hw_interval(struct snd_pcm_hw_params *hp, int param)
{
    return &hp->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

static void hw_set_mask(struct snd_pcm_hw_params *hp, int param, unsigned val)
{
    struct snd_mask *m = &hp->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m, 0, sizeof *m);
    m->bits[val / 32] = 1u << (val % 32);
}

/* Force a known-good config: S16_LE, interleaved, stereo, 44100 Hz.
 * HW_PARAMS picks the nearest the driver supports; we read back the result
 * so the caller knows the real rate/channels. */
int k93_snd_open(const char *pcmdev, int *rate, int *channels)
{
    if (snd_fd >= 0) return 0;
    int fd = open(pcmdev ? pcmdev : "/dev/snd/pcmC0D0p", O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;

    struct snd_pcm_hw_params hp;
    memset(&hp, 0, sizeof hp);
    memset(hp.masks, 0xff, sizeof hp.masks);      /* no constraint */
    /* an all-zero snd_interval means "must equal 0"; unconstrained is
     * [0, UINT_MAX] - without this every unset param refines to nothing */
    for (size_t i = 0; i < sizeof hp.intervals / sizeof hp.intervals[0]; i++)
        hp.intervals[i].max = ~0u;
    hp.rmask = ~0u;
    hp.info = ~0u;

    hw_set_mask(&hp, SNDRV_PCM_HW_PARAM_ACCESS,
                SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    hw_set_mask(&hp, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);

    struct snd_interval *iv;
    iv = hw_interval(&hp, SNDRV_PCM_HW_PARAM_CHANNELS);
    iv->min = iv->max = (channels && *channels > 0) ? (unsigned)*channels : 2;
    iv->integer = 1;

    iv = hw_interval(&hp, SNDRV_PCM_HW_PARAM_RATE);
    iv->min = iv->max = (rate && *rate > 0) ? (unsigned)*rate : 44100;
    iv->integer = 1;

    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) < 0) { close(fd); return -1; }

    if (rate)
        *rate = (int)hw_interval(&hp, SNDRV_PCM_HW_PARAM_RATE)->max;
    if (channels)
        *channels = (int)hw_interval(&hp, SNDRV_PCM_HW_PARAM_CHANNELS)->max;

    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) { close(fd); return -1; }
    snd_fd = fd;
    return 0;
}

int k93_snd_play(const void *s16le, size_t frames)
{
    if (snd_fd < 0) return -1;
    struct snd_xferi x;
    memset(&x, 0, sizeof x);
    x.buf = (void *)s16le;
    x.frames = frames;
    if (ioctl(snd_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) return -1;
    return 0;
}

void k93_snd_close(void)
{
    if (snd_fd >= 0) { close(snd_fd); snd_fd = -1; }
}
