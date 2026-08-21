#ifndef ATA_H
#define ATA_H

#include <stdint.h>

/* Auto-detect the first available non-CDROM IDE disk.
 * Must be called before ata_read_sector(). */
void ata_detect(void);

/* Read one 512-byte sector from the detected IDE disk (LBA28 PIO).
 * Returns 0 on success, -1 on error/timeout/no-disk. */
int ata_read_sector(uint32_t lba, uint8_t *buf);

/* Write one 512-byte sector to the detected IDE disk (LBA28 PIO).
 * Returns 0 on success, -1 on error/timeout/no-disk. */
int ata_write_sector(uint32_t lba, const uint8_t *buf);

/* Write all dirty cached sectors back to disk (used by fsync, periodic
 * flush and panic).  Returns 0 on success, -1 on error/no-disk. */
int ata_sync(void);

/* Number of dirty sectors still waiting to be flushed */
uint32_t ata_dirty_count(void);

#endif
