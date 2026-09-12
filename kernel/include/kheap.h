#ifndef KHEAP_H
#define KHEAP_H

#include <stddef.h>
#include <stdint.h>

// Initialize the kernel dynamic heap allocator
void kheap_init(void);

// Allocate dynamic memory on the kernel heap
void* kmalloc(size_t size);

// Allocate n*size bytes with overflow check (like calloc, zeroed)
void* kmalloc_array(size_t n, size_t size);
void* kcalloc(size_t n, size_t size);

// Reallocate memory block
void* krealloc(void* ptr, size_t size);

// Free a previously allocated memory block
void kfree(void* ptr);

// Statistics for observability (all counters monotonic except used)
void kheap_stats(uint64_t *used, uint64_t *total, uint64_t *allocs,
                 uint64_t *frees, uint64_t *fails, uint64_t *bad_frees);

// Simple integer to string conversion
void itoa(int64_t val, char *buf, int base);

#endif
