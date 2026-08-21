#ifndef KHEAP_H
#define KHEAP_H

#include <stddef.h>
#include <stdint.h>

// Initialize the kernel dynamic heap allocator
void kheap_init(void);

// Allocate dynamic memory on the kernel heap
void* kmalloc(size_t size);

// Reallocate memory block
void* krealloc(void* ptr, size_t size);

// Free a previously allocated memory block
void kfree(void* ptr);

// Simple integer to string conversion
void itoa(int64_t val, char *buf, int base);

#endif
