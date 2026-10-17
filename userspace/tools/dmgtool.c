/* k93dmgtool - inspect/extract .dmg (UDZO/UDIF) images. Uses libk93 reader. */
#include <k93dmg.h>
#include <k93util.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int usage(void)
{
    fprintf(stderr,
        "usage: k93dmgtool info  <file.dmg>\n"
        "       k93dmgtool x     <file.dmg> <out.raw>   (extract raw image)\n"
        "       k93dmgtool probe <file.dmg>\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 3) return usage();
    const char *cmd = argv[1], *path = argv[2];

    if (!strcmp(cmd, "probe")) {
        int ok = k93_dmg_probe(path);
        printf("%s: %s\n", path, ok ? "valid UDIF/UDZO image" : "not a supported dmg");
        return ok ? 0 : 1;
    }

    if (!strcmp(cmd, "info")) {
        k93_dmg *d = k93_dmg_open(path);
        if (!d) { fprintf(stderr, "k93dmgtool: cannot open %s (UDZO/UDIF only)\n", path); return 1; }
        char hs[32];
        k93_human_size(hs, sizeof(hs), (unsigned long long)k93_dmg_size(d));
        printf("file:   %s\nformat: UDZO (zlib blocks) / UDIF koly trailer\nsize:   %s (%llu bytes)\n",
               path, hs, (unsigned long long)k93_dmg_size(d));
        k93_dmg_close(d);
        return 0;
    }

    if (!strcmp(cmd, "x")) {
        if (argc < 4) return usage();
        k93_dmg *d = k93_dmg_open(path);
        if (!d) { fprintf(stderr, "k93dmgtool: cannot open %s\n", path); return 1; }
        int fd = open(argv[3], O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (fd < 0) { perror(argv[3]); k93_dmg_close(d); return 1; }
        int rc = k93_dmg_extract(d, fd);
        close(fd);
        k93_dmg_close(d);
        if (rc != 0) { fprintf(stderr, "k93dmgtool: extract failed (%d)\n", rc); return 1; }
        printf("extracted to %s\n", argv[3]);
        return 0;
    }
    return usage();
}
