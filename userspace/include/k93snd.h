/* k93snd.h - minimal clean-room ALSA PCM playback (kernel UAPI, no libasound) */
#ifndef K93SND_H
#define K93SND_H
#include <stddef.h>
#include <sys/types.h>                    /* ssize_t for capture reads */

/* Open a playback PCM node (NULL = /dev/snd/pcmC0D0p) and negotiate
 * S16_LE interleaved stereo. The optional rate/channels outputs are read
 * back with the real negotiated values. Returns 0 on success, -1 if there
 * is no usable sound device. */
int  k93_snd_open(const char *pcmdev, int *rate, int *channels);
/* Write S16_LE interleaved frames. Returns 0, or -1 on error/underrun. */
int  k93_snd_play(const void *s16le, size_t frames);
void k93_snd_close(void);

/* Capture (Camera.app): same negotiation on /dev/snd/pcmC*D*c. The fd is
 * nonblocking; k93_snd_cap_read returns 0 when no frames are ready yet. */
int    k93_snd_cap_open(const char *pcmdev, int *rate, int *channels);
ssize_t k93_snd_cap_read(void *s16le, size_t frames);
void   k93_snd_cap_close(void);

#endif
