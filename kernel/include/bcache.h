#ifndef BCACHE_H
#define BCACHE_H

#include <stdint.h>
#include <wait.h>

/* Number of 512-byte sectors kept in the write-back cache */
#define BCACHE_ENTRIES 64

typedef struct bcache_entry {
    uint32_t lba;
    uint8_t  data[512];
    uint8_t  valid;
    uint8_t  dirty;
    /* busy=1: entry reclamada com I/O inflight FORA do lock (ou ponta em
     * transição). O campo lba já pertence ao novo dono. Concorrentes no
     * mesmo LBA esperam em wait em vez de carregar em duplicata; a escolha
     * de vítima pula entries busy. Protegido por cache_lock. */
    uint8_t  busy;
    wait_queue_head_t wait;
} bcache_entry_t;

/* Install the raw (cache-bypassing) disk I/O hooks used to fill/flush
 * cache pages.  Called by the ATA driver during detection. */
void bcache_register_driver(int (*read_fn)(uint32_t, uint8_t *),
                            int (*write_fn)(uint32_t, const uint8_t *));

/* Initialize the block cache (idempotent) */
void bcache_init(void);

/* Read one sector through the cache (0 on success, -1 on error) */
int bcache_read(uint32_t lba, uint8_t *out);

/* Write one sector through the cache — marks the page dirty, does not
 * touch the disk until a flush.  (0 on success, -1 on error) */
int bcache_write(uint32_t lba, const uint8_t *data);

/* Write all dirty pages back to disk.  Returns 0 on success, -1 on error. */
int bcache_flush(void);

/* Force one specific sector to disk (clears its dirty bit). */
int bcache_flush_lba(uint32_t lba);

/* Drop a cached sector without writing it back (discard) */
void bcache_invalidate_lba(uint32_t lba);

/* Number of dirty pages currently waiting to be flushed */
uint32_t bcache_dirty_count(void);

/* Boot stress: two concurrent readers over the same LBA range (serial
 * SKIP without disk). */
void bcache_stress_test(void);

#endif
