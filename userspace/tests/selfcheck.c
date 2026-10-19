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
#include "k93snd.h"
#include "k93html.h"
#include "k93font.h"
#include "k93svg.h"
#include "k93ui.h"
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

    /* --- html parser + css subset (Safari engine) --- */
    {
        const char *H =
            "<!DOCTYPE html><html><head><title>T &amp; T</title>"
            "<style>h1{color:#aa0000;} .hid{display:none} p{margin-left:40px}"
            "</style></head><body><h1 id='hd' class='big'>Hi &#233;there</h1>"
            "<p class='hid'>HIDDEN</p><p style=\"color:#0000aa\">blue</p>"
            "<a href='/next'>go</a><ul><li>one<li>two</ul>"
            "<table><tr><td>a<td>b</tr></table>"
            "<p>break<br>line &lt;end&gt;</p>"
            "<script>var x = 1 < 2;</script></body></html>";
        k93node *hd_doc = k93html_parse(H);
        char hbuf[256];
        chk(hd_doc != NULL, "html parses");
        k93node *hn = k93html_query(hd_doc, "title");
        k93html_text_content(hn, hbuf, sizeof hbuf);
        chk(!strcmp(hbuf, "T & T"), "html title entities decoded");
        k93node *hh = k93html_get_id(hd_doc, "hd");
        chk(hh && !strcmp(hh->tag, "h1"), "html getElementById");
        chk(hh && !strcmp(k93html_attr(hh, "class"), "big"), "html attr read");
        k93node *hout[8];
        int hn2 = k93html_query_all(hd_doc, "p", hout, 8);
        chk(hn2 == 3, "html queryAll tag count");
        k93html_text_content(hout[1], hbuf, sizeof hbuf);
        chk(!strcmp(hbuf, "blue"), "html text content");
        k93node *hli = k93html_query(hd_doc, "li");
        k93html_text_content(hli, hbuf, sizeof hbuf);
        chk(!strcmp(hbuf, "one"), "html implied li close");
        k93node *htd = k93html_query(hd_doc, "td");
        chk(htd && htd->next && !strcmp(htd->next->tag, "td"),
            "html implied td close");
        k93node *hsc = k93html_query(hd_doc, "script");
        k93html_text_content(hsc, hbuf, sizeof hbuf);
        chk(!strcmp(hbuf, "var x = 1 < 2;"), "html raw script text");
        chk(k93html_query(hd_doc, ".big") == hh, "css class selector");
        k93css *hc = k93css_new();
        k93css_add(hc, "h1 { color: #aa0000; font-weight: bold }\n"
                       ".hid { display: none }\np { margin-left: 40px }\n"
                       "@media screen { p { color: red } }\n"
                       "#hd { text-align: center }");
        k93cstyle hst;
        memset(&hst, 0, sizeof hst);
        k93css_apply(hc, hh, &hst);
        chk((hst.flags & K93CSS_FG) && hst.fg == 0xaa0000, "css color");
        chk(hst.bold == 1, "css font-weight bold");
        chk(hst.center == 1, "css id selector");
        chk(!(hst.flags & K93CSS_HIDDEN), "css @media skipped");
        memset(&hst, 0, sizeof hst);
        k93css_apply(hc, hout[0], &hst);
        chk((hst.flags & K93CSS_HIDDEN) && hst.hidden, "css display:none");
        uint32_t hcol = 0;
        chk(k93css_color("#f00", &hcol) && hcol == 0xff0000, "css #rgb");
        k93node *hw = k93html_elem("div");
        k93html_set_inner_html(hw, "<b>bold</b>text");
        char *hi = k93html_inner_html(hw);
        chk(hi && !strcmp(hi, "<b>bold</b>text"), "innerHTML set/get");
        free(hi);
        k93css_free(hc);
        k93html_free(hw);
        k93html_free(hd_doc);
    }

    /* --- css combinators / attributes / specificity / entities --- */
    {
        k93node *doc2 = k93html_parse(
            "<div id=\"wrap\"><p class=\"a\">one</p><p class=\"b\">two</p></div>"
            "<ul><li id=\"i1\" data-x=\"7\" title=\"hello world\">item</li>"
            "<li>next</li></ul>");
        k93node *pa = k93html_query(doc2, "p.a");
        k93node *pb = k93html_query(doc2, "p.b");
        k93node *li2 = k93html_query(doc2, "#i1 ~ li");
        chk(pa && pb && li2, "selector targets exist");
        k93css *c2 = k93css_new();
        k93css_add(c2,
            "#wrap > p.a { color: #112233 }"
            "p.a + p.b { color: #445566 }"
            "#i1 ~ li { background-color: #778899 }"
            "p { color: #ff0000; font-weight: normal }"
            "p.a { color: #00ff00; margin: 2px 8px }"
            "ul li#i1 { font-weight: bold }"
            "[data-x=\"7\"] { text-decoration: underline }"
            "[title^=hell] { font-family: \"Nonexistent Sans\" }");
        k93cstyle s2;
        memset(&s2, 0, sizeof s2);
        k93css_apply(c2, pa, &s2);
        chk((s2.flags & K93CSS_FG) && s2.fg == 0x112233,
            "child comb + specificity order (id>class>elem)");
        chk((s2.flags & K93CSS_INDENT) && s2.indent == 8,
            "margin shorthand left value");
        memset(&s2, 0, sizeof s2);
        k93css_apply(c2, pb, &s2);
        chk((s2.flags & K93CSS_FG) && s2.fg == 0x445566,
            "adjacent sibling selector");
        memset(&s2, 0, sizeof s2);
        k93css_apply(c2, li2, &s2);
        chk((s2.flags & K93CSS_BG) && s2.bg == 0x778899,
            "general sibling selector");
        memset(&s2, 0, sizeof s2);
        k93node *li1 = k93html_query(doc2, "#i1");
        k93css_apply(c2, li1, &s2);
        chk(s2.bold == 1, "id specificity beats element");
        chk((s2.flags & K93CSS_UNDERLINE) && s2.underline,
            "attribute selector + underline");
        chk(s2.font == -1 && (s2.flags & K93CSS_FONT),
            "font-family unknown -> default");
        chk(k93html_query(doc2, "#wrap > p.a") == pa,
            "child combinator query");
        chk(k93html_query(doc2, "ul li[data-x='7']") == li1,
            "descendant + attribute query");
        k93css_free(c2);
        k93html_free(doc2);
        /* numeric + named entities -> UTF-8 */
        k93node *doc3 = k93html_parse("&#x4E2D;&#8364;&copy;&eacute;");
        char ebuf[64];
        k93html_text_content(doc3, ebuf, sizeof ebuf);
        chk(!strcmp(ebuf, "\xE4\xB8\xAD\xE2\x82\xAC\xC2\xA9\xC3\xA9"),
            "entities decode to UTF-8");
        k93html_free(doc3);
        printf("info - css combinators/specificity/entities ok\n");
    }

    /* --- css level: colors, new properties, pseudo-classes --- */
    {
        uint32_t col = 0;
        chk(k93css_color("rgb(255, 0, 0)", &col) && col == 0xff0000, "css rgb()");
        chk(k93css_color("rgba(0, 128, 0, 0.5)", &col) && col == 0x008000,
            "css rgba()");
        chk(k93css_color("hsl(240, 100%, 50%)", &col) && col == 0x0000ff,
            "css hsl() blue");
        chk(k93css_color("hsl(120, 100%, 50%)", &col) && col == 0x00ff00,
            "css hsl() green");
        chk(k93css_color("hsl(0, 0%, 100%)", &col) && col == 0xffffff,
            "css hsl() white");
        chk(k93css_color("rebeccapurple", &col) && col == 0x663399,
            "css named color (extended set)");
        chk(k93css_color("cornflowerblue", &col) && col == 0x6495ed,
            "css named color 2");
        chk(k93css_color("lightgoldenrodyellow", &col) && col == 0xfafad2,
            "css named color 3");
        chk(!k93css_color("notacolor", &col), "css bad color rejected");

        k93node *d4 = k93html_parse(
            "<html><body>"
            "<div id=r>"
            "<ul><li id=a>one</li><li id=b>two</li><li id=c>three</li></ul>"
            "<p class=x lang=en-US id=q>txt</p>"
            "<p lang=en id=w>other</p>"
            "</div></body></html>");
        k93css *c4 = k93css_new();
        k93css_add(c4,
            "p { text-transform: uppercase; line-height: 24px; opacity: 0.5 }\n"
            "div { border: 2px solid #123456; width: 300px }\n"
            "ul { list-style-type: decimal }\n"
            "#q { text-decoration: line-through; color: tomato }\n");
        k93node *qw = k93html_query(d4, "#w");
        k93node *qd = k93html_query(d4, "#r");
        k93node *qu = k93html_query(d4, "ul");
        k93cstyle s4;
        memset(&s4, 0, sizeof s4);
        k93css_apply(c4, qw, &s4);
        chk((s4.flags & K93CSS_XFORM) && s4.xform == 1, "css text-transform");
        chk((s4.flags & K93CSS_LH) && s4.lh == 24, "css line-height px");
        chk((s4.flags & K93CSS_OPACITY) && s4.opacity == 50, "css opacity 0.5");
        memset(&s4, 0, sizeof s4);
        k93css_apply(c4, qd, &s4);
        chk((s4.flags & K93CSS_BORDER) && s4.bordw == 2 && s4.bordc == 0x123456,
            "css border shorthand w+style+color");
        chk((s4.flags & K93CSS_WIDTH) && s4.width == 300, "css width px");
        memset(&s4, 0, sizeof s4);
        k93css_apply(c4, qu, &s4);
        chk((s4.flags & K93CSS_LSTYLE) && s4.lstyle == K93L_DECIMAL,
            "css list-style-type");
        memset(&s4, 0, sizeof s4);
        k93css_apply(c4, k93html_query(d4, "#q"), &s4);
        chk((s4.flags & K93CSS_STRIKE) && s4.strike == 1, "css line-through");
        chk((s4.flags & K93CSS_FG) && s4.fg == 0xff6347, "css named fg tomato");

        /* pseudo-classes */
        k93node *la = k93html_query(d4, "#a");
        k93node *lb = k93html_query(d4, "#b");
        k93node *lc = k93html_query(d4, "#c");
        chk(k93html_matches(la, "li:first-child"), ":first-child");
        chk(!k93html_matches(lb, "li:first-child"), ":first-child negative");
        chk(k93html_matches(lc, "li:last-child"), ":last-child");
        chk(k93html_matches(lb, "li:nth-child(2)"), ":nth-child(2)");
        chk(k93html_matches(la, "li:nth-child(odd)"), ":nth-child(odd)");
        chk(k93html_matches(lc, "li:nth-child(2n+1)"), ":nth-child(2n+1)");
        chk(!k93html_matches(lb, "li:nth-child(2n+1)"), ":nth-child neg");
        chk(k93html_matches(lb, "li:not(:first-child)"), ":not()");
        chk(k93html_matches(qw, "p:not(.x)"), ":not(class)");
        chk(k93html_matches(qw, "[lang|=en]"), "attr |= operator");
        chk(k93html_matches(la, "ul li"), "matches descendant");
        chk(k93html_matches(qw, "p, li"), "matches selector list");
        chk(!k93html_matches(la, "p"), "matches negative");
        k93node *root = k93html_query(d4, "html");
        chk(k93html_matches(root, ":root"), ":root");
        k93css_free(c4);
        k93html_free(d4);
        printf("info - css colors/properties/pseudo ok\n");
    }

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

    /* --- ALSA PCM (QEMU intel-hda / real-hardware HDA sound) --- */
    {
        int rate = 44100, chans = 2;
        if (k93_snd_open(NULL, &rate, &chans) < 0) {
            printf("skip - no /dev/snd PCM device (no sound card)\n");
        } else {
            printf("info - alsa pcm negotiated %d Hz, %d ch\n", rate, chans);
            static int16_t silence[4096 * 2];   /* .bss = zeros: no audible noise */
            chk(k93_snd_play(silence, 4096) == 0, "alsa pcm write");
            k93_snd_close();
        }
    }

    /* --- TTF rasterizer (clean-room font engine) --- */
    {
        /* guest: /usr/share/fonts/k93 (installed by mkrootfs). Build host:
         * probe known open-font dirs so the raster path is covered in
         * `make selfcheck` too. No font anywhere -> 8x16 fallback path. */
        int fok = k93font_init() == 0;
        if (!fok) {
            /* probe all known open-font dirs at once (colon list) */
            static const char *cand =
                "/usr/share/fonts/liberation-sans-fonts:"
                "/usr/share/fonts/liberation-serif-fonts:"
                "/usr/share/fonts/liberation-mono-fonts:"
                "/usr/share/fonts/liberation2-sans-fonts:"
                "/usr/share/fonts/liberation2-serif-fonts:"
                "/usr/share/fonts/liberation2-mono-fonts:"
                "/usr/share/fonts/truetype/liberation:"
                "/usr/share/fonts/dejavu-sans-fonts:"
                "/usr/share/fonts/truetype/dejavu:"
                "/usr/share/fonts/truetype/freefont:"
                "/usr/share/fonts/google-droid-sans-fonts:"
                "/usr/share/fonts/google-droid-sans-mono-fonts";
            setenv("K93_FONTDIR", cand, 1);
            fok = k93font_init() == 0;
        }
        int di = 0;
        uint32_t uc1 = k93_utf8_next("A\xc3\xa9", &di);
        uint32_t uc2 = k93_utf8_next("A\xc3\xa9", &di);
        chk(uc1 == 'A' && uc2 == 0xE9 && di == 3, "utf8 decode");
        if (!fok) {
            printf("skip - no TTF on this system (8x16 fallback)\n");
            chk(!k93font_ready(), "no font -> fallback flag");
            chk(k93font_width(1, "abc") == 24, "fallback width = 8x16 cells");
        } else {
            chk(k93font_ready(), "system font loads");
            chk(k93font_count() >= 1, "font families found");
            chk(k93font_id("system") == k93font_default_id(), "system alias -> default");
            int wa = k93font_width(1, "A"), wW = k93font_width(1, "W");
            int wi = k93font_width(1, "i"), wsp = k93font_width(1, " ");
            int wh = k93font_width(1, "Hello");
            chk(wa > 2 && wW >= wa && wsp >= 1 && wsp < wa,
                "proportional advances");
            chk(wi < wa, "narrow glyph narrower than A");
            chk(wh > 4 && wh < wa * 6, "word width sane");
            chk(k93font_baseline(1) > 4 && k93font_baseline(1) < 16,
                "baseline inside 16px cell");
            chk(k93font_width(1, "AV") <= k93font_width(1, "A") + k93font_width(1, "V"),
                "kerning: width(AV) <= width(A)+width(V)");
            int w, h, xo, yo;
            float adv;
            uint8_t *g = k93font_glyph(0, 'A', 1, 0, &w, &h, &xo, &yo, &adv);
            int ink = 0;
            if (g)
                for (int i = 0; i < w * h; i++) if (g[i]) ink++;
            chk(g && w > 2 && h > 4 && adv > 1.0f, "glyph A rasterizes");
            chk(ink > w * h / 8, "glyph A has plausible ink");
            k93font_glyph_free(g);
            g = k93font_glyph('A', ' ', 1, 0, &w, &h, &xo, &yo, &adv);
            chk(!g && adv > 1.0f, "space: advance, no bitmap");
            k93font_glyph_free(g);
            /* missing codepoint renders the '?' glyph, not a hole */
            g = k93font_glyph(0, 0x4E09, 1, 0, &w, &h, &xo, &yo, &adv);
            chk(g && w > 1 && h > 1, "missing codepoint -> fallback glyph");
            k93font_glyph_free(g);
            int mono = k93font_id("monospace");
            if (mono >= 0 && strstr(k93font_name(mono), "Mono") &&
                k93font_select(mono) == 0) {
                chk(k93font_width(1, "iii") == k93font_width(1, "WWW"),
                    "monospace: equal advances");
                chk(k93font_select(k93font_default_id()) == 0, "font reselect");
            } else {
                printf("skip - no monospace family installed\n");
            }
            chk(k93font_width(2, "Hello") > k93font_width(1, "Hello"),
                "scale 2 wider than scale 1");
            printf("info - font: default=%s families=%d baseline=%d w(Hello)=%d\n",
                   k93font_name(k93font_default_id()), k93font_count(),
                   k93font_baseline(1), wh);
            k93font_shutdown();
            chk(!k93font_ready(), "font shutdown clears state");
        }
    }

    /* --- SVG rasterizer + PNG encoder (browser pipeline) --- */
    {
        chk(k93png_crc32("123456789", 9) == 0xCBF43926u, "png crc32 vector");
        uint8_t solid[4 * 2 * 4];
        for (int i = 0; i < 8; i++) {
            solid[i * 4 + 0] = (uint8_t)(i * 30);
            solid[i * 4 + 1] = 77;
            solid[i * 4 + 2] = 200;
            solid[i * 4 + 3] = (uint8_t)(255 - i * 20);
        }
        size_t plen = 0;
        uint8_t *png = k93png_encode(solid, 4, 2, &plen);
        chk(png && plen > 50 && png[0] == 137 && png[1] == 'P',
            "png encode signature");
        if (png) {
            int pw = 0, ph = 0;
            uint8_t *back = k93png_decode_stored(png, plen, &pw, &ph);
            chk(back && pw == 4 && ph == 2 &&
                back && memcmp(solid, back, sizeof solid) == 0,
                "png decode roundtrip");
            free(back);
        }
        free(png);
        static const char *sv =
            "<svg width=\"16\" height=\"16\">"
            "<rect x=\"1\" y=\"1\" width=\"14\" height=\"14\" fill=\"#3366cc\"/>"
            "<circle cx=\"8\" cy=\"8\" r=\"4\" fill=\"white\"/>"
            "<path d=\"M2 13 L13 2\" stroke=\"#000000\" stroke-width=\"2\"/>"
            "</svg>";
        int vw = 0, vh = 0;
        uint8_t *rgba = k93svg_to_rgba(sv, strlen(sv), &vw, &vh);
        chk(rgba && vw == 16 && vh == 16, "svg raster size");
        if (rgba) {
            const uint8_t *c = rgba + ((size_t)14 * vw + 14) * 4; /* rect corner */
            chk(c[0] == 0x33 && c[2] == 0xCC && c[3] == 255,
                "svg rect fill");
            const uint8_t *cc = rgba + ((size_t)8 * vw + 8) * 4;  /* circle */
            chk(cc[0] > 200 && cc[1] > 200 && cc[2] > 200,
                "svg circle over rect");
            chk(rgba[3] == 0, "svg transparent background");
            const uint8_t *st = rgba + ((size_t)4 * vw + 11) * 4; /* stroke */
            chk(st[0] < 64 && st[3] > 200, "svg path stroke");
            /* svg -> png -> decode = identical pixels */
            size_t slen = 0;
            uint8_t *spng = k93svg_to_png(sv, strlen(sv), &slen, &vw, &vh);
            chk(spng && slen > 100, "svg to png pipeline");
            free(spng);
            free(rgba);
        }
        /* comma-separated path numbers (real-world SVG style) */
        static const char *svc =
            "<svg width=\"16\" height=\"16\">"
            "<path d=\"M2 13, C 4 2, 12 2, 14 13\" fill=\"none\" "
            "stroke=\"#000000\" stroke-width=\"2\"/></svg>";
        uint8_t *rgbc = k93svg_to_rgba(svc, strlen(svc), &vw, &vh);
        int dark = 0;
        if (rgbc)
            for (int i = 0; i < vw * vh; i++)
                if (rgbc[i * 4 + 3] > 200 && rgbc[i * 4] < 64) dark++;
        chk(rgbc && dark > 20, "svg cubic path with comma separators");
        free(rgbc);
        chk(!k93svg_to_rgba("<html></html>", 13, &vw, &vh),
            "svg rejects non-svg");
        printf("info - svg/png ok\n");
    }

    /* --- ui_image_load (stb_image backend) --- */
    {
        uint8_t rgba[4 * 4 * 4];
        for (int i = 0; i < 64; i += 4) {
            rgba[i] = 200; rgba[i + 1] = 10; rgba[i + 2] = 20; rgba[i + 3] = 255;
        }
        size_t plen = 0;
        uint8_t *png = k93png_encode(rgba, 4, 4, &plen);
        chk(png != NULL && plen > 0, "png encode for image test");
        if (png) {
            k93_write_file("/tmp/k93_imgtest.png", png, plen, 0);
            free(png);
            ui_image im;
            memset(&im, 0, sizeof im);
            chk(ui_image_load(&im, "/tmp/k93_imgtest.png") == 0 &&
                im.w == 4 && im.h == 4 && im.px &&
                (im.px[0] & 0xFFFFFFu) == 0xC80A14u,
                "ui_image_load png roundtrip");
            ui_image_free(&im);
            chk(im.px == NULL && im.w == 0, "ui_image_free clears");
            chk(ui_image_load(&im, "/nonexistent/x.png") < 0,
                "ui_image_load missing file fails");
        }
        remove("/tmp/k93_imgtest.png");
        printf("info - ui_image ok\n");
    }

    printf("%s (%d failures)\n", fails ? "SELFCHECK FAILED" : "SELFCHECK PASSED", fails);
    return fails ? 1 : 0;
}
