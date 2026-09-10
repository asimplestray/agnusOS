#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>

// Page Table Entry Flags (x86_64)
#define VMM_FLAG_PRESENT  (1ULL << 0)
#define VMM_FLAG_WRITE    (1ULL << 1)
#define VMM_FLAG_USER     (1ULL << 2)
#define VMM_FLAG_NX       (1ULL << 63) // No Execute (if supported)
#define VMM_FLAG_WC       (1ULL << 10) // Write-Combine (PWT=1, PCD=1)
#define VMM_FLAG_PWT      (1ULL << 3)  // Page Write-Through
#define VMM_FLAG_PCD      (1ULL << 4)  // Page Cache Disable

// Page Fault Error Code bits
#define PF_ERR_P       (1 << 0) // Page not present
#define PF_ERR_W       (1 << 1) // Write access
#define PF_ERR_U       (1 << 2) // User mode
#define PF_ERR_RSVD    (1 << 3) // Reserved bit set
#define PF_ERR_I       (1 << 4) // Instruction fetch

// Initialize the Virtual Memory Manager using the active PML4 page table
void vmm_init(void);

// Map a 4KB virtual page to a physical block address with target flags (active PML4)
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

// Unmap a previously mapped virtual address (active PML4)
void vmm_unmap_page(uint64_t virt);

// Map a region (active PML4)
void vmm_map_region(uint64_t virt, uint64_t phys, uint32_t size, uint64_t flags);

// Helper to invalidate a single TLB entry
static inline void vmm_tlb_flush(uint64_t virt) {
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

/* Process Isolation APIs */
uint64_t vmm_create_pml4(void);
void     vmm_free_pml4(uint64_t pml4_phys);
void     vmm_map_page_in_pml4(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);
void     vmm_map_region_in_pml4(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint32_t size, uint64_t flags);
void     vmm_unmap_page_in_pml4(uint64_t pml4_phys, uint64_t virt);
uint64_t vmm_get_phys(uint64_t pml4_phys, uint64_t virt);

// Page fault handler - called from interrupt_handler
void vmm_page_fault_handler(uint64_t fault_addr, uint64_t error_code, uint64_t rip);

#endif
