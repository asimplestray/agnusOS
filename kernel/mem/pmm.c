#include <pmm.h>
#include <multiboot2.h>
#include <screen.h>

extern char _kernel_start[];
extern char _kernel_end[];

static uint8_t* pmm_bitmap = 0;
static uint64_t total_pages = 0;
static uint64_t free_pages = 0;
static uint64_t total_memory = 0;
static uint64_t bitmap_size = 0;

#define ALIGN_4K(addr) (((addr) + 4095) & ~4095)

// Helper functions to get/set bits in the physical memory bitmap
static inline void bitmap_set(uint64_t page) {
    if (page >= total_pages) return;
    if (!(pmm_bitmap[page / 8] & (1 << (page % 8)))) {
        pmm_bitmap[page / 8] |= (1 << (page % 8));
        free_pages--;
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
    uint64_t start_page = start_addr / PAGE_SIZE;
    uint64_t num_pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    
    for (uint64_t i = 0; i < num_pages; i++) {
        bitmap_set(start_page + i);
    }
}

// Mark a range of physical memory as free/available
static void pmm_free_region(uint64_t start_addr, uint64_t length) {
    uint64_t start_page = start_addr / PAGE_SIZE;
    uint64_t num_pages = length / PAGE_SIZE; // Only free complete pages
    
    for (uint64_t i = 0; i < num_pages; i++) {
        bitmap_clear(start_page + i);
    }
}

void pmm_init(uint64_t mbi_addr) {
    uint32_t* mbi_size_ptr = (uint32_t*)mbi_addr;
    uint32_t mbi_size = *mbi_size_ptr;

    struct multiboot_tag* tag = (struct multiboot_tag*)(mbi_addr + 8);
    struct multiboot_tag_mmap* mmap_tag = 0;

    // 1. Locate the Memory Map tag in the Multiboot2 info
    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
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

    // 2. Discover total memory by finding the highest available address
    uint64_t max_addr = 0;
    uint64_t mmap_entries_count = (mmap_tag->size - sizeof(struct multiboot_tag_mmap)) / mmap_tag->entry_size;
    
    for (uint64_t i = 0; i < mmap_entries_count; i++) {
        struct multiboot_mmap_entry* entry = &mmap_tag->entries[i];
        if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
            if (entry->addr + entry->len > max_addr) {
                max_addr = entry->addr + entry->len;
            }
        }
    }

    total_memory = max_addr;
    total_pages = total_memory / PAGE_SIZE;
    bitmap_size = total_pages / 8;
    free_pages = 0;

    // 3. Position the PMM bitmap in memory after BOTH the kernel and the Multiboot structure to prevent overwrite!
    uint64_t kernel_end_addr = (uint64_t)_kernel_end;
    uint64_t mbi_end_addr = mbi_addr + mbi_size;
    uint64_t safe_start = (kernel_end_addr > mbi_end_addr) ? kernel_end_addr : mbi_end_addr;
    pmm_bitmap = (uint8_t*)ALIGN_4K(safe_start);

    // 4. Initially, mark all pages as reserved/used (1)
    for (uint64_t i = 0; i < bitmap_size; i++) {
        pmm_bitmap[i] = 0xFF;
    }

    // 5. Parse the memory map and free available regions (0)
    for (uint64_t i = 0; i < mmap_entries_count; i++) {
        struct multiboot_mmap_entry* entry = &mmap_tag->entries[i];
        if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
            pmm_free_region(entry->addr, entry->len);
        }
    }

    // 6. PROTECT CRITICAL SECTORS: Re-reserve memory containing OS structures
    // - Reserve the first 1MB (contains BIOS, real mode tables, VGA memory)
    pmm_reserve_region(0x00000000, 0x100000);

    // - Reserve the Kernel itself (loaded at 1MB to kernel_end)
    uint64_t kernel_len = (uint64_t)_kernel_end - (uint64_t)_kernel_start;
    pmm_reserve_region((uint64_t)_kernel_start, kernel_len);

    // - Reserve the PMM Bitmap itself
    pmm_reserve_region((uint64_t)pmm_bitmap, bitmap_size);

    // - Reserve the Multiboot2 Information structure passed by GRUB
    pmm_reserve_region(mbi_addr, mbi_size);

    // - Reserve any Multiboot2 Modules (initrd, firmware, etc.)
    tag = (struct multiboot_tag*)(mbi_addr + 8);
    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT_TAG_TYPE_MODULE) {
            struct multiboot_tag_module* mod = (struct multiboot_tag_module*)tag;
            pmm_reserve_region(mod->mod_start, mod->mod_end - mod->mod_start);
        }
        tag = (struct multiboot_tag*)((uintptr_t)tag + ((tag->size + 7) & ~7));
    }

    // Logs the physical RAM statistics
    screen_log(" OK ", COLOR_LIGHT_GREEN, "Physical Memory Manager (PMM) bitmap initialized.");
}

uint64_t pmm_alloc_block(void) {
    // Search the bitmap byte-by-byte for a byte that has at least one free frame (not 0xFF)
    for (uint64_t i = 0; i < bitmap_size; i++) {
        if (pmm_bitmap[i] != 0xFF) {
            // Find the exact bit (0) inside the byte
            for (int bit = 0; bit < 8; bit++) {
                if (!(pmm_bitmap[i] & (1 << bit))) {
                    uint64_t page = i * 8 + bit;
                    bitmap_set(page);
                    return page * PAGE_SIZE; // Return the physical address of the page
                }
            }
        }
    }
    
    // Out of memory!
    return 0;
}

void pmm_free_block(uint64_t addr) {
    uint64_t page = addr / PAGE_SIZE;
    bitmap_clear(page);
}

uint64_t pmm_get_free_memory(void) {
    return free_pages * PAGE_SIZE;
}

uint64_t pmm_get_total_memory(void) {
    return total_memory;
}
