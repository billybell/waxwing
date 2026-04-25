// Glue between FatFS's diskio interface and the RP2040 QSPI flash.
//
// FatFS thinks in 512-byte sectors. The RP2040 flash erase unit is
// FLASH_SECTOR_SIZE (4096 bytes) and the program unit is
// FLASH_PAGE_SIZE (256 bytes), so each 512-byte FatFS write boils down
// to:
//   1. read the surrounding 4 KiB flash sector into RAM
//   2. patch the 512 bytes the host wants to update
//   3. erase the 4 KiB flash sector
//   4. program the patched 4 KiB back, page by page
//
// flash_range_erase and flash_range_program disable the XIP cache and
// must run with interrupts disabled, otherwise an IRQ handler resident
// in flash will fault during the program. The CYW43 driver runs in
// poll mode here, so disabling IRQs while we hold the bus is safe.

#include <stdint.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include "fatfs/ff.h"
#include "fatfs/diskio.h"   // FatFS's BYTE/UINT/LBA_t typedefs and prototypes
#include "diskio.h"

#define FLASH_PDRV 0

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uintptr_t fs_sector_to_flash_offset(uint32_t fs_sector) {
    return (uintptr_t)FLASH_FS_OFFSET_BYTES + (uintptr_t)fs_sector * DISK_SECTOR_SIZE;
}

// XIP-mapped pointer into the flash partition, for memcpy reads.
static const uint8_t *fs_sector_to_xip_ptr(uint32_t fs_sector) {
    return (const uint8_t *)(XIP_BASE + fs_sector_to_flash_offset(fs_sector));
}

// Erase the 4 KiB flash sector that contains `flash_offset` and re-program
// it from the contents of `block` (which must be FLASH_SECTOR_SIZE bytes).
// Caller-provided buffer keeps this alloc-free.
static void rewrite_4k_block(uint32_t flash_offset, const uint8_t *block) {
    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(flash_offset, FLASH_SECTOR_SIZE);
    flash_range_program(flash_offset, block, FLASH_SECTOR_SIZE);
    restore_interrupts(irq);
}

// ---------------------------------------------------------------------------
// Public init helper
// ---------------------------------------------------------------------------

bool disk_init(void) {
    return true;
}

// ---------------------------------------------------------------------------
// FatFS diskio interface
// ---------------------------------------------------------------------------

DSTATUS disk_initialize(BYTE pdrv) {
    return (pdrv == FLASH_PDRV) ? 0 : STA_NOINIT;
}

DSTATUS disk_status(BYTE pdrv) {
    return (pdrv == FLASH_PDRV) ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != FLASH_PDRV) return RES_PARERR;
    if ((uint32_t)sector + count > FLASH_FS_TOTAL_SECTORS) return RES_PARERR;

    memcpy(buff, fs_sector_to_xip_ptr((uint32_t)sector),
           (size_t)count * DISK_SECTOR_SIZE);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != FLASH_PDRV) return RES_PARERR;
    if ((uint32_t)sector + count > FLASH_FS_TOTAL_SECTORS) return RES_PARERR;

    // We coalesce writes into 4 KiB flash-sector-sized rewrites. Walk the
    // requested range in chunks bounded by the erase block.
    static uint8_t scratch[FLASH_SECTOR_SIZE];
    const uint32_t sectors_per_block = FLASH_SECTOR_SIZE / DISK_SECTOR_SIZE;

    uint32_t cur = (uint32_t)sector;
    uint32_t end = cur + count;
    const uint8_t *src = buff;

    while (cur < end) {
        // Find the flash-sector-aligned base for the FatFS sector we're at.
        uint32_t block_base = cur - (cur % sectors_per_block);
        uint32_t in_block_off = cur - block_base;
        uint32_t can_take = sectors_per_block - in_block_off;
        uint32_t take = (end - cur < can_take) ? (end - cur) : can_take;

        // Read the 4 KiB block, patch in the new sectors, write it back.
        memcpy(scratch, fs_sector_to_xip_ptr(block_base), FLASH_SECTOR_SIZE);
        memcpy(scratch + in_block_off * DISK_SECTOR_SIZE, src, take * DISK_SECTOR_SIZE);

        uint32_t flash_offset = (uint32_t)fs_sector_to_flash_offset(block_base);
        rewrite_4k_block(flash_offset, scratch);

        cur += take;
        src += take * DISK_SECTOR_SIZE;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != FLASH_PDRV) return RES_PARERR;
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT: {
            *(LBA_t *)buff = FLASH_FS_TOTAL_SECTORS;
            return RES_OK;
        }
        case GET_SECTOR_SIZE: {
            *(WORD *)buff = DISK_SECTOR_SIZE;
            return RES_OK;
        }
        case GET_BLOCK_SIZE: {
            // Erase block size in units of disk sectors.
            *(DWORD *)buff = FLASH_SECTOR_SIZE / DISK_SECTOR_SIZE;
            return RES_OK;
        }
        default:
            return RES_PARERR;
    }
}
