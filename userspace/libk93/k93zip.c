/* k93zip.c - minimal ZIP reader for .ipa sideloading, clean-room.
 * Observation basis: the .ipa files are ordinary ZIP archives whose members are
 * Payload/<Name>.app/... ; format implemented from the public APPNOTE spec. */
#define _GNU_SOURCE
#include "k93zip.h"
#include "k93util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <zlib.h>

struct k93_zip {
    const uint8_t *map;
    size_t maplen;
    int fd;
    int count;
    uint64_t *lho;                       /* local header offsets */
    uint64_t *csize, *usize;
    uint16_t *method;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

k93_zip *k93_zip_open(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 22) { close(fd); return NULL; }
    size_t len = (size_t)st.st_size;
    const uint8_t *map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { close(fd); return NULL; }

    /* locate EOCD (scan backwards over comment area) */
    size_t scan = len < 70000 ? len : 70000;
    const uint8_t *eocd = NULL;
    for (size_t i = len - 22 + 1; i > len - scan; i--) {
        const uint8_t *p = map + i - 1;
        if (rd32(p) == 0x06054b50) { eocd = p; break; }
        if (i <= len - scan) break;
    }
    if (!eocd) { munmap((void *)map, len); close(fd); return NULL; }

    uint64_t count = rd16(eocd + 10);
    uint64_t cdsize = rd32(eocd + 12);
    uint64_t cdoff  = rd32(eocd + 16);

    /* ZIP64? */
    if (count == 0xFFFF || cdoff == 0xFFFFFFFFu || cdsize == 0xFFFFFFFFu) {
        /* look for zip64 locator just before eocd */
        if ((size_t)(eocd - map) >= 20) {
            const uint8_t *loc = eocd - 20;
            if (rd32(loc) == 0x07064b50) {
                uint64_t zoff = rd64(loc + 8);
                if (zoff + 56 <= len && rd32(map + zoff) == 0x06064b50) {
                    const uint8_t *z = map + zoff;
                    count = rd64(z + 32);
                    cdsize = rd64(z + 40);
                    cdoff = rd64(z + 48);
                }
            }
        }
    }
    if (cdoff + cdsize > len || count > 200000) { munmap((void *)map, len); close(fd); return NULL; }

    k93_zip *z = calloc(1, sizeof *z);
    z->fd = fd; z->map = map; z->maplen = len; z->count = (int)count;
    z->lho = calloc((size_t)count, 8);
    z->csize = calloc((size_t)count, 8);
    z->usize = calloc((size_t)count, 8);
    z->method = calloc((size_t)count, 2);

    const uint8_t *p = map + cdoff, *end = map + cdoff + cdsize;
    int i = 0;
    while (p + 46 <= end && i < (int)count) {
        if (rd32(p) != 0x02014b50) break;
        uint16_t m = rd16(p + 10);
        uint64_t cs = rd32(p + 20), us = rd32(p + 24);
        uint16_t nlen = rd16(p + 28), xlen = rd16(p + 30), clen = rd16(p + 32);
        uint64_t lho = rd32(p + 42);
        const uint8_t *x = p + 46 + nlen, *xend = x + xlen;
        while (x + 4 <= xend) {
            uint16_t id = rd16(x), sz = rd16(x + 2);
            if (id == 0x0001) {                      /* ZIP64 extended info */
                const uint8_t *q = x + 4;
                if (us == 0xFFFFFFFFu && q + 8 <= xend) { us = rd64(q); q += 8; }
                if (cs == 0xFFFFFFFFu && q + 8 <= xend) { cs = rd64(q); q += 8; }
                if (lho == 0xFFFFFFFFu && q + 8 <= xend) { lho = rd64(q); q += 8; }
            }
            x += 4 + sz;
        }
        z->method[i] = m; z->csize[i] = cs; z->usize[i] = us; z->lho[i] = lho;
        i++;
        p += 46 + nlen + xlen + clen;
    }
    z->count = i;
    return z;
}

void k93_zip_close(k93_zip *z)
{
    if (!z) return;
    munmap((void *)z->map, z->maplen);
    close(z->fd);
    free(z->lho); free(z->csize); free(z->usize); free(z->method);
    free(z);
}

int k93_zip_count(const k93_zip *z) { return z ? z->count : 0; }

static int entry_name(const k93_zip *z, int idx, const uint8_t **np, uint16_t *nlen)
{
    if (!z || idx < 0 || idx >= z->count) return -1;
    const uint8_t *p = z->map + z->lho[idx];
    if (z->lho[idx] + 30 > z->maplen || rd32(p) != 0x04034b50) return -1;
    *nlen = rd16(p + 26);
    *np = p + 30;
    return 0;
}

int k93_zip_stat(const k93_zip *z, int idx, char *name, size_t namelen,
                 uint64_t *size, uint64_t *csize, int *method, int *isdir)
{
    const uint8_t *np; uint16_t nlen;
    if (entry_name(z, idx, &np, &nlen) < 0) return -1;
    size_t c = nlen < namelen - 1 ? nlen : namelen - 1;
    memcpy(name, np, c); name[c] = 0;
    if (size) *size = z->usize[idx];
    if (csize) *csize = z->csize[idx];
    if (method) *method = z->method[idx];
    if (isdir) *isdir = (name[c ? c - 1 : 0] == '/');
    return 0;
}

int k93_zip_extract_fd(const k93_zip *z, int idx, int outfd)
{
    const uint8_t *np; uint16_t nlen;
    if (entry_name(z, idx, &np, &nlen) < 0) return -1;
    /* local header: sig(4) ver(2) flags(2) method(2) time(2) date(2) crc(4)
     * csize(4) usize(4) namelen(2) extralen(2) -> name starts at +30 */
    uint16_t xlen = rd16(np - 2);
    const uint8_t *data = np + nlen + xlen;
    uint64_t cs = z->csize[idx], us = z->usize[idx];

    if (z->method[idx] == 0) {
        uint64_t left = cs;
        const uint8_t *p = data;
        while (left) {
            ssize_t w = write(outfd, p, left > (1u << 20) ? (1u << 20) : (size_t)left);
            if (w <= 0) return -1;
            p += w; left -= (uint64_t)w;
        }
        return 0;
    }
    if (z->method[idx] != 8) return -2;   /* unsupported compression */

    z_stream s;
    memset(&s, 0, sizeof s);
    if (inflateInit2(&s, -15) != Z_OK) return -1;
    uint8_t *out = k93_xmalloc(1 << 20);
    s.next_in = (Bytef *)data;
    s.avail_in = (uInt)cs;
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
        if (rc) break;
        if (r == Z_STREAM_END) break;
        if (r != Z_OK) { rc = -3; break; }
    } while (s.avail_in || s.avail_out == 0);
    inflateEnd(&s);
    free(out);
    (void)us;
    return rc;
}

/* reject absolute paths and ".." segments (zip-slip) */
static int safe_join(char *out, size_t cap, const char *root, const char *rel)
{
    if (rel[0] == '/' || strstr(rel, "..")) return -1;
    if (snprintf(out, cap, "%s/%s", root, rel) >= (int)cap) return -1;
    return 0;
}

int k93_zip_extract(const k93_zip *z, int idx, const char *destpath)
{
    char name[1024];
    uint64_t size, csize; int method, isdir;
    if (k93_zip_stat(z, idx, name, sizeof name, &size, &csize, &method, &isdir) < 0) return -1;
    if (isdir) return k93_mkdir_p(destpath, 0755);
    char dir[1100];
    k93_strlcpy(dir, destpath, sizeof dir);
    for (char *q = dir + strlen(dir); q > dir; q--) {
        if (*q == '/') { *q = 0; break; }
    }
    k93_mkdir_p(dir, 0755);
    int fd = open(destpath, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) return -1;
    int rc = k93_zip_extract_fd(z, idx, fd);
    close(fd);
    return rc;
}

int k93_zip_extract_all(const k93_zip *z, const char *rootdir)
{
    int n = 0;
    for (int i = 0; i < z->count; i++) {
        char name[1024], dest[2100];
        uint64_t size, csize; int method, isdir;
        if (k93_zip_stat(z, i, name, sizeof name, &size, &csize, &method, &isdir) < 0) return -1;
        if (safe_join(dest, sizeof dest, rootdir, name) < 0) return -1;
        if (k93_zip_extract(z, i, dest) < 0) return -1;
        n++;
    }
    return n;
}

int k93_zip_find(const k93_zip *z, const char *suffix)
{
    size_t sl = strlen(suffix);
    for (int i = 0; i < z->count; i++) {
        char name[1024];
        uint64_t a, b; int c, d;
        if (k93_zip_stat(z, i, name, sizeof name, &a, &b, &c, &d) < 0) continue;
        size_t nl = strlen(name);
        if (nl >= sl && !strcmp(name + nl - sl, suffix)) return i;
    }
    return -1;
}
