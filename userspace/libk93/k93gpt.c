/* k93gpt.c - minimal GPT writer/verifier (clean-room, UEFI spec rev 2.x). */
#define _GNU_SOURCE
#include "k93gpt.h"
#include "k93util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <stdint.h>
#include <zlib.h>

/* standard GUIDs (byte layout as stored on disk, mixed-endian) */
const uint8_t k93_guid_linux_fs[16] = {
    0xaf, 0x63, 0x1c, 0x7c, 0x87, 0x4f, 0x04, 0x63,
    0x82, 0x4d, 0x75, 0x24, 0x25, 0x90, 0xa4, 0x02
};
const uint8_t k93_guid_linux_swap[16] = {
    0x6d, 0xfd, 0x57, 0x06, 0xab, 0xa4, 0xc4, 0x43,
    0x84, 0xe5, 0x09, 0x33, 0xc8, 0x4b, 0x4f, 0x4f
};
static const uint8_t GPT_MAGIC[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };

static void put_le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put_le64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t rd_le32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint64_t rd_le64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

static uint32_t crc32_buf(const uint8_t *p, size_t n)
{
    return (uint32_t)crc32(0L, p, (uInt)n);
}

/* deterministic-but-unique disk GUID from time + path noise */
static void make_guid(uint8_t g[16])
{
    uint64_t t = (uint64_t)time(NULL);
    uint64_t r = ((uint64_t)getpid() << 32) ^ (uint64_t)(uintptr_t)&t ^ (t * 2654435761u);
    for (int i = 0; i < 16; i++) g[i] = (uint8_t)(r >> ((i % 8) * 8)) ^ (uint8_t)(t >> ((i % 6) * 8));
    g[6] = (uint8_t)((g[6] & 0x0f) | 0x40);           /* version 4 */
    g[8] = (uint8_t)((g[8] & 0x3f) | 0x80);           /* variant */
}

static int write_at(int fd, const void *buf, size_t n, off_t off)
{
    if (lseek(fd, off, SEEK_SET) < 0) return -1;
    const uint8_t *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}

int k93_gpt_write(int fd, uint32_t sector_size, uint64_t total_sectors,
                  const k93_gpt_part *parts, int nparts)
{
    if (sector_size < 512 || total_sectors < 34 || nparts < 1 || nparts > 128)
        return -1;
    const uint32_t entsz = 128;
    uint32_t nent = 128;                                 /* table capacity */
    uint64_t table_bytes = (uint64_t)nent * entsz;
    uint32_t table_sectors = (uint32_t)((table_bytes + sector_size - 1) / sector_size);

    uint64_t first_usable = 1 + table_sectors;           /* after LBA0 + table */
    uint64_t last_usable = total_sectors - table_sectors - 1;
    if (first_usable >= last_usable) return -1;

    /* partition entries */
    uint8_t *table = calloc(1, table_bytes);
    if (!table) return -1;
    for (int i = 0; i < nparts; i++) {
        uint8_t *e = table + (size_t)i * entsz;
        const uint8_t *tg = parts[i].type_guid ? parts[i].type_guid : k93_guid_linux_fs;
        memcpy(e, tg, 16);
        make_guid(e + 16);
        put_le64(e + 32, parts[i].start_sector);
        put_le64(e + 40, parts[i].start_sector + parts[i].sector_count - 1);
        put_le64(e + 56, 0);                            /* attributes */
        for (int c = 0; c < 35 && parts[i].name[c]; c++) {
            e[56 + c * 2] = (uint8_t)parts[i].name[c];  /* UTF-16LE, ASCII subset */
        }
    }
    uint32_t table_crc = crc32_buf(table, table_bytes);

    /* partition entries (backup) at end */
    uint8_t disk_guid[16];
    make_guid(disk_guid);

    /* --- protective MBR --- */
    uint8_t mbr[512];
    memset(mbr, 0, sizeof mbr);
    uint8_t *pe = mbr + 446;
    pe[4] = 0xEE;                                        /* GPT protective type */
    put_le32(pe + 8, 1);
    uint32_t sz = total_sectors > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)total_sectors;
    put_le32(pe + 12, sz);
    mbr[510] = 0x55; mbr[511] = 0xAA;
    if (write_at(fd, mbr, 512, 0) < 0) { free(table); return -1; }

    /* --- primary GPT header at LBA 1 --- */
    uint8_t hdr[512];
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, GPT_MAGIC, 8);
    put_le32(hdr + 8, 0x00010000);                      /* revision */
    put_le32(hdr + 12, 92);                             /* header size */
    /* hdr+16 crc filled later */
    put_le32(hdr + 20, 0);
    put_le64(hdr + 24, 1);                              /* current lba */
    put_le64(hdr + 32, total_sectors - 1);              /* backup lba */
    put_le64(hdr + 40, first_usable);
    put_le64(hdr + 48, last_usable);
    memcpy(hdr + 56, disk_guid, 16);
    put_le64(hdr + 72, 2);                              /* entries lba */
    put_le32(hdr + 80, nent);
    put_le32(hdr + 84, entsz);
    put_le32(hdr + 88, table_crc);
    uint32_t hcrc = crc32_buf(hdr, 92);
    put_le32(hdr + 16, hcrc);
    if (write_at(fd, hdr, 512, (off_t)sector_size) < 0) { free(table); return -1; }
    if (write_at(fd, table, table_bytes, (off_t)(2 * sector_size)) < 0) { free(table); return -1; }

    /* --- backup header + table at end of disk --- */
    uint64_t backup_hdr_lba = total_sectors - 1;
    uint64_t backup_tbl_lba = backup_hdr_lba - table_sectors;
    if (write_at(fd, table, table_bytes, (off_t)(backup_tbl_lba * sector_size)) < 0) { free(table); return -1; }
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, GPT_MAGIC, 8);
    put_le32(hdr + 8, 0x00010000);
    put_le32(hdr + 12, 92);
    put_le64(hdr + 24, backup_hdr_lba);
    put_le64(hdr + 32, 1);                              /* primary header lba */
    put_le64(hdr + 40, first_usable);
    put_le64(hdr + 48, last_usable);
    memcpy(hdr + 56, disk_guid, 16);
    put_le64(hdr + 72, backup_tbl_lba);
    put_le32(hdr + 80, nent);
    put_le32(hdr + 84, entsz);
    put_le32(hdr + 88, table_crc);
    hcrc = crc32_buf(hdr, 92);
    put_le32(hdr + 16, hcrc);
    if (write_at(fd, hdr, 512, (off_t)(backup_hdr_lba * sector_size)) < 0) { free(table); return -1; }
    fsync(fd);
    free(table);
    return 0;
}

int k93_gpt_verify(int fd, uint32_t sector_size)
{
    uint8_t hdr[512];
    if (lseek(fd, (off_t)sector_size, SEEK_SET) < 0) return -1;
    if (read(fd, hdr, 512) != 512) return -1;
    if (memcmp(hdr, GPT_MAGIC, 8) != 0) return -1;
    uint32_t stored = rd_le32(hdr + 16);
    uint8_t hcrcbuf[92];
    memcpy(hcrcbuf, hdr, 92);
    memset(hcrcbuf + 16, 0, 4);                          /* CRC field excluded */
    uint32_t calc = crc32_buf(hcrcbuf, 92);
    if (stored != calc) return -1;
    uint64_t tbl = rd_le64(hdr + 72);
    uint32_t nent = rd_le32(hdr + 80);
    uint32_t entsz = rd_le32(hdr + 84);
    if (nent > 1024 || entsz != 128) return -1;
    size_t bytes = (size_t)nent * entsz;
    uint8_t *table = malloc(bytes);
    if (!table) return -1;
    if (lseek(fd, (off_t)(tbl * sector_size), SEEK_SET) < 0) { free(table); return -1; }
    if (read(fd, table, bytes) != (ssize_t)bytes) { free(table); return -1; }
    uint32_t tcrc = rd_le32(hdr + 88);
    int ok = (crc32_buf(table, bytes) == tcrc);
    free(table);
    return ok ? 0 : -1;
}
