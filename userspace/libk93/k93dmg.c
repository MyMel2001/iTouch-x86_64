/* k93dmg.c - UDIF/UDZO .dmg decoder, clean-room.
 * Structure knowledge (public, documented in libdmg-hfsplus, dmg2img and Apple
 * TN1159-style descriptions): 512-byte 'koly' trailer at EOF -> XML plist ->
 * blkx array -> per-region 'mish' block table -> 40-byte block descriptors. */
#define _GNU_SOURCE
#include "k93dmg.h"
#include "k93plist.h"
#include "k93util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <zlib.h>

#define KOLY_SIG 0x6b6f6c79u
#define MISH_SIG 0x6d697368u
#define SECTOR   512u

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

struct k93_dmg {
    const uint8_t *map;
    size_t maplen;
    int fd;
    uint64_t dataoff, datalen;           /* compressed data fork */
    uint64_t sectors;                    /* total output sectors */
    /* flattened block table */
    struct blk { uint32_t type; uint64_t sector, count, coff, clen; } *blks;
    int nblks, capblks;
    const uint8_t *fork;                 /* data fork base */
};

static int push_blocks(k93_dmg *d, const uint8_t *mish, size_t len, uint64_t first_sector)
{
    if (len < 204 || be32(mish) != MISH_SIG) return -1;
    uint64_t base = be64(mish + 24);                 /* dataOffset */
    for (size_t o = 204; o + 40 <= len; o += 40) {
        const uint8_t *b = mish + o;
        uint32_t type = be32(b);
        if (type == 0xffffffffu) break;
        uint64_t sec = be64(b + 8), cnt = be64(b + 16);
        uint64_t coff = be64(b + 24), clen = be64(b + 32);
        if (d->nblks == d->capblks) {
            d->capblks = d->capblks ? d->capblks * 2 : 64;
            d->blks = realloc(d->blks, sizeof(*d->blks) * (size_t)d->capblks);
            if (!d->blks) exit(202);
        }
        d->blks[d->nblks].type = type;
        d->blks[d->nblks].sector = sec + first_sector;
        d->blks[d->nblks].count = cnt;
        d->blks[d->nblks].coff = base + coff;
        d->blks[d->nblks].clen = clen;
        d->nblks++;
        (void)first_sector;
    }
    return 0;
}

static int b64decode(const char *in, size_t inlen, uint8_t **out, size_t *outlen)
{
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int val = 0, valb = -8;
    size_t cap = inlen * 3 / 4 + 4, n = 0;
    uint8_t *o = k93_xmalloc(cap);
    for (size_t i = 0; i < inlen; i++) {
        char c = in[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        const char *p = strchr(T, c);
        if (!p) { free(o); return -1; }
        val = (val << 6) | (int)(p - T);
        valb += 6;
        if (valb >= 0) {
            o[n++] = (uint8_t)((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    *out = o; *outlen = n;
    return 0;
}

k93_dmg *k93_dmg_open(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 1024) { close(fd); return NULL; }
    size_t len = (size_t)st.st_size;
    const uint8_t *map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { close(fd); return NULL; }

    const uint8_t *koly = map + len - 512;
    if (be32(koly) != KOLY_SIG) { munmap((void *)map, len); close(fd); return NULL; }

    k93_dmg *d = calloc(1, sizeof *d);
    d->fd = fd; d->map = map; d->maplen = len;
    d->dataoff = be64(koly + 24);
    d->datalen = be64(koly + 32);
    d->sectors = be64(koly + 372);
    uint64_t xmloff = be64(koly + 212), xmllen = be64(koly + 220);
    if (xmloff + xmllen > len) { k93_dmg_close(d); return NULL; }

    char *xml = k93_xmalloc((size_t)xmllen + 1);
    memcpy(xml, map + xmloff, (size_t)xmllen);
    xml[xmllen] = 0;
    char err[128];
    k93_plist *pl = k93_plist_parse(xml, (size_t)xmllen, err, sizeof err);
    free(xml);
    if (!pl) { k93_dmg_close(d); return NULL; }

    const k93_plist *rf = k93_plist_dict_get(pl, "resource-fork");
    const k93_plist *blkx = rf ? k93_plist_dict_get(rf, "blkx") : NULL;
    int nblkx = k93_plist_array_count(blkx);
    if (!nblkx) { k93_plist_free(pl); k93_dmg_close(d); return NULL; }

    d->fork = map + d->dataoff;
    for (int i = 0; i < nblkx; i++) {
        const k93_plist *e = k93_plist_array_at(blkx, i);
        const k93_plist *dat = k93_plist_dict_get(e, "Data");
        if (!dat || k93_plist_type_of(dat) != K93_PLIST_DATA) continue;
        size_t b64len = 0;
        const char *b64 = k93_plist_data(dat, &b64len);
        uint8_t *raw; size_t rawlen;
        if (!b64 || b64decode(b64, b64len, &raw, &rawlen) < 0) continue;
        /* FirstSectorNumber lives at mish+8 */
        uint64_t first = rawlen >= 16 ? be64(raw + 8) : 0;
        push_blocks(d, raw, rawlen, first);
        free(raw);
    }
    k93_plist_free(pl);
    if (!d->nblks) { k93_dmg_close(d); return NULL; }
    return d;
}

void k93_dmg_close(k93_dmg *d)
{
    if (!d) return;
    munmap((void *)d->map, d->maplen);
    close(d->fd);
    free(d->blks);
    free(d);
}

uint64_t k93_dmg_size(const k93_dmg *d) { return d ? d->sectors * SECTOR : 0; }

static int write_zeros(int fd, uint64_t n)
{
    static uint8_t z[SECTOR * 8];
    while (n) {
        size_t c = n > sizeof z ? sizeof z : (size_t)n;
        ssize_t w = write(fd, z, c);
        if (w <= 0) return -1;
        n -= (uint64_t)w;
    }
    return 0;
}

int k93_dmg_extract(k93_dmg *d, int outfd)
{
    if (!d) return -1;
    /* blocks arrive in region order; ensure monotonic output by seeking when
     * the table is not strictly ascending (mish tables normally are). */
    uint64_t cur = 0;
    for (int i = 0; i < d->nblks; i++) {
        struct blk *b = &d->blks[i];
        uint64_t out_off = b->sector * SECTOR;
        uint64_t bytes = b->count * SECTOR;
        if (out_off > cur) {
            /* sparse hole for gap */
            if (lseek(outfd, (off_t)out_off, SEEK_SET) < 0) return -1;
            cur = out_off;
        }
        switch (b->type) {
        case 0x00000000:                                    /* ZERO */
        case 0x00000002:                                    /* IGNORE */
            if (write_zeros(outfd, bytes) < 0) return -1;
            break;
        case 0x00000001:                                    /* COPY */
            if (lseek(outfd, (off_t)out_off, SEEK_SET) < 0) return -1;
            {
                const uint8_t *p = d->fork + b->coff;
                uint64_t left = b->clen < bytes ? b->clen : bytes;
                while (left) {
                    size_t c = left > (1u << 20) ? (1u << 20) : (size_t)left;
                    ssize_t w = write(outfd, p, c);
                    if (w <= 0) return -1;
                    p += w; left -= (uint64_t)w;
                }
            }
            break;
        case 0x80000005: {                                 /* UDZO/zlib */
            if (lseek(outfd, (off_t)out_off, SEEK_SET) < 0) return -1;
            z_stream s;
            memset(&s, 0, sizeof s);
            int rinit = inflateInit(&s);
            if (rinit != Z_OK) {
                memset(&s, 0, sizeof s);
                if (inflateInit2(&s, -15) != Z_OK) return -1;
            }
            s.next_in = (Bytef *)(d->fork + b->coff);
            s.avail_in = (uInt)b->clen;
            uint8_t *out = k93_xmalloc(1 << 20);
            uint64_t produced = 0;
            int rc = 0;
            do {
                s.next_out = out;
                s.avail_out = 1 << 20;
                int r = inflate(&s, Z_NO_FLUSH);
                size_t have = (1 << 20) - s.avail_out;
                size_t off = 0;
                while (off < have) {
                    ssize_t w = write(outfd, out + off, have - off);
                    if (w <= 0) { rc = -1; break; }
                    off += (size_t)w;
                }
                produced += have;
                if (rc) break;
                if (r == Z_STREAM_END) break;
                if (r != Z_OK) { rc = -2; break; }
            } while (s.avail_in || s.avail_out == 0);
            inflateEnd(&s);
            free(out);
            if (rc) return -1;
            if (produced < bytes && write_zeros(outfd, bytes - produced) < 0) return -1;
            break;
        }
        default:
            return -3;                                     /* unsupported codec */
        }
        cur = out_off + bytes;
    }
    /* ensure final size (best effort: some sinks are pipes) */
    (void)ftruncate(outfd, (off_t)(d->sectors * SECTOR));
    return 0;
}

int k93_dmg_probe(const char *path)
{
    k93_dmg *d = k93_dmg_open(path);
    if (!d) return 0;
    k93_dmg_close(d);
    return 1;
}
