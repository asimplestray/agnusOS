#include <pmm.h>
#include <multiboot2.h>
#include <screen.h>
#include <serial.h>
#include <spinlock.h>

extern char _kernel_start[];
extern char _kernel_end[];

static uint8_t* pmm_bitmap = 0;
static uint64_t total_pages = 0;
static uint64_t free_pages = 0;
static uint64_t total_memory = 0;
static uint64_t bitmap_size = 0;
static spinlock_irq_t pmm_lock = { SPINLOCK_INIT, 0 };

/* Permanently reserved ranges captured at init. pmm_free_block() must
 * never release these, even if the address is page-aligned. */
#define PMM_MAX_RESERVED 16
static struct { uint64_t start, end; } pmm_reserved[PMM_MAX_RESERVED];
static int pmm_reserved_count = 0;

/* Debug counters: silent in the hot path, observable via getter. */
static uint64_t pmm_err_double = 0;
static uint64_t pmm_err_invalid = 0;
static uint64_t pmm_err_reserved = 0;

#define ALIGN_4K(addr) (((addr) + 4095) & ~4095ULL)

/* Overflow-safe add for u64 ranges. Returns false on overflow. */
static inline int u64_add_ok(uint64_t a, uint64_t b, uint64_t *out) {
    if (UINT64_MAX - a < b) return 0;
    *out = a + b;
    return 1;
}

static void pmm_track_reserved(uint64_t start, uint64_t len) {
    uint64_t end;
    if (!len || !u64_add_ok(start, len, &end)) return;
    /* Page-align outward so partial pages stay reserved. */
    start &= ~(uint64_t)(PAGE_SIZE - 1);
    end = (end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (end <= start) return;
    if (pmm_reserved_count >= PMM_MAX_RESERVED) return;
    pmm_reserved[pmm_reserved_count].start = start;
    pmm_reserved[pmm_reserved_count].end = end;
    pmm_reserved_count++;
}

int pmm_is_reserved(uint64_t addr) {
    uint64_t page_start = addr & ~(uint64_t)(PAGE_SIZE - 1);
    for (int i = 0; i < pmm_reserved_count; i++) {
        if (page_start >= pmm_reserved[i].start &&
            page_start < pmm_reserved[i].end)
            return 1;
    }
    return 0;
}

// Helper functions to get/set bits in the physical memory bitmap
static inline void bitmap_set(uint64_t page) {
    if (page >= total_pages) return;
    if (!(pmm_bitmap[page / 8] & (1 << (page % 8)))) {
        pmm_bitmap[page / 8] |= (1 << (page % 8));
        if (free_pages > 0) free_pages--;
    }
}

static inline void bitmap_clear(uint64_t page) {
    if (page >= total_pages) return;
    if (pmm_bitmap[page / 8] & (1 << (page % 8))) {
        pmm_bitmap[page / 8] &= ~(1 << (page % 8));
        free_pages++;
    }
}

static inline int bitmap_test(uint64_t page) {
    if (page >= total_pages) return 1; // Out of bounds is considered "used"
    return (pmm_bitmap[page / 8] & (1 << (page % 8))) != 0;
}

// Mark a range of physical memory as used/reserved
static void pmm_reserve_region(uint64_t start_addr, uint64_t length) {
    uint64_t end;
    if (!length || !u64_add_ok(start_addr, length, &end)) return;
    uint64_t start_page = start_addr / PAGE_SIZE;
    uint64_t end_page = (end + PAGE_SIZE - 1) / PAGE_SIZE;
    if (end_page <= start_page) return;
    if (start_page >= total_pages) return;
    if (end_page > total_pages) end_page = total_pages;
    for (uint64_t p = start_page; p < end_page; p++) {
        bitmap_set(p);
    }
}

// Mark a range of physical memory as free/available
static void pmm_free_region(uint64_t start_addr, uint64_t length) {
    uint64_t end;
    if (!length || !u64_add_ok(start_addr, length, &end)) return;
    /* Only free complete pages: round start up, end down. */
    uint64_t first = (start_addr + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t last = end / PAGE_SIZE; /* exclusive */
    if (last <= first) return;
    if (first >= total_pages) return;
    if (last > total_pages) last = total_pages;
    for (uint64_t p = first; p < last; p++) {
        bitmap_clear(p);
    }
}

void pmm_init(uint64_t mbi_addr) {
    spinlock_init(&pmm_lock.lock);
    if (!mbi_addr) {
        screen_log("FAIL", COLOR_LIGHT_RED, "PMM: null multiboot info!");
        while (1);
    }
    uint32_t mbi_size = *(volatile uint32_t*)mbi_addr;
    if (mbi_size < 8 || mbi_size > (1u << 20)) {
        screen_log("FAIL", COLOR_LIGHT_RED, "PMM: multiboot size invalido!");
        while (1);
    }

    struct multiboot_tag* tag = (struct multiboot_tag*)(mbi_addr + 8);
    struct multiboot_tag_mmap* mmap_tag = 0;

    // 1. Locate the Memory Map tag, with a hard iteration cap so a corrupt
    //    tag list cannot hang the boot.
    for (int steps = 0; steps < 64; steps++) {
        if ((uint64_t)tag < mbi_addr || (uint64_t)tag + sizeof(*tag) > mbi_addr + mbi_size)
            break;
        if (tag->type == MULTIBOOT_TAG_TYPE_END) break;
        if (tag->size < 8 || (uint64_t)tag + tag->size > mbi_addr + mbi_size)
            break;
        if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
            mmap_tag = (struct multiboot_tag_mmap*)tag;
            break;
        }
        tag = (struct multiboot_tag*)((uintptr_t)tag + ((tag->size + 7) & ~7));
    }

    if (!mmap_tag) {
        screen_log("FAIL", COLOR_LIGHT_RED, "No Multiboot2 memory map tag found!");
        while (1);
    }
    if (mmap_tag->entry_size < sizeof(struct multiboot_mmap_entry) ||
        mmap_tag->entry_size > 256 ||
        mmap_tag->size < sizeof(struct multiboot_tag_mmap) ||
        mmap_tag->size > mbi_size) {
        screen_log("FAIL", COLOR_LIGHT_RED, "PMM: mmap tag invalida!");
        while (1);
    }

    // 2. Discover total memory with overflow-safe max tracking.
    uint64_t max_addr = 0;
    uint64_t mmap_entries_count =
        (mmap_tag->size - sizeof(struct multiboot_tag_mmap)) / mmap_tag->entry_size;
    if (mmap_entries_count > 256) mmap_entries_count = 256;

    for (uint64_t i = 0; i < mmap_entries_count; i++) {
        struct multiboot_mmap_entry* entry =
            (struct multiboot_mmap_entry*)((uintptr_t)mmap_tag->entries + i * mmap_tag->entry_size);
        uint64_t end;
        if (!u64_add_ok(entry->addr, entry->len, &end)) continue; /* corrupt entry */
        if (entry->type == MULTIBOOT_MEMORY_AVAILABLE && entry->len) {
            if (end > max_addr) max_addr = end;
        }
    }
    if (!max_addr) {
        screen_log("FAIL", COLOR_LIGHT_RED, "PMM: nenhuma RAM utilizavel!");
        while (1);
    }

    total_memory = max_addr;
    total_pages = total_memory / PAGE_SIZE;
    bitmap_size = (total_pages + 7) / 8; /* round up, old code truncated */
    free_pages = 0;

    // 3. Position the PMM bitmap after kernel, mbi AND all multiboot modules (initrd) to prevent overwrite!
    uint64_t kernel_end_addr = (uint64_t)_kernel_end;
    uint64_t mbi_end;
    if (!u64_add_ok(mbi_addr, mbi_size, &mbi_end)) mbi_end = kernel_end_addr;
    uint64_t safe_start = (kernel_end_addr > mbi_end) ? kernel_end_addr : mbi_end;
    // Also consider initrd/modules end (commit c32c427 grew kernel close to 0x177000)
    {
        struct multiboot_tag *t = (struct multiboot_tag *)(mbi_addr + 8);
        for (int steps = 0; steps < 64; steps++) {
            if ((uint64_t)t < mbi_addr || (uint64_t)t + sizeof(*t) > mbi_addr + mbi_size)
                break;
            if (t->type == MULTIBOOT_TAG_TYPE_END) break;
            if (t->size < 8 || (uint64_t)t + t->size > mbi_addr + mbi_size) break;
            if (t->type == MULTIBOOT_TAG_TYPE_MODULE) {
                struct multiboot_tag_module *m = (struct multiboot_tag_module *)t;
                uint64_t mod_end = (uint64_t)m->mod_end;
                if (mod_end > safe_start) safe_start = mod_end;
            }
            t = (struct multiboot_tag *)(((uintptr_t)t) + ((t->size + 7) & ~7));
        }
    }
    uint64_t bitmap_end;
    pmm_bitmap = (uint8_t*)ALIGN_4K(safe_start);
    if (!u64_add_ok((uint64_t)pmm_bitmap, bitmap_size, &bitmap_end) ||
        bitmap_end > max_addr) {
        screen_log("FAIL", COLOR_LIGHT_RED, "PMM: sem espaco para o bitmap!");
        while (1);
    }

    // 4. Initially, mark all pages as reserved/used (1)
    for (uint64_t i = 0; i < bitmap_size; i++) {
        pmm_bitmap[i] = 0xFF;
    }

    // 5. Parse the memory map and free available regions (0).
    //    Overlapping/unsorted entries are safe: bitmap_clear is idempotent.
    for (uint64_t i = 0; i < mmap_entries_count; i++) {
        struct multiboot_mmap_entry* entry =
            (struct multiboot_mmap_entry*)((uintptr_t)mmap_tag->entries + i * mmap_tag->entry_size);
        if (entry->type == MULTIBOOT_MEMORY_AVAILABLE && entry->len) {
            pmm_free_region(entry->addr, entry->len);
        }
    }

    // 6. PROTECT CRITICAL SECTORS: Re-reserve memory containing OS structures
    // - Reserve the first 1MB (contains BIOS, real mode tables, VGA memory)
    pmm_reserve_region(0x00000000, 0x100000);
    pmm_track_reserved(0x00000000, 0x100000);

    // - Reserve the Kernel itself (loaded at 1MB to kernel_end)
    uint64_t kernel_len = (uint64_t)_kernel_end - (uint64_t)_kernel_start;
    pmm_reserve_region((uint64_t)_kernel_start, kernel_len);
    pmm_track_reserved((uint64_t)_kernel_start, kernel_len);

    // - Reserve the PMM Bitmap itself
    pmm_reserve_region((uint64_t)pmm_bitmap, bitmap_size);
    pmm_track_reserved((uint64_t)pmm_bitmap, bitmap_size);

    // - Reserve the Multiboot2 Information structure passed by GRUB
    pmm_reserve_region(mbi_addr, mbi_size);
    pmm_track_reserved(mbi_addr, mbi_size);

    // - Reserve any Multiboot2 Modules (initrd, firmware, etc.)
    tag = (struct multiboot_tag*)(mbi_addr + 8);
    for (int steps = 0; steps < 64; steps++) {
        if ((uint64_t)tag < mbi_addr || (uint64_t)tag + sizeof(*tag) > mbi_addr + mbi_size)
            break;
        if (tag->type == MULTIBOOT_TAG_TYPE_END) break;
        if (tag->size < 8 || (uint64_t)tag + tag->size > mbi_addr + mbi_size) break;
        if (tag->type == MULTIBOOT_TAG_TYPE_MODULE) {
            struct multiboot_tag_module* mod = (struct multiboot_tag_module*)tag;
            uint64_t mlen = (mod->mod_end > mod->mod_start) ?
                (uint64_t)(mod->mod_end - mod->mod_start) : 0;
            pmm_reserve_region(mod->mod_start, mlen);
            pmm_track_reserved(mod->mod_start, mlen);
        }
        tag = (struct multiboot_tag*)((uintptr_t)tag + ((tag->size + 7) & ~7));
    }

    // Logs the physical RAM statistics
    screen_log(" OK ", COLOR_LIGHT_GREEN, "Physical Memory Manager (PMM) bitmap initialized.");
}

uint64_t pmm_alloc_block(void) {
    unsigned long flags;
    spin_lock_irqsave(&pmm_lock, &flags);

    // Search the bitmap byte-by-byte for a byte that has at least one free frame (not 0xFF)
    for (uint64_t i = 0; i < bitmap_size; i++) {
        if (pmm_bitmap[i] != 0xFF) {
            // Find the exact bit (0) inside the byte
            for (int bit = 0; bit < 8; bit++) {
                if (!(pmm_bitmap[i] & (1 << bit))) {
                    uint64_t page = i * 8 + bit;
                    if (page >= total_pages) break;
                    bitmap_set(page);
                    uint64_t address = page * PAGE_SIZE;
                    spin_unlock_irqrestore(&pmm_lock, flags);
                    return address; // Return the physical address of the page
                }
            }
        }
    }

    // Out of memory!
    spin_unlock_irqrestore(&pmm_lock, flags);
    return 0;
}

uint64_t pmm_alloc_block_dma32(void) {
    unsigned long flags;
    const uint64_t max_page_4g = 0x100000000ULL / PAGE_SIZE;
    uint64_t limit = total_pages < max_page_4g ? total_pages : max_page_4g;
    uint64_t limit_bytes = (limit + 7) / 8;
    if (limit_bytes > bitmap_size) limit_bytes = bitmap_size;

    spin_lock_irqsave(&pmm_lock, &flags);
    for (uint64_t i = 0; i < limit_bytes; i++) {
        if (pmm_bitmap[i] != 0xFF) {
            for (int bit = 0; bit < 8; bit++) {
                if (!(pmm_bitmap[i] & (1 << bit))) {
                    uint64_t page = i * 8 + bit;
                    if (page >= limit) break;
                    bitmap_set(page);
                    spin_unlock_irqrestore(&pmm_lock, flags);
                    return page * PAGE_SIZE;
                }
            }
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return 0;
}

uint64_t pmm_alloc_blocks(uint64_t n) {
    if (!n || n > total_pages) return 0;
    unsigned long flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    /* First-fit run scan. O(pages) but n is tiny in practice (DMA/coherent). */
    uint64_t run = 0, run_start = 0;
    for (uint64_t page = 0; page < total_pages; page++) {
        if (!bitmap_test(page)) {
            if (run == 0) run_start = page;
            if (++run == n) {
                for (uint64_t p = run_start; p < run_start + n; p++)
                    bitmap_set(p);
                spin_unlock_irqrestore(&pmm_lock, flags);
                return run_start * PAGE_SIZE;
            }
        } else {
            run = 0;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return 0;
}

int pmm_free_block_status(uint64_t addr) {
    if ((addr & (PAGE_SIZE - 1)) != 0 || addr == 0) {
        __sync_fetch_and_add(&pmm_err_invalid, 1);
        return -1;
    }
    uint64_t page = addr / PAGE_SIZE;
    if (page >= total_pages) {
        __sync_fetch_and_add(&pmm_err_invalid, 1);
        return -1;
    }
    if (pmm_is_reserved(addr)) {
        __sync_fetch_and_add(&pmm_err_reserved, 1);
        return -3;
    }
    unsigned long flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    if (!bitmap_test(page)) {
        /* Already free: double-free. Count it, keep the page free. */
        spin_unlock_irqrestore(&pmm_lock, flags);
        __sync_fetch_and_add(&pmm_err_double, 1);
        return -2;
    }
    bitmap_clear(page);
    spin_unlock_irqrestore(&pmm_lock, flags);
    return 0;
}

void pmm_free_block(uint64_t addr) {
    (void)pmm_free_block_status(addr);
}

void pmm_free_blocks(uint64_t addr, uint64_t n) {
    if (!n) return;
    uint64_t end;
    if ((addr & (PAGE_SIZE - 1)) || !u64_add_ok(addr, n * PAGE_SIZE, &end)) {
        __sync_fetch_and_add(&pmm_err_invalid, 1);
        return;
    }
    for (uint64_t i = 0; i < n; i++)
        pmm_free_block(addr + i * PAGE_SIZE);
}

uint64_t pmm_get_free_memory(void) {
    unsigned long flags;
    uint64_t result;

    spin_lock_irqsave(&pmm_lock, &flags);
    result = free_pages * PAGE_SIZE;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return result;
}

uint64_t pmm_get_total_memory(void) {
    return total_memory;
}

uint64_t pmm_get_reserved_memory(void) {
    uint64_t reserved = 0;
    for (int i = 0; i < pmm_reserved_count; i++)
        reserved += pmm_reserved[i].end - pmm_reserved[i].start;
    return reserved;
}

void pmm_get_error_counters(uint64_t *dbl, uint64_t *inv, uint64_t *rsv) {
    if (dbl) *dbl = pmm_err_double;
    if (inv) *inv = pmm_err_invalid;
    if (rsv) *rsv = pmm_err_reserved;
}
