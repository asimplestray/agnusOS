#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

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

/* Canonical lower-half address space available to ring 3. The null page is
 * intentionally excluded so NULL and small invalid pointers always fail. */
#define VMM_USER_MIN       0x0000000000010000ULL
#define VMM_USER_MAX       0x00007FFFFFFFFFFFULL

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
uint64_t vmm_active_pml4(void);
void     vmm_free_pml4(uint64_t pml4_phys);
void     vmm_activate_pml4(uint64_t pml4_phys);
/* Map one page. Returns false on invalid args, userspace-range violation,
 * W^X violation (USER+WRITE without NX) or OOM while allocating tables.
 * Intermediate tables allocated during a failed call are left in place
 * (leaked, never half-mapped): the target PTE itself is untouched. */
bool     vmm_map_page_in_pml4(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);
bool     vmm_map_region_in_pml4(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint32_t size, uint64_t flags);
void     vmm_unmap_page_in_pml4(uint64_t pml4_phys, uint64_t virt);
uint64_t vmm_get_phys(uint64_t pml4_phys, uint64_t virt);
uint64_t vmm_get_page_flags(uint64_t pml4_phys, uint64_t virt);

/* VMA registry per address space (mm->vmas, guarded by mm->lock).
 * Faults outside a registered VMA are fatal to the task. */
struct mm_struct;
struct vma;
bool vma_add(struct mm_struct *mm, uint64_t start, uint64_t end,
             uint64_t flags, int type);
struct vma *vma_find(struct mm_struct *mm, uint64_t addr);
void vma_remove(struct mm_struct *mm, uint64_t start, uint64_t end);
void vma_clear(struct mm_struct *mm);

struct interrupt_frame;

// Page fault handler - registered as the C handler for exception vector 14
void vmm_page_fault_handler(struct interrupt_frame *frame);

#endif
