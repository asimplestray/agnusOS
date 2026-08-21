/*
 * BCache — write-back block cache for the ATA disk
 *
 * All sector traffic (FAT, directory clusters, file data) goes through
 * this cache.  Writes only mark the page dirty; the actual disk write
 * happens later on:
 *
 *   - fsync() syscall  -> bcache_flush()
 *   - periodic timer   -> bcache_flush() (every few seconds)
 *   - kernel panic     -> bcache_flush() (before halting)
 *
 * This protects the FAT32 volume against corruption after a reboot: a
 * crash only loses the last few seconds of unwritten data instead of
 * leaving half-written FAT/directory sectors.
 */

#include <bcache.h>
#include <spinlock.h>
#include <stddef.h>
#include <stdbool.h>

/* Raw disk hooks installed by the ATA driver (bypass the cache) */
static int (*raw_read)(uint32_t lba, uint8_t *buf)  = NULL;
static int (*raw_write)(uint32_t lba, const uint8_t *buf) = NULL;

static bcache_entry_t cache[BCACHE_ENTRIES];
static uint32_t next_victim = 0;
static spinlock_irq_t cache_lock = { SPINLOCK_INIT, 0 };
static bool cache_ready = false;

void bcache_init(void)
{
    if (cache_ready) return;
    for (int i = 0; i < BCACHE_ENTRIES; i++) {
        cache[i].valid = 0;
        cache[i].dirty = 0;
        cache[i].lba   = 0;
    }
    next_victim = 0;
    cache_ready = true;
}

/* ATA driver calls this at detect time to register the raw I/O hooks */
void bcache_register_driver(int (*read_fn)(uint32_t, uint8_t *),
                            int (*write_fn)(uint32_t, const uint8_t *))
{
    raw_read  = read_fn;
    raw_write = write_fn;
}

static bcache_entry_t *bcache_find(uint32_t lba)
{
    for (int i = 0; i < BCACHE_ENTRIES; i++) {
        if (cache[i].valid && cache[i].lba == lba)
            return &cache[i];
    }
    return NULL;
}

int bcache_read(uint32_t lba, uint8_t *out)
{
    if (!cache_ready || !raw_read) return -1;
    if (!out) return -1;

    unsigned long flags;
    spin_lock_irqsave(&cache_lock, &flags);

    bcache_entry_t *e = bcache_find(lba);
    if (e) {
        for (int i = 0; i < 512; i++) out[i] = e->data[i];
        spin_unlock_irqrestore(&cache_lock, flags);
        return 0;
    }

    /* Miss: evict a slot (flushing it first if dirty), then load from disk */
    e = &cache[next_victim];
    next_victim = (next_victim + 1) % BCACHE_ENTRIES;

    if (e->dirty && raw_write(e->lba, e->data) != 0) {
        spin_unlock_irqrestore(&cache_lock, flags);
        return -1;
    }

    if (raw_read(lba, e->data) != 0) {
        e->valid = 0;
        e->dirty = 0;
        spin_unlock_irqrestore(&cache_lock, flags);
        return -1;
    }

    e->lba   = lba;
    e->valid = 1;
    e->dirty = 0;
    for (int i = 0; i < 512; i++) out[i] = e->data[i];

    spin_unlock_irqrestore(&cache_lock, flags);
    return 0;
}

int bcache_write(uint32_t lba, const uint8_t *data)
{
    if (!cache_ready || !raw_write) return -1;
    if (!data) return -1;

    unsigned long flags;
    spin_lock_irqsave(&cache_lock, &flags);

    bcache_entry_t *e = bcache_find(lba);
    if (!e) {
        /* Evict a slot; a dirty victim is written back first */
        e = &cache[next_victim];
        next_victim = (next_victim + 1) % BCACHE_ENTRIES;

        if (e->dirty && raw_write(e->lba, e->data) != 0) {
            spin_unlock_irqrestore(&cache_lock, flags);
            return -1;
        }
        e->lba = lba;
    }

    for (int i = 0; i < 512; i++) e->data[i] = data[i];
    e->valid = 1;
    e->dirty = 1;

    spin_unlock_irqrestore(&cache_lock, flags);
    return 0;
}

int bcache_flush(void)
{
    if (!cache_ready) return 0;
    if (!raw_write) return -1;

    int result = 0;
    unsigned long flags;
    spin_lock_irqsave(&cache_lock, &flags);

    for (int i = 0; i < BCACHE_ENTRIES; i++) {
        if (cache[i].dirty) {
            if (raw_write(cache[i].lba, cache[i].data) != 0) {
                result = -1;   /* keep dirty bit so we retry later */
            } else {
                cache[i].dirty = 0;
            }
        }
    }

    spin_unlock_irqrestore(&cache_lock, flags);
    return result;
}

int bcache_flush_lba(uint32_t lba)
{
    if (!cache_ready) return 0;

    unsigned long flags;
    spin_lock_irqsave(&cache_lock, &flags);

    bcache_entry_t *e = bcache_find(lba);
    if (!e || !e->dirty) {
        spin_unlock_irqrestore(&cache_lock, flags);
        return 0;
    }

    int result = (raw_write && raw_write(lba, e->data) == 0) ? 0 : -1;
    if (result == 0) e->dirty = 0;

    spin_unlock_irqrestore(&cache_lock, flags);
    return result;
}

/* Drop a cached sector (used to discard the initial settle read) */
void bcache_invalidate_lba(uint32_t lba)
{
    if (!cache_ready) return;

    unsigned long flags;
    spin_lock_irqsave(&cache_lock, &flags);

    bcache_entry_t *e = bcache_find(lba);
    if (e) {
        e->valid = 0;
        e->dirty = 0;
    }

    spin_unlock_irqrestore(&cache_lock, flags);
}

uint32_t bcache_dirty_count(void)
{
    uint32_t count = 0;
    for (int i = 0; i < BCACHE_ENTRIES; i++) {
        if (cache[i].dirty) count++;
    }
    return count;
}
