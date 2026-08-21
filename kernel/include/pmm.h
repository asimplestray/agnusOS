#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

// Initialize the Physical Memory Manager using the Multiboot2 info structure
void pmm_init(uint64_t mbi_addr);

// Allocate a 4KB physical page frame (returns the physical address of the page, or 0 if out of memory)
uint64_t pmm_alloc_block(void);

// Free a previously allocated 4KB physical page frame
void pmm_free_block(uint64_t addr);

// Get the total free memory in bytes
uint64_t pmm_get_free_memory(void);

// Get the total physical memory detected in bytes
uint64_t pmm_get_total_memory(void);

#endif
