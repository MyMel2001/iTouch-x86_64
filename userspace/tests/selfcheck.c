/* selfcheck.c - host+guest runnable checks for libk93 (no framework).
 * Usage: selfcheck [fixture-dir]
 * Fixture dir (default ./fixtures) may contain test.ipa and test.dmg made by
 * tools/mkfixtures.py. Every check prints "ok - <name>" or "FAIL - <name>". */
#define _GNU_SOURCE
#include "k93util.h"
#include "k93plist.h"
#include "k93zip.h"
#include "k93dmg.h"
#include "k93app.h"
#include "k93gpt.h"
#include <linux/fb.h>
#ifndef __user
#define __user
#endif
#include <linux/drm.h>
#include <linux/drm_mode.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static int fails = 0;

static void chk(int cond, const char *name)
{
    printf("%s - %s\n", cond ? "ok  " : "FAIL", name);
    if (!cond) fails++;
}

static const char *SAMPLE =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "  <key>CFBundleName</key><string>Test &amp; App</string>\n"
    "  <key>CFBundleIdentifier</key><string>com.k93.test</string>\n"
    "  <key>CFBundleVersion</key><integer>42</integer>\n"
    "  <key>CFBundleExecutable</key><string>Test</string>\n"
    "  <key>CFBundleNested</key>\n"
    "  <dict><key>Alpha</key><string>one</string>\n"
    "        <key>List</key><array><string>a</string><integer>7</integer><true/></array>\n"
    "  </dict>\n"
    "</dict>\n"
    "</plist>\n";

int main(int argc, char **argv)
{
    const char *fxdir = argc > 1 ? argv[1] : "fixtures";
    char path[512], err[128];

    /* --- plist --- */
    k93_plist *p = k93_plist_parse(SAMPLE, strlen(SAMPLE), err, sizeof err);
    chk(p != NULL, "plist parses");
    if (p) {
        chk(!strcmp(k93_plist_dict_str(p, "CFBundleName", ""), "Test & App"), "plist string + entities");
        chk(!strcmp(k93_plist_dict_str(p, "CFBundleIdentifier", ""), "com.k93.test"), "plist identifier");
        chk(k93_plist_dict_int(p, "CFBundleVersion", 0) == 42, "plist integer");
        const k93_plist *nested = k93_plist_dict_get(p, "CFBundleNested");
        chk(nested && k93_plist_type_of(nested) == K93_PLIST_DICT, "nested dict");
        const k93_plist *list = nested ? k93_plist_dict_get(nested, "List") : NULL;
        chk(k93_plist_array_count(list) == 3, "array count");
        chk(!strcmp(k93_plist_string(k93_plist_array_at(list, 0)), "a"), "array string elem");
        chk(k93_plist_int(k93_plist_array_at(list, 1), 0) == 7, "array int elem");
        chk(k93_plist_bool(k93_plist_array_at(list, 2), 0) == 1, "array bool elem");
        k93_plist_free(p);
    }
    k93_plist *bad = k93_plist_parse("bplist00xxxx", 12, err, sizeof err);
    chk(bad == NULL && strstr(err, "binary"), "binary plist rejected with reason");
    k93_plist_free(bad);

    /* malformed xml must not crash */
    k93_plist *m1 = k93_plist_parse("<plist><dict><key>a</key>", 24, err, sizeof err);
    chk(m1 == NULL, "truncated dict -> error");
    k93_plist *m2 = k93_plist_parse("<plist><dict><key>a</key><string>x</dict></plist>", 48, err, sizeof err);
    chk(m2 == NULL, "mismatched tag -> error");
    k93_plist *m3 = k93_plist_parse("<plist><array><string>z", 23, err, sizeof err);
    chk(m3 == NULL, "truncated array -> error");
    k93_plist_free(m1); k93_plist_free(m2); k93_plist_free(m3);

    /* --- zip/ipa --- */
    snprintf(path, sizeof path, "%s/test.ipa", fxdir);
    k93_zip *z = k93_zip_open(path);
    chk(z != NULL, "ipa (zip) opens");
    if (z) {
        chk(k93_zip_count(z) >= 3, "ipa entry count");
        int idx = k93_zip_find(z, "Info.plist");
        chk(idx >= 0, "ipa Info.plist located");
        char tmp[] = "/tmp/k93-sc-XXXXXX";
        int tfd = mkstemp(tmp);
        if (tfd >= 0 && idx >= 0) {
            chk(k93_zip_extract_fd(z, idx, tfd) == 0, "ipa entry inflates");
            close(tfd);
            size_t l = 0;
            char *txt = k93_read_file(tmp, &l);
            chk(txt && strstr(txt, "com.k93.fixture"), "extracted content matches");
            free(txt);
            unlink(tmp);
        }
        /* zip-slip protection */
        int rc = k93_zip_extract_all(z, "/tmp/k93-slip-test");
        chk(rc > 0, "safe extraction of all entries");
        k93_rm_tree("/tmp/k93-slip-test");
        k93_zip_close(z);
    }
    k93_zip *notz = k93_zip_open("/etc/hostname");
    chk(notz == NULL, "non-zip rejected");
    k93_zip_close(notz);

    /* --- dmg --- */
    snprintf(path, sizeof path, "%s/test.dmg", fxdir);
    k93_dmg *dm = k93_dmg_open(path);
    chk(dm != NULL, "dmg (UDZO) opens");
    if (dm) {
        chk(k93_dmg_size(dm) == 1024 * 1024, "dmg logical size");
        char tmp[] = "/tmp/k93-dmg-XXXXXX";
        int tfd = mkstemp(tmp);
        if (tfd >= 0) {
            chk(k93_dmg_extract(dm, tfd) == 0, "dmg extracts to raw image");
            /* verify first sector content */
            unsigned char buf[512];
            lseek(tfd, 0, SEEK_SET);
            chk(read(tfd, buf, 512) == 512 && !memcmp(buf, "K93DMGTEST", 10), "dmg payload bytes match");
            /* and a byte near the end of the compressed region */
            lseek(tfd, 512 * 300, SEEK_SET);
            chk(read(tfd, buf, 16) == 16, "dmg tail readable");
            close(tfd);
        }
        k93_dmg_close(dm);
    }
    chk(!k93_dmg_probe("/etc/hostname"), "non-dmg rejected");

    /* --- util --- */
    chk(k93_mkdir_p("/tmp/k93-sc/a/b/c", 0755) == 0, "mkdir_p");
    chk(k93_write_file("/tmp/k93-sc/a/b/c/f.txt", "hello", 5, 0) == 0, "write_file");
    size_t l = 0;
    char *r = k93_read_file("/tmp/k93-sc/a/b/c/f.txt", &l);
    chk(r && l == 5 && !memcmp(r, "hello", 5), "read_file");
    free(r);
    k93_rm_tree("/tmp/k93-sc");
    chk(access("/tmp/k93-sc", F_OK) != 0, "rm_tree");

    /* --- app scan on a synthetic bundle --- */
    if (geteuid() == 0) {
        k93_mkdir_p("/Applications/SelfCheck.app", 0755);
        FILE *f = fopen("/Applications/SelfCheck.app/Info.plist", "w");
        if (f) {
            fputs(SAMPLE, f);
            fclose(f);
        }
        k93_app apps[64];
        int n = k93_app_scan(apps, 64);
        int found = 0;
        for (int i = 0; i < n; i++) if (!strcmp(apps[i].ident, "com.k93.test")) found = 1;
        chk(found, "app scan finds synthetic bundle");
        k93_rm_tree("/Applications/SelfCheck.app");
    }

    /* --- gpt (write + verify on a temp 8 MiB image) --- */
    {
        char gptfile[] = "/tmp/k93-gpt-XXXXXX";
        int gfd = mkstemp(gptfile);
        if (gfd >= 0) {
            const uint64_t sectors = 16384;             /* 8 MiB @ 512 B */
            if (ftruncate(gfd, (off_t)(sectors * 512)) == 0) {
                k93_gpt_part parts[2];
                memset(parts, 0, sizeof parts);
                parts[0].start_sector = 2048;
                parts[0].sector_count = 8192;
                k93_strlcpy(parts[0].name, "K93OS root", sizeof parts[0].name);
                parts[1].start_sector = 2048 + 8192;
                parts[1].sector_count = 2048;
                k93_strlcpy(parts[1].name, "K93OS swap", sizeof parts[1].name);
                parts[1].type_guid = k93_guid_linux_swap;
                chk(k93_gpt_write(gfd, 512, sectors, parts, 2) == 0, "gpt write");
                chk(k93_gpt_verify(gfd, 512) == 0, "gpt verify");
                unsigned char mbr[512];
                lseek(gfd, 0, SEEK_SET);
                chk(read(gfd, mbr, 512) == 512 && mbr[510] == 0x55 && mbr[511] == 0xAA,
                    "gpt protective MBR signature");
            }
            close(gfd);
            unlink(gptfile);
        }
    }

    /* --- framebuffer (graphics stack; skipped when headless) --- */
    {
        int fbf = open("/dev/fb0", O_RDWR);
        if (fbf < 0) {
            printf("skip - no /dev/fb0 (headless text VM)\n");
        } else {
            struct fb_fix_screeninfo fi;
            struct fb_var_screeninfo vi;
            chk(ioctl(fbf, FBIOGET_FSCREENINFO, &fi) == 0 &&
                ioctl(fbf, FBIOGET_VSCREENINFO, &vi) == 0, "fb0 fixed+var info");
            printf("info - fb0 %ux%ux%ubpp line=%u id=%s\n", vi.xres, vi.yres,
                   vi.bits_per_pixel, fi.line_length, fi.id);
            chk(vi.bits_per_pixel == 16 || vi.bits_per_pixel == 24 ||
                vi.bits_per_pixel == 32, "fb0 pixel format supported by k93ui");
            size_t flen = fi.smem_len ? fi.smem_len
                                      : (size_t)fi.line_length * vi.yres_virtual;
            uint8_t *fm = mmap(NULL, flen, PROT_READ | PROT_WRITE, MAP_SHARED, fbf, 0);
            chk(fm != MAP_FAILED, "fb0 mmap");
            if (fm != MAP_FAILED) {
                /* checker write + readback proves the pixel path end-to-end */
                for (unsigned y = 0; y < 32 && y < vi.yres; y++)
                    memset(fm + (size_t)y * fi.line_length, (y & 1) ? 0xFF : 0x00,
                           fi.line_length < 64 ? fi.line_length : 64);
                volatile uint8_t v0 = fm[0];
                volatile uint8_t v1 = fm[fi.line_length];
                chk((v0 == 0x00 || v0 == 0xFF) && (v1 == 0x00 || v1 == 0xFF) && v0 != v1,
                    "fb pattern write/readback");
                munmap(fm, flen);
            }
            close(fbf);
        }
    }

    /* --- DRM/KMS dumb buffer (graphics stack on kernels without fbdev) --- */
    {
        int drif = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
        if (drif < 0) {
            printf("skip - no /dev/dri/card0 (no KMS device)\n");
        } else {
            struct drm_mode_create_dumb cd = { .width = 64, .height = 32, .bpp = 32 };
            chk(ioctl(drif, DRM_IOCTL_MODE_CREATE_DUMB, &cd) == 0 && cd.handle != 0,
                "kms dumb buffer create");
            chk(cd.size >= (uint64_t)cd.pitch * 32, "kms dumb buffer geometry");
            printf("info - kms dumb buffer %ux%u pitch=%u size=%llu\n", cd.width,
                   cd.height, cd.pitch, (unsigned long long)cd.size);
            struct drm_mode_map_dumb md = { .handle = cd.handle };
            chk(ioctl(drif, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0, "kms dumb buffer map");
            uint8_t *dm = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, drif, md.offset);
            chk(dm != MAP_FAILED, "kms dumb buffer mmap");
            if (dm != MAP_FAILED) {
                for (unsigned y = 0; y < 32; y++)
                    memset(dm + (size_t)y * cd.pitch, (y & 1) ? 0xAA : 0x55,
                           cd.pitch < 64 ? cd.pitch : 64);
                volatile uint8_t d0 = dm[0];
                volatile uint8_t d1 = dm[cd.pitch];
                chk((d0 == 0x55 || d0 == 0xAA) && d1 != d0,
                    "kms dumb buffer write/readback");
                munmap(dm, cd.size);
            }
            struct drm_mode_destroy_dumb dd = { .handle = cd.handle };
            ioctl(drif, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
            close(drif);
        }
    }

    printf("%s (%d failures)\n", fails ? "SELFCHECK FAILED" : "SELFCHECK PASSED", fails);
    return fails ? 1 : 0;
}
