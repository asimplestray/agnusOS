#include <kheap.h>
#include <vmm.h>
#include <pmm.h>
#include <screen.h>
#include <string.h>

#define HEAP_START        0x10000000 // 256MB - within boot identity-mapped range
#define HEAP_INITIAL_SIZE 0x800000   // 8MB Initial Heap Size

struct heap_block {
    size_t size;               // Size of usable data in this block
    int is_free;              // 1 if this block is free, 0 if in use
    struct heap_block* next;   // Pointer to the next block in the list
};

static struct heap_block* heap_start_block = 0;
static uint64_t heap_end = 0;

/* Coalesce: Merge consecutive free blocks to prevent fragmentation */
static void kheap_coalesce(void) {
    struct heap_block* current = heap_start_block;
    while (current != 0) {
        if (current->is_free && current->next != 0 && current->next->is_free) {
            current->size = current->size + sizeof(struct heap_block) + current->next->size;
            current->next = current->next->next;
            /* Repeat on the same block in case the next-next block is also free */
            continue;
        }
        current = current->next;
    }
}

void kheap_init(void) {
    // The kernel heap region (256MB-256MB+8MB) is already identity-mapped with 2MB huge pages
    // by the boot page tables. We just need to initialize the heap metadata.
    
    // 1. Initialize the first big block spanning the entire heap
    heap_start_block = (struct heap_block*)HEAP_START;
    heap_start_block->size = HEAP_INITIAL_SIZE - sizeof(struct heap_block);
    heap_start_block->is_free = 1;
    heap_start_block->next = 0;

    heap_end = HEAP_START + HEAP_INITIAL_SIZE;

    screen_log(" OK ", COLOR_LIGHT_GREEN, "Kernel Heap Allocator (kmalloc/kfree) initialized (Size: 8MB).");
}

void* kmalloc(size_t size) {
    if (size == 0) return 0;

    // Align size to 8 bytes for double-word CPU alignment speed
    size = (size + 7) & ~7;
    
    struct heap_block* current = heap_start_block;
    
    // First-Fit search
    while (current != 0) {
        if (current->is_free && current->size >= size) {
            // Can we split this block?
            if (current->size >= size + sizeof(struct heap_block) + 8) {
                struct heap_block* new_block = (struct heap_block*)((uintptr_t)current + sizeof(struct heap_block) + size);
                
                new_block->size = current->size - size - sizeof(struct heap_block);
                new_block->is_free = 1;
                new_block->next = current->next;
                
                current->size = size;
                current->next = new_block;
            }
            
            current->is_free = 0;
            return (void*)((uintptr_t)current + sizeof(struct heap_block));
        }
        current = current->next;
    }
    
    // Heap expansion disabled - kernel heap region is pre-mapped with huge pages
    return 0;
}

void kfree(void* ptr) {
    if (!ptr) return;

    // Retrieve the header of the block
    struct heap_block* block = (struct heap_block*)((uintptr_t)ptr - sizeof(struct heap_block));
    block->is_free = 1;

    // Coalesce: Merge consecutive free blocks to prevent fragmentation
    kheap_coalesce();
}

void* krealloc(void* ptr, size_t size) {
    if (!ptr)
        return kmalloc(size);
    
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }
    
    struct heap_block* block = (struct heap_block*)((uintptr_t)ptr - sizeof(struct heap_block));
    
    if (block->size >= size) {
        if (block->size >= size + sizeof(struct heap_block) + 8) {
            struct heap_block* new_block = (struct heap_block*)((uintptr_t)block + sizeof(struct heap_block) + size);
            new_block->size = block->size - size - sizeof(struct heap_block);
            new_block->is_free = 1;
            new_block->next = block->next;
            block->size = size;
            block->next = new_block;
            kheap_coalesce();
        }
        return ptr;
    }
    
    void* new_ptr = kmalloc(size);
    if (!new_ptr)
        return NULL;
    
    memcpy(new_ptr, ptr, block->size);
    kfree(ptr);
    
    return new_ptr;
}

void itoa(int64_t val, char *buf, int base) {
    if (base < 2 || base > 36) return;
    
    char *ptr = buf;
    char *ptr1 = ptr;
    char tmp_char;
    int64_t tmp_val;
    
    if (val == 0) {
        *ptr++ = '0';
        *ptr = '\0';
        return;
    }
    
    if (val < 0 && base == 10) {
        *ptr++ = '-';
        val = -val;
    }
    
    while (val != 0) {
        tmp_val = val;
        val /= base;
        *ptr++ = "0123456789abcdefghijklmnopqrstuvwxyz"[tmp_val - val * base];
    }
    
    *ptr = '\0';
    
    if (*buf == '-') ptr1++;
    
    while (ptr1 < ptr - 1) {
        tmp_char = *ptr1;
        *ptr1 = *(ptr - 1);
        *--ptr = tmp_char;
        ptr1++;
    }
}
