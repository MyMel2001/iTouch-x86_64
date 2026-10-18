/* Camera.app - K93OS camera (clean-room).
 * Real V4L2 capture app: preview, digital zoom, hardware auto-focus toggle
 * when the sensor exposes one, multiple cameras (switch at runtime), stills
 * (PPM into /var/mobile/Media/Photos, which Photos.app renders), and video
 * recording to AVI (uncompressed YUY2 video + ALSA PCM audio, muxed in a
 * hand-written RIFF container - no encoder needed because V4L2 delivers
 * raw frames).
 *
 * No capture hardware (the QEMU test VM): a clearly-labeled synthetic test
 * pattern stands in for the sensor so the whole UI/recording path stays
 * exercisable; photo + video writes work the same in both modes.
 *
 * Ceilings: YUYV-only sensors (MJPEG-only modes are rejected with a message
 * - shipping a JPEG decoder is the upgrade path), digital zoom only (hardware
 * zoom controls not driven), capture via the kernel UAPI (no libv4l2). */
#define _GNU_SOURCE
#include "k93ui.h"
#include "k93util.h"
#include "k93snd.h"
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <poll.h>

#define MAXDEV   8
#define MAXBUF   4
#define PAT_W    640
#define PAT_H    480
#define PAT_FPS  15
#define ZOOM_MAX 8
#define PHOTOS   "/var/mobile/Media/Photos"

struct camdev { char path[32]; char card[64]; };

static struct {
    struct camdev dev[MAXDEV];
    int ndev;                          /* capture-capable devices found */
    int cur;                           /* device index, -1 = test pattern */
    int fd;
    void *map[MAXBUF];
    size_t maplen[MAXBUF];
    int nbuf;
    unsigned sw, sh;                   /* active source size (negotiated or
                                          pattern when no camera) */
    int streaming;
    unsigned char *frame;              /* last YUYV frame (sw*sh*2) */
    unsigned char *pat;                /* test-pattern YUYV (PAT_W*PAT_H*2) */
    int zoom;                          /* 1,2,4,8 digital */
    int rec;
    int af_supported, af_on;
    char lastmsg[96];                  /* transient status line */
    long lastmsg_until;                /* now_ms() deadline */
    int have_audio, arate, achan;
    uint32_t *prev;                    /* preview RGBA buffer */
    int pw, ph;
} K = { .fd = -1, .cur = -1, .zoom = 1 };

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void say(const char *msg)
{
    snprintf(K.lastmsg, sizeof K.lastmsg, "%.90s", msg);
    K.lastmsg_until = now_ms() + 2500;
}

/* ------------------------------------------------------------- AVI writer */
/* Minimal RIFF AVI: one YUY2 video stream + optional PCM audio stream,
 * idx1 index kept in memory and flushed on close. */

#define AVI_FPS_EST 30                 /* replaced by measured rate */

struct aviw {
    FILE *f;
    int w, h;
    int has_audio, arate, achan;
    unsigned frames;
    long long audio_frames;            /* audio frames written */
    long t0;
    long p_riffsz, p_avih_us, p_avih_frames, p_vidlen;
    long p_audlen, p_movisz, movi_fourcc;
    unsigned char *idx;
    size_t idx_len, idx_cap;
    int failed;
};

static void wf32(FILE *f, uint32_t v)
{
    unsigned char b[4] = { v, v >> 8, v >> 16, v >> 24 };
    fwrite(b, 1, 4, f);
}
static void wf16(FILE *f, uint16_t v)
{
    unsigned char b[2] = { v, v >> 8 };
    fwrite(b, 1, 2, f);
}
static void wfcc(FILE *f, const char *cc) { fwrite(cc, 1, 4, f); }

static void avi_idx_add(struct aviw *a, const char *fcc, uint32_t off,
                        uint32_t len)
{
    if (a->idx_len + 16 > a->idx_cap) {
        size_t cap = a->idx_cap ? a->idx_cap * 2 : 65536;
        unsigned char *p = realloc(a->idx, cap);
        if (!p) { a->failed = 1; return; }
        a->idx = p; a->idx_cap = cap;
    }
    unsigned char *e = a->idx + a->idx_len;
    memcpy(e, fcc, 4);
    e[4] = 0x10; e[5] = 0; e[6] = 0; e[7] = 0;      /* AVIF_KEYFRAME */
    e[8] = off; e[9] = off >> 8; e[10] = off >> 16; e[11] = off >> 24;
    e[12] = len; e[13] = len >> 8; e[14] = len >> 16; e[15] = len >> 24;
    a->idx_len += 16;
}

static int avi_open(struct aviw *a, const char *path, int w, int h,
                    int has_audio, int arate, int achan)
{
    memset(a, 0, sizeof *a);
    a->w = w; a->h = h;
    a->has_audio = has_audio; a->arate = arate; a->achan = achan;
    a->t0 = now_ms();
    a->f = fopen(path, "w+b");
    if (!a->f) return -1;

    int streams = has_audio ? 2 : 1;
    long bytes_per_frame = (long)w * h * 2;
    long audio_block = has_audio ? (long)achan * 2 : 0;

    wfcc(a->f, "RIFF");
    a->p_riffsz = ftell(a->f);
    wf32(a->f, 0);                                  /* patched on close */
    wfcc(a->f, "AVI ");

    /* hdrl: "hdrl" + avih(8+56) + video strl LIST + optional audio strl */
    wfcc(a->f, "LIST");
    wf32(a->f, 4 + 8 + 56 + 8 + 4 + 8 + 56 + 8 + 40 +
               (has_audio ? (8 + 4 + 8 + 56 + 8 + 18) : 0));
    wfcc(a->f, "hdrl");

    wfcc(a->f, "avih");
    wf32(a->f, 56);
    a->p_avih_us = ftell(a->f);
    wf32(a->f, 1000000 / AVI_FPS_EST);              /* us/frame (patch) */
    wf32(a->f, (uint32_t)(bytes_per_frame * AVI_FPS_EST +
                          (has_audio ? (long)arate * audio_block : 0)));
    wf32(a->f, 0);
    wf32(a->f, 0x11);                               /* HASINDEX|INTERLEAVED */
    a->p_avih_frames = ftell(a->f);
    wf32(a->f, 0);                                  /* total frames (patch) */
    wf32(a->f, 0);                                  /* initial frames */
    wf32(a->f, (uint32_t)streams);
    wf32(a->f, (uint32_t)bytes_per_frame);
    wf32(a->f, (uint32_t)w);
    wf32(a->f, (uint32_t)h);
    wf32(a->f, 0); wf32(a->f, 0); wf32(a->f, 0); wf32(a->f, 0);

    /* video strl (AVISTREAMHEADER = 56 bytes incl. dwInitialFrames) */
    wfcc(a->f, "LIST");
    wf32(a->f, 4 + 8 + 56 + 8 + 40);
    wfcc(a->f, "strl");
    wfcc(a->f, "strh");
    wf32(a->f, 56);
    wfcc(a->f, "vids");
    wfcc(a->f, "YUY2");
    wf32(a->f, 0);                                  /* flags */
    wf16(a->f, 0); wf16(a->f, 0);                   /* priority, language */
    wf32(a->f, 0);                                  /* initial frames */
    wf32(a->f, 1);                                  /* scale */
    wf32(a->f, AVI_FPS_EST);                        /* rate (patch) */
    wf32(a->f, 0);                                  /* start */
    a->p_vidlen = ftell(a->f);
    wf32(a->f, 0);                                  /* length (patch) */
    wf32(a->f, (uint32_t)bytes_per_frame);
    wf32(a->f, 0xFFFFFFFF);                         /* quality (-1) */
    wf32(a->f, 0);                                  /* sample size */
    wf16(a->f, 0); wf16(a->f, 0);
    wf16(a->f, (uint16_t)w); wf16(a->f, (uint16_t)h);
    wfcc(a->f, "strf");
    wf32(a->f, 40);                                 /* BITMAPINFOHEADER */
    wf32(a->f, 40);
    wf32(a->f, (uint32_t)w);
    wf32(a->f, (uint32_t)h);
    wf16(a->f, 1);                                  /* planes */
    wf16(a->f, 16);                                 /* bpp */
    wfcc(a->f, "YUY2");
    wf32(a->f, (uint32_t)(w * h * 2));
    wf32(a->f, 0); wf32(a->f, 0); wf32(a->f, 0); wf32(a->f, 0);

    if (has_audio) {
        wfcc(a->f, "LIST");
        wf32(a->f, 4 + 8 + 56 + 8 + 18);
        wfcc(a->f, "strl");
        wfcc(a->f, "strh");
        wf32(a->f, 56);
        wfcc(a->f, "auds");
        wf32(a->f, 0);                              /* handler */
        wf32(a->f, 0);                              /* flags */
        wf16(a->f, 0); wf16(a->f, 0);
        wf32(a->f, 0);                              /* initial frames */
        wf32(a->f, (uint32_t)(achan * 2));          /* scale = block align */
        wf32(a->f, (uint32_t)(arate * achan * 2));  /* rate = byte rate */
        wf32(a->f, 0);                              /* start */
        a->p_audlen = ftell(a->f);
        wf32(a->f, 0);                              /* length (patch) */
        wf32(a->f, (uint32_t)(arate * achan * 2 * 4));
        wf32(a->f, 0xFFFFFFFF);                     /* quality (-1) */
        wf32(a->f, (uint32_t)(achan * 2));          /* sample size */
        wf16(a->f, 0); wf16(a->f, 0); wf16(a->f, 0); wf16(a->f, 0);
        wfcc(a->f, "strf");
        wf32(a->f, 18);
        wf16(a->f, 1);                              /* PCM */
        wf16(a->f, (uint16_t)achan);
        wf32(a->f, (uint32_t)arate);
        wf32(a->f, (uint32_t)(arate * achan * 2));
        wf16(a->f, (uint16_t)(achan * 2));
        wf16(a->f, 16);
        wf16(a->f, 0);                              /* cbSize */
    }

    wfcc(a->f, "LIST");
    a->p_movisz = ftell(a->f);
    wf32(a->f, 0);                                  /* patched on close */
    a->movi_fourcc = ftell(a->f);
    wfcc(a->f, "movi");
    return 0;
}

static void avi_chunk(struct aviw *a, const char *fcc, const void *data,
                      uint32_t len)
{
    uint32_t off = (uint32_t)(ftell(a->f) - a->movi_fourcc);
    wfcc(a->f, fcc);
    wf32(a->f, len);
    fwrite(data, 1, len, a->f);
    if (len & 1) fputc(0, a->f);
    avi_idx_add(a, fcc, off, len);
}

static void avi_close(struct aviw *a)
{
    if (!a->f) return;
    long idx_start = ftell(a->f);
    if (a->idx_len) {
        wfcc(a->f, "idx1");
        wf32(a->f, (uint32_t)a->idx_len);
        fwrite(a->idx, 1, a->idx_len, a->f);
    }
    long end = ftell(a->f);
    long elapsed = now_ms() - a->t0;
    if (elapsed < 1) elapsed = 1;

    uint32_t fps = (uint32_t)((long long)a->frames * 1000 / elapsed);
    if (fps < 1) fps = 1;

    fseek(a->f, a->p_riffsz, SEEK_SET);
    wf32(a->f, (uint32_t)(end - 8));
    fseek(a->f, a->p_avih_frames, SEEK_SET);
    wf32(a->f, a->frames);
    fseek(a->f, a->p_avih_us, SEEK_SET);
    wf32(a->f, (uint32_t)((elapsed * 1000LL) /
                          (a->frames ? a->frames : 1)));
    fseek(a->f, a->p_vidlen, SEEK_SET);
    wf32(a->f, a->frames);
    fseek(a->f, a->p_vidlen - 8, SEEK_SET);         /* dwRate */
    wf32(a->f, fps);
    if (a->has_audio && a->p_audlen) {
        fseek(a->f, a->p_audlen, SEEK_SET);
        wf32(a->f, (uint32_t)a->audio_frames);
    }
    fseek(a->f, a->p_movisz, SEEK_SET);
    wf32(a->f, (uint32_t)(idx_start - (a->p_movisz + 4)));
    fseek(a->f, end, SEEK_SET);

    fclose(a->f);
    a->f = NULL;
    free(a->idx);
    a->idx = NULL;
}

/* --------------------------------------------------------- V4L2 capture */

static void dev_scan(void)
{
    K.ndev = 0;
    for (int i = 0; i < MAXDEV; i++) {
        struct v4l2_capability cap;
        memset(&cap, 0, sizeof cap);
        snprintf(K.dev[K.ndev].path, sizeof K.dev[0].path, "/dev/video%d", i);
        int fd = open(K.dev[K.ndev].path, O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        int ok = ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0;
        unsigned caps = cap.capabilities;
        if (ok && (caps & V4L2_CAP_DEVICE_CAPS)) caps = cap.device_caps;
        if (ok && (caps & V4L2_CAP_VIDEO_CAPTURE) &&
            (caps & V4L2_CAP_STREAMING)) {
            snprintf(K.dev[K.ndev].card, sizeof K.dev[0].card, "%.63s",
                     cap.card);
            K.ndev++;
        }
        close(fd);
    }
}

/* close the sensor and restore pattern dimensions as the active source */
static void cam_close(void)
{
    if (K.streaming) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(K.fd, VIDIOC_STREAMOFF, &type);
        K.streaming = 0;
    }
    for (int i = 0; i < K.nbuf; i++)
        if (K.map[i] && K.map[i] != MAP_FAILED)
            munmap(K.map[i], K.maplen[i]);
    K.nbuf = 0;
    if (K.fd >= 0) { close(K.fd); K.fd = -1; }
    K.sw = PAT_W;
    K.sh = PAT_H;
}

/* open device K.cur; 0 = streaming, -1 = fall back to the test pattern */
static int cam_open(void)
{
    cam_close();
    if (K.cur < 0 || K.cur >= K.ndev) { K.cur = -1; return -1; }
    K.fd = open(K.dev[K.cur].path, O_RDWR | O_NONBLOCK);
    if (K.fd < 0) { K.cur = -1; return -1; }

    struct v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = 640;
    f.fmt.pix.height = 480;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    f.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(K.fd, VIDIOC_TRY_FMT, &f) < 0 ||
        f.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        say("sensor has no YUYV mode (MJPEG-only?)");
        cam_close(); K.cur = -1; return -1;
    }
    if (ioctl(K.fd, VIDIOC_S_FMT, &f) < 0) {
        cam_close(); K.cur = -1; return -1;
    }
    K.sw = f.fmt.pix.width;
    K.sh = f.fmt.pix.height;

    size_t need = (size_t)K.sw * K.sh * 2;
    unsigned char *nf = realloc(K.frame, need);
    if (!nf) { cam_close(); K.cur = -1; return -1; }
    K.frame = nf;
    memset(K.frame, 0x80, need);

    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof rb);
    rb.count = MAXBUF;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(K.fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 1) {
        say("mmap streaming unsupported");
        cam_close(); K.cur = -1; return -1;
    }
    K.nbuf = (int)rb.count;
    for (int i = 0; i < K.nbuf; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = (unsigned)i;
        if (ioctl(K.fd, VIDIOC_QUERYBUF, &b) < 0) {
            cam_close(); K.cur = -1; return -1;
        }
        K.maplen[i] = b.length;
        K.map[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                        K.fd, b.m.offset);
        if (K.map[i] == MAP_FAILED) { cam_close(); K.cur = -1; return -1; }
        ioctl(K.fd, VIDIOC_QBUF, &b);
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(K.fd, VIDIOC_STREAMON, &type) < 0) {
        say("STREAMON failed");
        cam_close(); K.cur = -1; return -1;
    }
    K.streaming = 1;

    /* hardware auto-focus switch, when the sensor exposes one */
    K.af_supported = 0; K.af_on = 0;
    struct v4l2_queryctrl q;
    memset(&q, 0, sizeof q);
    q.id = V4L2_CID_FOCUS_AUTO;
    if (ioctl(K.fd, VIDIOC_QUERYCTRL, &q) == 0 &&
        !(q.flags & V4L2_CTRL_FLAG_DISABLED)) {
        K.af_supported = 1;
        struct v4l2_control c;
        memset(&c, 0, sizeof c);
        c.id = V4L2_CID_FOCUS_AUTO;
        if (ioctl(K.fd, VIDIOC_G_CTRL, &c) == 0) K.af_on = c.value ? 1 : 0;
    }
    return 0;
}

/* poll one raw frame into K.frame; 1 = new frame, 0 = none */
static int cam_frame(int timeout_ms)
{
    if (!K.streaming) return 0;
    struct pollfd p = { .fd = K.fd, .events = POLLIN };
    if (poll(&p, 1, timeout_ms) <= 0) return 0;
    struct v4l2_buffer b;
    memset(&b, 0, sizeof b);
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    if (ioctl(K.fd, VIDIOC_DQBUF, &b) < 0) return 0;
    size_t need = (size_t)K.sw * K.sh * 2;
    if (b.bytesused >= need) {
        memcpy(K.frame, K.map[b.index], need);
    } else {
        memcpy(K.frame, K.map[b.index], b.bytesused);
        memset(K.frame + b.bytesused, 0x80, need - b.bytesused);
    }
    ioctl(K.fd, VIDIOC_QBUF, &b);
    return 1;
}

/* --------------------------------------------------------- test pattern */
/* BT.601 color bars + a sweeping highlight, raw YUYV, clearly synthetic. */
static void pattern_gen(long ms)
{
    static const unsigned char bars[8][3] = {      /* Y U V */
        {235,128,128}, {210,16,146}, {170,166,16}, {145,54,34},
        {106,202,222}, {81,90,240},  {41,240,110}, {16,128,128},
    };
    int w = PAT_W, h = PAT_H;
    int sweep = (int)((ms / 30) % (long)h);
    for (int y = 0; y < h; y++) {
        unsigned char *row = K.pat + (size_t)y * w * 2;
        int hot = y >= sweep && y < sweep + 20;
        for (int x = 0; x < w; x += 2) {
            int bi = x * 8 / w;
            if (bi > 7) bi = 7;
            unsigned char Y = hot ? 235 : bars[bi][0];
            row[x * 2] = Y;
            row[x * 2 + 1] = bars[bi][1];
            row[x * 2 + 2] = Y;
            row[x * 2 + 3] = bars[bi][2];
        }
    }
}

/* --------------------------------------------------- YUYV -> RGBA blit */
static uint32_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint32_t)v); }

/* crop the centre 1/zoom of src, nearest-neighbour scale to pw x ph */
static void yuyv_to_rgba(const unsigned char *s, int sw, int sh,
                         uint32_t *out, int pw, int ph, int zoom)
{
    int cw = sw / zoom, ch = sh / zoom;
    int sx0 = (sw - cw) / 2, sy0 = (sh - ch) / 2;
    for (int oy = 0; oy < ph; oy++) {
        int sy = sy0 + (int)((long)oy * ch / ph);
        const unsigned char *row = s + (size_t)sy * sw * 2;
        uint32_t *dst = out + (size_t)oy * pw;
        for (int ox = 0; ox < pw; ox++) {
            int sx = sx0 + (int)((long)ox * cw / pw);
            int pair = sx & ~1;
            int Y = row[sx * 2];
            int U = row[pair * 2 + 1] - 128;
            int V = row[pair * 2 + 3] - 128;
            int c = Y - 16;
            if (c < 0) c = 0;
            int r = (1192 * c + 1634 * V + 512) >> 10;
            int g = (1192 * c - 831 * U - 401 * V + 512) >> 10;
            int b = (1192 * c + 2066 * U + 512) >> 10;
            dst[ox] = 0xFF000000u | (clamp8(r) << 16) |
                      (clamp8(g) << 8) | clamp8(b);
        }
    }
}

/* ------------------------------------------------------------- stills */

static void snapshot(void)
{
    if (!K.prev || K.pw <= 0) { say("nothing to capture yet"); return; }
    char path[256];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    for (int i = 0; i < 10000; i++) {
        if (i == 0)
            snprintf(path, sizeof path,
                     PHOTOS "/K93CAM-%04d%02d%02d-%02d%02d%02d.ppm",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
        else
            snprintf(path, sizeof path,
                     PHOTOS "/K93CAM-%04d%02d%02d-%02d%02d%02d-%d.ppm",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec, i);
        if (access(path, F_OK) != 0) break;
    }
    mkdir(PHOTOS, 0755);
    FILE *f = fopen(path, "wb");
    if (!f) { say("photo: cannot write"); return; }
    fprintf(f, "P6\n%d %d\n255\n", K.pw, K.ph);
    unsigned char *row = malloc((size_t)K.pw * 3);
    if (row) {
        for (int y = 0; y < K.ph; y++) {
            const uint32_t *src = K.prev + (size_t)y * K.pw;
            for (int x = 0; x < K.pw; x++) {
                row[x * 3] = (unsigned char)(src[x] >> 16);
                row[x * 3 + 1] = (unsigned char)(src[x] >> 8);
                row[x * 3 + 2] = (unsigned char)src[x];
            }
            fwrite(row, 1, (size_t)K.pw * 3, f);
        }
        free(row);
    }
    fclose(f);
    char m[96];
    snprintf(m, sizeof m, "saved %s", strrchr(path, '/') + 1);
    say(m);
}

/* -------------------------------------------------------------- record */

static struct aviw V;
static char vpath[256];

static void rec_toggle(void)
{
    if (K.rec) {
        avi_close(&V);
        K.rec = 0;
        struct stat st;
        char m[96];
        if (stat(vpath, &st) == 0)
            snprintf(m, sizeof m, "saved %s (%ld KB)",
                     strrchr(vpath, '/') + 1, (long)(st.st_size / 1024));
        else
            snprintf(m, sizeof m, "saved recording");
        say(m);
        return;
    }
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(vpath, sizeof vpath,
             PHOTOS "/K93REC-%04d%02d%02d-%02d%02d%02d.avi",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    mkdir(PHOTOS, 0755);
    if (avi_open(&V, vpath, (int)K.sw, (int)K.sh,
                 K.have_audio, K.arate, K.achan) < 0) {
        say("cannot create video file");
        return;
    }
    K.rec = 1;
    say("recording started");
}

static unsigned char *abuf;                    /* audio scratch */
#define ABUF_FRAMES 8192

/* append current frame + its audio slice to the AVI */
static void rec_add(const unsigned char *yuyv)
{
    if (!K.rec || V.failed || !V.f) return;
    long elapsed = now_ms() - V.t0;
    /* audio: catch up the clock (drift-free), pad with silence if short */
    if (K.have_audio && K.achan > 0) {
        long long awant = ((long long)K.arate * elapsed) / 1000;
        long long want = awant - V.audio_frames;
        if (want > 0) {
            size_t take = want > ABUF_FRAMES ? ABUF_FRAMES : (size_t)want;
            size_t got = 0;
            while (got < take) {
                ssize_t r = k93_snd_cap_read(abuf + got * 2 * (size_t)K.achan,
                                             take - got);
                if (r <= 0) break;
                got += (size_t)r;
            }
            if (got < take)                       /* pad */
                memset(abuf + got * 2 * (size_t)K.achan, 0,
                       (take - got) * 2 * (size_t)K.achan);
            avi_chunk(&V, "01wb", abuf,
                      (uint32_t)(take * 2 * (size_t)K.achan));
            V.audio_frames += (long long)take;
        }
    }
    avi_chunk(&V, "00dc", yuyv, (uint32_t)K.sw * K.sh * 2);
    V.frames++;
}

/* ------------------------------------------------------------------ UI */

static void draw(int new_frame, long now)
{
    int W = ui_px_width(), H = ui_px_height();
    int rx = 0, ry = 20, rw = W, rh = H - 20 - 24;
    if (rw < 64 || rh < 64) return;

    const unsigned char *src;
    if (K.streaming) {
        src = K.frame;
    } else {
        pattern_gen(now);
        src = K.pat;
    }

    /* aspect-fit the 4:3 source into the rect */
    int pw = rw, ph = (int)((long)rw * K.sh / (K.sw ? K.sw : 1));
    if (ph > rh) {
        ph = rh;
        pw = (int)((long)rh * K.sw / (K.sh ? K.sh : 1));
    }
    if (pw > rw) pw = rw;
    if (ph > rh) ph = rh;
    if (pw < 2 || ph < 2) return;
    if (!K.prev || pw != K.pw || ph != K.ph) {
        free(K.prev);
        K.prev = malloc((size_t)pw * ph * 4);
        if (!K.prev) return;
        K.pw = pw; K.ph = ph;
        new_frame = 1;
    }
    if (new_frame || !K.streaming)
        yuyv_to_rgba(src, (int)K.sw, (int)K.sh, K.prev, pw, ph, K.zoom);

    ui_fill(rx, ry, rw, rh, ui_rgb(0x10, 0x14, 0x1C));   /* letterbox */
    int ox = rx + (rw - pw) / 2, oy = ry + (rh - ph) / 2;
    ui_blit(ox, oy, pw, ph, K.prev);

    char title[80];
    if (K.streaming)
        snprintf(title, sizeof title, "Camera - %s",
                 K.dev[K.cur].card[0] ? K.dev[K.cur].card : K.dev[K.cur].path);
    else
        snprintf(title, sizeof title, "Camera - TEST PATTERN (no camera)");
    ui_statusbar(title);

    char hint[176];
    snprintf(hint, sizeof hint,
             "%s  +/-:zoom  F:AF(%s)  C:cam  SPACE:photo  %s  Q:quit",
             K.streaming ? K.dev[K.cur].path : "pattern",
             K.streaming ? (K.af_supported ? (K.af_on ? "auto" : "off")
                                           : "fixed") : "n/a",
             K.rec ? "R:stop" : "R:rec");
    ui_statusmsg(hint);

    /* overlays on top of the preview */
    if (K.zoom > 1) {
        char badge[16];
        snprintf(badge, sizeof badge, "x%d", K.zoom);
        int bw = ui_text_width(1, badge) + 12;
        ui_roundrect(ox + 8, oy + 8, bw, 22, 6,
                     ui_rgb(0x00, 0x00, 0x00), ui_rgb(0x00, 0x00, 0x00));
        ui_text_bold(ox + 14, oy + 11, 1, badge, ui_rgb(0xFF, 0xE0, 0x40), 0);
    }
    if (K.rec) {
        ui_fill(ox + pw - 70, oy + 8, 62, 22, ui_rgb(0xC0, 0x18, 0x18));
        ui_text_bold(ox + pw - 64, oy + 11, 1, "REC",
                     ui_rgb(0xFF, 0xFF, 0xFF), 0x400000);
    }
    if (K.lastmsg[0] && now < K.lastmsg_until) {
        int mw = ui_text_width(1, K.lastmsg) + 16;
        int mx = ox + (pw - mw) / 2;
        if (mx < 4) mx = 4;
        ui_roundrect(mx, oy + ph - 34, mw, 24, 6,
                     ui_rgb(0x10, 0x18, 0x30), ui_rgb(0x10, 0x18, 0x30));
        ui_text_sh(mx + 8, oy + ph - 30, 1, K.lastmsg,
                   ui_rgb(0xFF, 0xFF, 0xFF), 0x000000);
    } else if (now >= K.lastmsg_until) {
        K.lastmsg[0] = 0;
    }
    ui_flush();
}

static void set_af(int on)
{
    if (!K.af_supported) { say("fixed-focus lens (no AF control)"); return; }
    struct v4l2_control c;
    memset(&c, 0, sizeof c);
    c.id = V4L2_CID_FOCUS_AUTO;
    c.value = on ? 1 : 0;
    if (ioctl(K.fd, VIDIOC_S_CTRL, &c) < 0) { say("AF control rejected"); return; }
    K.af_on = on;
    say(on ? "auto-focus on" : "auto-focus off");
}

/* cameras ... -> pattern -> first camera */
static void cam_cycle(void)
{
    cam_close();
    if (K.ndev == 0) { K.cur = -1; return; }
    K.cur = (K.cur + 1 >= K.ndev) ? -1 : K.cur + 1;
    if (K.cur >= 0) cam_open();          /* sets K.cur=-1 on failure */
}

int main(void)
{
    if (ui_init("Camera") < 0) return 1;

    dev_scan();
    if (!ui_is_graphical()) {
        char body[768];
        size_t o = (size_t)snprintf(body, sizeof body, "Camera devices:\n\n");
        if (K.ndev == 0)
            o += (size_t)snprintf(body + o, sizeof body - o,
                                  "  none (no V4L2 capture hardware)\n");
        for (int i = 0; i < K.ndev && o < sizeof body; i++)
            o += (size_t)snprintf(body + o, sizeof body - o,
                                  "  %s  (%s)\n", K.dev[i].path, K.dev[i].card);
        snprintf(body + o, sizeof body - o,
                 "\nPreview/record needs the framebuffer backend.");
        ui_msgbox("Camera", body);
        ui_shutdown();
        return 0;
    }

    K.sw = PAT_W;
    K.sh = PAT_H;
    K.frame = malloc((size_t)PAT_W * PAT_H * 2);
    K.pat = malloc((size_t)PAT_W * PAT_H * 2);
    abuf = malloc((size_t)ABUF_FRAMES * 2 * 4);
    if (!K.frame || !K.pat || !abuf) { ui_shutdown(); return 1; }
    memset(K.frame, 0x80, (size_t)PAT_W * PAT_H * 2);

    K.cur = K.ndev > 0 ? 0 : -1;
    if (K.cur >= 0 && cam_open() < 0) K.cur = -1;

    K.have_audio = k93_snd_cap_open(NULL, &K.arate, &K.achan) == 0;

    int new_frame = 1;
    long next_pat = 0;
    for (;;) {
        long now = now_ms();

        int k = ui_getkey(0);
        if (k > 0) {
            if (k == UI_KEY_ESC || k == UI_KEY_CTRL_C ||
                k == UI_KEY_CHAR_BASE + 'q') {
                if (K.rec) rec_toggle();
                break;
            }
            if (k == UI_KEY_CHAR_BASE + '+' || k == UI_KEY_CHAR_BASE + '=') {
                if (K.zoom < ZOOM_MAX) K.zoom *= 2;
            } else if (k == UI_KEY_CHAR_BASE + '-') {
                if (K.zoom > 1) K.zoom /= 2;
            } else if (k == UI_KEY_CHAR_BASE + 'f') {
                set_af(!K.af_on);
            } else if (k == UI_KEY_CHAR_BASE + 'c') {
                if (K.rec) say("stop recording before switching camera");
                else { cam_cycle(); new_frame = 1; }
            } else if (k == UI_KEY_CHAR_BASE + 'r') {
                rec_toggle();
            } else if (k == UI_KEY_ENTER || k == UI_KEY_CHAR_BASE + ' ') {
                snapshot();
            }
        }

        /* pacing: camera polls the sensor, pattern ticks at 15 fps */
        if (K.streaming) {
            if (cam_frame(0)) new_frame = 1;
            else if (cam_frame(25)) new_frame = 1;
        } else {
            if (now >= next_pat) {
                new_frame = 1;
                next_pat = now + 1000 / PAT_FPS;
            }
        }

        draw(new_frame, now);
        new_frame = 0;

        if (K.rec)
            rec_add(K.streaming ? K.frame : K.pat);
        if (V.failed) { rec_toggle(); say("disk write failed - stopped"); }

        if (!K.streaming && now < next_pat)
            usleep((useconds_t)(next_pat - now) * 1000);
    }

    if (K.rec) rec_toggle();
    cam_close();
    k93_snd_cap_close();
    free(K.frame); free(K.pat); free(K.prev); free(abuf);
    ui_shutdown();
    return 0;
}
