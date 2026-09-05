#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H

#include <stdint.h>

#define MULTIBOOT2_MAGIC 0x36d76289

// Multiboot2 Tag Types
#define MULTIBOOT_TAG_TYPE_END 0
#define MULTIBOOT_TAG_TYPE_CMDLINE 1
#define MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME 2
#define MULTIBOOT_TAG_TYPE_MODULE 3
#define MULTIBOOT_TAG_TYPE_BASIC_MEMINFO 4
#define MULTIBOOT_TAG_TYPE_BOOTDEV 5
#define MULTIBOOT_TAG_TYPE_MMAP 6
#define MULTIBOOT_TAG_TYPE_VBE 7
#define MULTIBOOT_TAG_TYPE_FRAMEBUFFER 8
#define MULTIBOOT_TAG_TYPE_ACPI_OLD 14
#define MULTIBOOT_TAG_TYPE_ACPI_NEW 15

// Memory map types
#define MULTIBOOT_MEMORY_AVAILABLE 1
#define MULTIBOOT_MEMORY_RESERVED 2
#define MULTIBOOT_MEMORY_ACPI_RECLAIMABLE 3
#define MULTIBOOT_MEMORY_NVS 4
#define MULTIBOOT_MEMORY_BADRAM 5

// Header for any Multiboot2 tag
struct multiboot_tag {
    uint32_t type;
    uint32_t size;
};

// Memory map entry layout
struct multiboot_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
} __attribute__((packed));

// Memory map tag
struct multiboot_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct multiboot_mmap_entry entries[0];
};

// Basic memory info tag
struct multiboot_tag_basic_meminfo {
    uint32_t type;
    uint32_t size;
    uint32_t mem_lower;
    uint32_t mem_upper;
};

// Framebuffer tag — passed by GRUB when a linear framebuffer was set up
struct multiboot_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;   // Physical address of the framebuffer
    uint32_t framebuffer_pitch;  // Bytes per row (may be wider than width*bpp/8)
    uint32_t framebuffer_width;  // Width in pixels
    uint32_t framebuffer_height; // Height in pixels
    uint8_t  framebuffer_bpp;    // Bits per pixel (32 in our case)
    uint8_t  framebuffer_type;   // 1 = RGB color, 2 = EGA text
    uint16_t reserved;
} __attribute__((packed));

// Module tag (initrd)
struct multiboot_tag_module {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char cmdline[0];
} __attribute__((packed));

// ACPI RSDP tag (type 14 = ACPI 1.0, type 15 = ACPI 2.0+)
struct multiboot_tag_acpi {
    uint32_t type;
    uint32_t size;
    uint64_t rsdp;
} __attribute__((packed));

// Iterate multiboot tags
static inline struct multiboot_tag *multiboot_next_tag(struct multiboot_tag *tag) {
    return (struct multiboot_tag *)(((uintptr_t)tag) + ((tag->size + 7) & ~7));
}

static inline struct multiboot_tag *multiboot_find_tag(uint64_t mbi, uint32_t type) {
    struct multiboot_tag *tag = (struct multiboot_tag *)(mbi + 8);
    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == type) return tag;
        tag = multiboot_next_tag(tag);
    }
    return NULL;
}

#endif
