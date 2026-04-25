#ifndef WAXWING_DISKIO_H
#define WAXWING_DISKIO_H

#include <stdint.h>
#include <stdbool.h>

// FatFS sector size. We expose the underlying flash to FatFS in 512-byte
// chunks; the RP2040 flash erase block is 4096 bytes, so each FatFS
// sector write requires a read-modify-erase-program cycle of an 8-sector
// block. Diskio does the bookkeeping so FatFS doesn't have to know.
#define DISK_SECTOR_SIZE 512

// Flash partition reserved for FatFS, expressed in bytes from the start
// of the QSPI flash chip (XIP base 0x10000000).
//
// Layout on the 2 MiB Pico W flash:
//   0x000000 .. 0x13FFFF  firmware (1.25 MiB)
//   0x140000 .. 0x1EFFFF  FatFS partition (704 KiB)
//   0x1F0000 .. 0x1FFFFF  reserved (BTstack TLV / headroom, 64 KiB)
//
// If the firmware grows past 1.25 MiB the linker will fail to fit it
// before the FS partition; bump FLASH_FS_OFFSET_BYTES at that point.
#define FLASH_FS_OFFSET_BYTES   0x00140000u
#define FLASH_FS_SIZE_BYTES     0x000B0000u   // 704 KiB

#define FLASH_FS_TOTAL_SECTORS  (FLASH_FS_SIZE_BYTES / DISK_SECTOR_SIZE)

bool disk_init(void);

#endif // WAXWING_DISKIO_H
