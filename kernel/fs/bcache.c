/*
 * BCache — write-back block cache for the ATA disk
 *
 * All sector traffic (FAT, directory clusters, file data) goes through
 * this cache.  Writes only mark the page dirty; the actual disk write
 * happens later on:
 *
 *   - fsync() syscall  -> bcache_flush()
 *   - periodic worker  -> bcache_flush() (scheduled by the timer)
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
#include <task.h>
#include <wait.h>

/* Raw disk hooks installed by the ATA driver (bypass the cache) */
static int (*raw_read)(uint32_t lba, uint8_t *buf)  = NULL;
static int (*raw_write)(uint32_t lba, const uint8_t *buf) = NULL;

static bcache_entry_t cache[BCACHE_ENTRIES];
static uint32_t next_victim = 0;
static spinlock_irq_t cache_lock = { SPINLOCK_INIT, 0 };
static bool cache_ready = false;

/* Acordada a cada transição busy->0: destrava quem espera vítima livre
 * quando todas as 64 estão inflight. */
static wait_queue_head_t bcache_wait;
static volatile uint32_t completions = 0;

void bcache_init(void)
{
    if (cache_ready) return;
    for (int i = 0; i < BCACHE_ENTRIES; i++) {
        cache[i].valid = 0;
        cache[i].dirty = 0;
        cache[i].lba   = 0;
        cache[i].busy  = 0;
        init_waitqueue_head(&cache[i].wait);
    }
    next_victim = 0;
    init_waitqueue_head(&bcache_wait);
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

/* Escolhe vítima pulando entries com I/O inflight. Com lock preso.
 * Retorna NULL se as 64 estiverem busy (chamador espera bcache_wait). */
static bcache_entry_t *bcache_claim_victim_locked(void)
{
    for (int i = 0; i < BCACHE_ENTRIES; i++) {
        bcache_entry_t *e = &cache[next_victim];
        next_victim = (next_victim + 1) % BCACHE_ENTRIES;
        if (!e->busy)
            return e;
    }
    return NULL;
}

/* Com lock preso. Libera o dono do busy e acorda espera do LBA + espera
 * global de vítima. O chamador já instalou/restauraram o estado. */
static void bcache_complete_locked(bcache_entry_t *e)
{
    e->busy = 0;
    completions++;
    wake_up(&e->wait);
    wake_up(&bcache_wait);
}

int bcache_read(uint32_t lba, uint8_t *out)
{
    if (!cache_ready || !raw_read) return -1;
    if (!out) return -1;

    /* Buffers de I/O na stack: o disco nunca é tocado com o lock preso,
     * então o timer (e outro usuário do cache) progridem no meio do I/O. */
    uint8_t io_buf[512];
    uint8_t victim_buf[512];

    for (;;) {
        unsigned long flags;
        spin_lock_irqsave(&cache_lock, &flags);

        bcache_entry_t *e = bcache_find(lba);
        if (e && !e->busy) {
            for (int i = 0; i < 512; i++) out[i] = e->data[i];
            spin_unlock_irqrestore(&cache_lock, flags);
            return 0;
        }
        if (e) {
            /* Mesmo LBA com load inflight: espera a conclusão em vez de
             * carregar em duplicata. Entry é array estático — o ponteiro
             * continua válido através do sleep. */
            spin_unlock_irqrestore(&cache_lock, flags);
            wait_event(e->wait, !e->busy);
            continue;
        }

        e = bcache_claim_victim_locked();
        if (!e) {
            uint32_t gen = completions;
            spin_unlock_irqrestore(&cache_lock, flags);
            wait_event(bcache_wait, completions != gen);
            continue;
        }

        uint32_t old_lba = e->lba;
        uint8_t old_valid = e->valid;
        uint8_t old_dirty = e->dirty;
        if (old_dirty) {
            for (int i = 0; i < 512; i++) victim_buf[i] = e->data[i];
        }
        e->lba = lba;
        e->valid = 0;
        e->dirty = 0;
        e->busy = 1;
        spin_unlock_irqrestore(&cache_lock, flags);

        /* I/O real, sem lock e com IRQs ligadas. */
        if (old_dirty && raw_write(old_lba, victim_buf) != 0) {
            spin_lock_irqsave(&cache_lock, &flags);
            e->lba = old_lba;
            e->valid = old_valid;
            e->dirty = 1;   /* writeback falhou: tenta de novo depois */
            bcache_complete_locked(e);
            spin_unlock_irqrestore(&cache_lock, flags);
            return -1;
        }
        if (raw_read(lba, io_buf) != 0) {
            spin_lock_irqsave(&cache_lock, &flags);
            e->valid = 0;
            e->dirty = 0;
            bcache_complete_locked(e);
            spin_unlock_irqrestore(&cache_lock, flags);
            return -1;
        }

        spin_lock_irqsave(&cache_lock, &flags);
        for (int i = 0; i < 512; i++) {
            e->data[i] = io_buf[i];
            out[i] = io_buf[i];
        }
        e->valid = 1;
        e->dirty = 0;
        bcache_complete_locked(e);
        spin_unlock_irqrestore(&cache_lock, flags);
        return 0;
    }
}

int bcache_write(uint32_t lba, const uint8_t *data)
{
    if (!cache_ready || !raw_write) return -1;
    if (!data) return -1;

    uint8_t victim_buf[512];

    for (;;) {
        unsigned long flags;
        spin_lock_irqsave(&cache_lock, &flags);

        bcache_entry_t *e = bcache_find(lba);
        if (e && e->busy) {
            spin_unlock_irqrestore(&cache_lock, flags);
            wait_event(e->wait, !e->busy);
            continue;
        }
        if (e) {
            for (int i = 0; i < 512; i++) e->data[i] = data[i];
            e->valid = 1;
            e->dirty = 1;
            spin_unlock_irqrestore(&cache_lock, flags);
            return 0;
        }

        e = bcache_claim_victim_locked();
        if (!e) {
            uint32_t gen = completions;
            spin_unlock_irqrestore(&cache_lock, flags);
            wait_event(bcache_wait, completions != gen);
            continue;
        }

        uint32_t old_lba = e->lba;
        uint8_t old_valid = e->valid;
        uint8_t old_dirty = e->dirty;
        if (old_dirty) {
            for (int i = 0; i < 512; i++) victim_buf[i] = e->data[i];
        }
        e->lba = lba;
        e->valid = 0;
        e->dirty = 0;
        e->busy = 1;
        spin_unlock_irqrestore(&cache_lock, flags);

        if (old_dirty && raw_write(old_lba, victim_buf) != 0) {
            spin_lock_irqsave(&cache_lock, &flags);
            e->lba = old_lba;
            e->valid = old_valid;
            e->dirty = 1;
            bcache_complete_locked(e);
            spin_unlock_irqrestore(&cache_lock, flags);
            return -1;
        }

        spin_lock_irqsave(&cache_lock, &flags);
        for (int i = 0; i < 512; i++) e->data[i] = data[i];
        e->valid = 1;
        e->dirty = 1;
        bcache_complete_locked(e);
        spin_unlock_irqrestore(&cache_lock, flags);
        return 0;
    }
}

int bcache_flush(void)
{
    if (!cache_ready) return 0;
    if (!raw_write) return -1;

    int result = 0;
    uint8_t io_buf[512];

    /* Uma entry por vez: reclama (busy) sob lock, escreve fora do lock.
     * Entry busy com dono é pulada — o dono instala dado mais novo e o
     * próximo flush a leva (flush concorrente não perde update). */
    for (int i = 0; i < BCACHE_ENTRIES; i++) {
        uint32_t lba = 0;
        int need = 0;

        unsigned long flags;
        spin_lock_irqsave(&cache_lock, &flags);
        if (cache[i].dirty && !cache[i].busy) {
            cache[i].busy = 1;
            lba = cache[i].lba;
            for (int j = 0; j < 512; j++) io_buf[j] = cache[i].data[j];
            need = 1;
        }
        spin_unlock_irqrestore(&cache_lock, flags);

        if (!need)
            continue;
        int rc = raw_write(lba, io_buf);

        spin_lock_irqsave(&cache_lock, &flags);
        /* busy ainda é nosso (invalidate pula busy; writer espera no
         * busy) — só confirma geração pelo lba. */
        if (cache[i].busy && cache[i].lba == lba) {
            if (rc == 0)
                cache[i].dirty = 0;
            else
                result = -1;   /* mantém dirty p/ retry */
        } else if (rc != 0) {
            result = -1;
        }
        bcache_complete_locked(&cache[i]);
        spin_unlock_irqrestore(&cache_lock, flags);
    }

    return result;
}

int bcache_flush_lba(uint32_t lba)
{
    if (!cache_ready) return 0;
    if (!raw_write) return -1;

    uint8_t io_buf[512];
    int need = 0;

    unsigned long flags;
    spin_lock_irqsave(&cache_lock, &flags);
    bcache_entry_t *e = bcache_find(lba);
    if (e && e->dirty && !e->busy) {
        e->busy = 1;
        for (int j = 0; j < 512; j++) io_buf[j] = e->data[j];
        need = 1;
    }
    int already_clean = (e && !e->dirty);
    spin_unlock_irqrestore(&cache_lock, flags);

    if (already_clean || !e)
        return 0;
    if (!need) {
        /* Dono vai instalar (talvez dirty de novo): espera e reporta o
         * estado final sem forçar outro write. */
        wait_event(e->wait, !e->busy);
        spin_lock_irqsave(&cache_lock, &flags);
        bcache_entry_t *e2 = bcache_find(lba);
        int rc = (e2 && e2->dirty) ? -1 : 0;
        spin_unlock_irqrestore(&cache_lock, flags);
        return rc;
    }
    int result = (raw_write(lba, io_buf) == 0) ? 0 : -1;

    spin_lock_irqsave(&cache_lock, &flags);
    if (e->busy && e->lba == lba && result == 0)
        e->dirty = 0;
    if (result != 0)
        result = -1;
    bcache_complete_locked(e);
    spin_unlock_irqrestore(&cache_lock, flags);
    return result;
}

/* Drop a cached sector (used to discard the initial settle read).
 * Entries com I/O inflight são preservadas: descartar a reserva de outro
 * dono corromperia o install; o dado que chega do disco é fresco mesmo. */
void bcache_invalidate_lba(uint32_t lba)
{
    if (!cache_ready) return;

    unsigned long flags;
    spin_lock_irqsave(&cache_lock, &flags);

    bcache_entry_t *e = bcache_find(lba);
    if (e && !e->busy) {
        e->valid = 0;
        e->dirty = 0;
    }

    spin_unlock_irqrestore(&cache_lock, flags);
}

uint32_t bcache_dirty_count(void)
{
    uint32_t count = 0;
    unsigned long flags;

    spin_lock_irqsave(&cache_lock, &flags);
    for (int i = 0; i < BCACHE_ENTRIES; i++) {
        if (cache[i].dirty) count++;
    }
    spin_unlock_irqrestore(&cache_lock, flags);
    return count;
}

/* ------------------------------------------------------------------ */
/* Stress de boot: dois readers concorrentes no mesmo range, com I/O  */
/* fora do lock e preempção de 100 Hz. Leituras não alteram o disco,  */
/* então qualquer LBA serve; o dado deve ser estável entre as tasks.  */
/* Polling limitado (sem hang) + SKIP honesto sem disco.              */
/* ------------------------------------------------------------------ */

#define BCACHE_STRESS_LBAS  32
#define BCACHE_STRESS_BASE  1024

static uint8_t stress_buf_a[BCACHE_STRESS_LBAS][512];
static uint8_t stress_buf_b[BCACHE_STRESS_LBAS][512];
static volatile int stress_a_done;
static volatile int stress_b_done;

static void bcache_stress_reader_a(void) {
    for (int i = 0; i < BCACHE_STRESS_LBAS; i++) {
        if (bcache_read(BCACHE_STRESS_BASE + i, stress_buf_a[i]) != 0) {
            stress_buf_a[i][0] = 0xEE;
            break;
        }
    }
    stress_a_done = 1;
}

static void bcache_stress_reader_b(void) {
    for (int i = 0; i < BCACHE_STRESS_LBAS; i++) {
        if (bcache_read(BCACHE_STRESS_BASE + i, stress_buf_b[i]) != 0) {
            stress_buf_b[i][0] = 0xEE;
            break;
        }
    }
    stress_b_done = 1;
}

static int bcache_stress_join(volatile int *flag) {
    unsigned it = 2000000;
    while (it-- && !*flag)
        schedule();
    return *flag;
}

void bcache_stress_test(void) {
    serial_print("[BCACHE] stress: 2 readers x 32 LBAs concorrentes...\n");

    /* Sem disco não há o que exercitar: SKIP, não FAIL. */
    uint8_t probe[512];
    for (int i = 0; i < BCACHE_STRESS_LBAS; i++)
        bcache_invalidate_lba(BCACHE_STRESS_BASE + i);
    if (bcache_read(BCACHE_STRESS_BASE, probe) != 0) {
        serial_print("[BCACHE] SKIP (sem disco)\n");
        return;
    }
    for (int i = 1; i < BCACHE_STRESS_LBAS; i++)
        bcache_invalidate_lba(BCACHE_STRESS_BASE + i);

    stress_a_done = 0;
    stress_b_done = 0;
    task_struct_t *ta = task_create(bcache_stress_reader_a, 0);
    task_struct_t *tb = task_create(bcache_stress_reader_b, 0);
    if (!ta || !tb) {
        serial_print("[BCACHE] FAIL: spawn\n");
        return;
    }

    int ok = bcache_stress_join(&stress_a_done) &&
             bcache_stress_join(&stress_b_done);
    /* Estabilidade: mesmo LBA, duas tasks, bytes idênticos. */
    for (int i = 0; ok && i < BCACHE_STRESS_LBAS; i++) {
        for (int j = 0; j < 512; j++) {
            if (stress_buf_a[i][j] != stress_buf_b[i][j]) {
                ok = 0;
                break;
            }
        }
    }
    /* Sem sujeira residual e flush limpo. */
    ok = ok && (bcache_flush() == 0) && (bcache_dirty_count() == 0);

    if (ok)
        serial_print("[BCACHE] ALL CHECKS PASSED (concorrência + flush limpo)\n");
    else
        serial_print("[BCACHE] TESTS FAILED\n");
}
