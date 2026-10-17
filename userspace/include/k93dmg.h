/* k93dmg.h - Apple Disk Image (.dmg) reader, clean-room.
 * Supports UDZO (zlib-compressed blocks), UDIF 'koly' trailer, mish block
 * tables, 'raw'/'zero'/'ignore' blocks. Ceiling: only single-file UDZO/UDIF
 * images; sparse bundles (.sparsebundle) and encrypted images unsupported. */
#ifndef K93DMG_H
#define K93DMG_H
#include <stddef.h>
#include <stdint.h>

typedef struct k93_dmg k93_dmg;

k93_dmg *k93_dmg_open(const char *path);
void     k93_dmg_close(k93_dmg *d);
/* total uncompressed size in bytes */
uint64_t k93_dmg_size(const k93_dmg *d);
/* write the full raw image to an open fd; returns 0 on success.
 * zero runs are punched as holes (sparse) when the fd supports seeking. */
int      k93_dmg_extract(k93_dmg *d, int outfd);
/* quick sanity: 1 if file starts with UDIF koly trailer and parses */
int      k93_dmg_probe(const char *path);

#endif
