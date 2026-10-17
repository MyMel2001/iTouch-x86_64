/* k93gpt.h - minimal GPT (GUID Partition Table) writer, clean-room.
 * Used by InstallToHD to prepare a target disk: 1 MiB alignment, one EFI
 * system partition is intentionally omitted (this OS boots via the firmware
 * bootloader chain documented in README), a swap partition and a root
 * partition. */
#ifndef K93GPT_H
#define K93GPT_H
#include <stdint.h>

typedef struct {
    uint64_t start_sector;
    uint64_t sector_count;
    char     name[72];                  /* UTF-16LE is produced internally */
    const uint8_t *type_guid;           /* NULL for Linux filesystem GUID */
} k93_gpt_part;

extern const uint8_t k93_guid_linux_fs[16];
extern const uint8_t k93_guid_linux_swap[16];

int k93_gpt_write(int fd, uint32_t sector_size, uint64_t total_sectors,
                  const k93_gpt_part *parts, int nparts);
/* verify the GPT we (or someone) wrote; 0 = valid */
int k93_gpt_verify(int fd, uint32_t sector_size);

#endif
