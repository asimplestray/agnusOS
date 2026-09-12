#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

// Initialize the Physical Memory Manager using the Multiboot2 info structure
void pmm_init(uint64_t mbi_addr);

// Allocate a 4KB physical page frame (returns the physical address of the page, or 0 if out of memory)
uint64_t pmm_alloc_block(void);

// Allocate a 4KB page guaranteed below 4 GiB (DMA32). Returns 0 on failure.
uint64_t pmm_alloc_block_dma32(void);

// Allocate n contiguous 4KB pages. Returns base physical address or 0.
uint64_t pmm_alloc_blocks(uint64_t n);

// Free a previously allocated 4KB physical page frame
void pmm_free_block(uint64_t addr);

// Free n contiguous pages starting at addr
void pmm_free_blocks(uint64_t addr, uint64_t n);

// Status version: 0 ok, -1 invalid/unaligned, -2 double-free, -3 reserved
int pmm_free_block_status(uint64_t addr);

// True if the page belongs to a permanently reserved region (low 1MB,
// kernel, PMM bitmap, MBI, modules). Never free these after init.
int pmm_is_reserved(uint64_t addr);

// Get the total free memory in bytes
uint64_t pmm_get_free_memory(void);

// Get the total physical memory detected in bytes
uint64_t pmm_get_total_memory(void);

// Reserved (non-freeable) bytes tracked at init
uint64_t pmm_get_reserved_memory(void);

// Counters for debug: rejected frees (double/invalid/reserved)
void pmm_get_error_counters(uint64_t *dbl, uint64_t *inv, uint64_t *rsv);

#endif
