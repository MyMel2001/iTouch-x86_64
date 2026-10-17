/* k93zip.h - minimal ZIP (.ipa) reader, clean-room, zlib inflate.
 * Supports: stored (0) and deflate (8) entries, ZIP64 EOCD locator. */
#ifndef K93ZIP_H
#define K93ZIP_H
#include <stddef.h>
#include <stdint.h>

typedef struct k93_zip k93_zip;

k93_zip *k93_zip_open(const char *path);
void     k93_zip_close(k93_zip *z);
int      k93_zip_count(const k93_zip *z);
/* name: PATH_MAX out; returns 0 on success */
int      k93_zip_stat(const k93_zip *z, int idx, char *name, size_t namelen,
                      uint64_t *size, uint64_t *csize, int *method, int *isdir);
/* extract one entry to an already-opened O_CREAT|O_WRONLY file descriptor */
int      k93_zip_extract_fd(const k93_zip *z, int idx, int outfd);
/* extract one entry to path (creates parents). Path-traversal safe. */
int      k93_zip_extract(const k93_zip *z, int idx, const char *destpath);
/* extract every entry under root dir. Returns entries written or -1. */
int      k93_zip_extract_all(const k93_zip *z, const char *rootdir);
/* find first entry whose name equals or ends with suffix; -1 if none */
int      k93_zip_find(const k93_zip *z, const char *suffix);

#endif
