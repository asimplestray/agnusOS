#include <vmm.h>
#include <pmm.h>
#include <screen.h>
#include <task.h>
#include <kheap.h>

static uint64_t* active_pml4 = 0;
uint64_t kernel_pml4_phys = 0;

static inline uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

void vmm_activate_pml4(uint64_t pml4_phys) {
    if (!pml4_phys || read_cr3() == pml4_phys)
        return;

    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

uint64_t vmm_active_pml4(void) {
    return read_cr3();
}

void vmm_init(void) {
    // Read the active PML4 physical address from CR3 (identity mapped during boot)
    kernel_pml4_phys = read_cr3();
    active_pml4 = (uint64_t*)kernel_pml4_phys;
    screen_log(" OK ", COLOR_LIGHT_GREEN, "Virtual Memory Manager (VMM) initialized successfully.");
}

uint64_t vmm_create_pml4(void) {
    uint64_t new_pml4_phys = pmm_alloc_block();
    if (!new_pml4_phys) return 0;
    
    uint64_t *new_pml4 = (uint64_t *)new_pml4_phys;
    // Zero user space mappings (entries 0 to 255)
    for (int i = 0; i < 256; i++) {
        new_pml4[i] = 0;
    }
    // Preserve kernel heap mapping (entry 0 maps 0-512GB, includes 3GB heap)
    new_pml4[0] = active_pml4[0];
    // Copy kernel space mappings (entries 256 to 511)
    for (int i = 256; i < 512; i++) {
        new_pml4[i] = active_pml4[i];
    }
    
    return new_pml4_phys;
}

bool vmm_map_page_in_pml4(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!pml4_phys || !phys)
        return false;
    if (virt & (PAGE_SIZE - 1)) return false;
    if ((flags & VMM_FLAG_USER) &&
        (virt < VMM_USER_MIN || virt > VMM_USER_MAX))
        return false;
    /* Never allow a USER mapping above the canonical user range (kernel/
     * MMIO/page-table pages). Supervisor mappings keep full range. */
    if ((flags & VMM_FLAG_USER) && ((virt >> 47) & 1)) return false;
    /* W^X hardening for userspace data: USER+WRITE without NX becomes NX.
     * Code mappings must come without WRITE instead. */
    if ((flags & (VMM_FLAG_USER | VMM_FLAG_WRITE)) ==
        (VMM_FLAG_USER | VMM_FLAG_WRITE))
        flags |= VMM_FLAG_NX;

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t *pml4 = (uint64_t *)pml4_phys;
    uint64_t pml4_entry = pml4[pml4_idx];
    uint64_t* pdpt = 0;
    if (!(pml4_entry & VMM_FLAG_PRESENT)) {
        uint64_t new_table_phys = pmm_alloc_block();
        if (!new_table_phys) return false;
        
        uint64_t* new_table = (uint64_t*)new_table_phys;
        for (int i = 0; i < 512; i++) {
            new_table[i] = 0;
        }
        
        uint64_t table_flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITE;
        if (flags & VMM_FLAG_USER) table_flags |= VMM_FLAG_USER;
        pml4[pml4_idx] = new_table_phys | table_flags;
        pdpt = new_table;
    } else {
        pdpt = (uint64_t*)(pml4_entry & ~0xFFF);
    }

    uint64_t pdpt_entry = pdpt[pdpt_idx];
    uint64_t* pd = 0;
    if (!(pdpt_entry & VMM_FLAG_PRESENT)) {
        uint64_t new_table_phys = pmm_alloc_block();
        if (!new_table_phys) return false;
        
        uint64_t* new_table = (uint64_t*)new_table_phys;
        for (int i = 0; i < 512; i++) {
            new_table[i] = 0;
        }
        
        uint64_t table_flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITE;
        if (flags & VMM_FLAG_USER) table_flags |= VMM_FLAG_USER;
        pdpt[pdpt_idx] = new_table_phys | table_flags;
        pd = new_table;
    } else {
        pd = (uint64_t*)(pdpt_entry & ~0xFFF);
    }

    uint64_t pd_entry = pd[pd_idx];
    uint64_t* pt = 0;
    if (!(pd_entry & VMM_FLAG_PRESENT)) {
        uint64_t new_table_phys = pmm_alloc_block();
        if (!new_table_phys) return false;
        
        uint64_t* new_table = (uint64_t*)new_table_phys;
        for (int i = 0; i < 512; i++) {
            new_table[i] = 0;
        }
        
        uint64_t table_flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITE;
        if (flags & VMM_FLAG_USER) table_flags |= VMM_FLAG_USER;
        pd[pd_idx] = new_table_phys | table_flags;
        pt = new_table;
    } else {
        pt = (uint64_t*)(pd_entry & ~0xFFF);
    }

    uint64_t pte_flags = flags;
    if (flags & VMM_FLAG_WC) {
        pte_flags |= VMM_FLAG_PWT | VMM_FLAG_PCD;
        pte_flags &= ~VMM_FLAG_WC;
    }
    pt[pt_idx] = (phys & ~0xFFF) | pte_flags;

    uint64_t active_pml4_phys = read_cr3();
    if (active_pml4_phys == pml4_phys) {
        vmm_tlb_flush(virt);
    }
    return true;
}

bool vmm_map_region_in_pml4(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint32_t size, uint64_t flags) {
    if (!size) return true;
    if (virt & (PAGE_SIZE - 1)) return false;
    uint64_t end;
    if (__builtin_add_overflow(virt, (uint64_t)size, &end)) return false;
    /* Same rounding as before: cover [virt, virt+size) with whole pages.
     * phys is aligned down like vmm_map_page_in_pml4 does per page. */
    uint64_t phys_base = phys & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        if (!vmm_map_page_in_pml4(pml4_phys, virt + i * PAGE_SIZE,
                                  phys_base + i * PAGE_SIZE, flags)) {
            /* Roll back pages mapped by this call; tables stay (safe leak). */
            for (uint64_t j = 0; j < i; j++)
                vmm_unmap_page_in_pml4(pml4_phys, virt + j * PAGE_SIZE);
            return false;
        }
    }
    return true;
}

void vmm_unmap_page_in_pml4(uint64_t pml4_phys, uint64_t virt) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t *pml4 = (uint64_t *)pml4_phys;
    uint64_t pml4_entry = pml4[pml4_idx];
    if (!(pml4_entry & VMM_FLAG_PRESENT)) return;

    uint64_t* pdpt = (uint64_t*)(pml4_entry & ~0xFFF);
    uint64_t pdpt_entry = pdpt[pdpt_idx];
    if (!(pdpt_entry & VMM_FLAG_PRESENT)) return;

    uint64_t* pd = (uint64_t*)(pdpt_entry & ~0xFFF);
    uint64_t pd_entry = pd[pd_idx];
    if (!(pd_entry & VMM_FLAG_PRESENT)) return;

    uint64_t* pt = (uint64_t*)(pd_entry & ~0xFFF);
    pt[pt_idx] = 0; // Clear the entry

    uint64_t active_pml4_phys = read_cr3();
    if (active_pml4_phys == pml4_phys) {
        vmm_tlb_flush(virt);
    }
}

void vmm_free_pml4(uint64_t pml4_phys) {
    if (!pml4_phys) return;
    uint64_t *pml4 = (uint64_t *)pml4_phys;
    /* Entries 1..255 are per-process. Entry 0 is SHARED with the kernel
     * (kernel heap lives at 0x10000000, copied by vmm_create_pml4) and must
     * never be freed here — freeing it would destroy the kernel heap page
     * tables on the first task exit. Entries 256..511 are kernel-half,
     * also shared, also skipped. */
    for (int i = 1; i < 256; i++) { // user space only, minus shared entry 0
        if (pml4[i] & VMM_FLAG_PRESENT) {
            uint64_t *pdpt = (uint64_t *)(pml4[i] & ~0xFFF);
            for (int j = 0; j < 512; j++) {
                if (pdpt[j] & VMM_FLAG_PRESENT) {
                    uint64_t *pd = (uint64_t *)(pdpt[j] & ~0xFFF);
                    for (int k = 0; k < 512; k++) {
                        if (pd[k] & VMM_FLAG_PRESENT) {
                            uint64_t *pt = (uint64_t *)(pd[k] & ~0xFFF);
                            for (int l = 0; l < 512; l++) {
                                if (pt[l] & VMM_FLAG_PRESENT) {
                                    uint64_t phys_page = pt[l] & ~0xFFF;
                                    pmm_free_block(phys_page);
                                }
                            }
                            pmm_free_block((uint64_t)pt);
                        }
                    }
                    pmm_free_block((uint64_t)pd);
                }
            }
            pmm_free_block((uint64_t)pdpt);
        }
    }
    pmm_free_block(pml4_phys);
}

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    vmm_map_page_in_pml4(read_cr3(), virt, phys, flags);
}

void vmm_map_region(uint64_t virt, uint64_t phys, uint32_t size, uint64_t flags) {
    vmm_map_region_in_pml4(read_cr3(), virt, phys, size, flags);
}

void vmm_unmap_page(uint64_t virt) {
    vmm_unmap_page_in_pml4(read_cr3(), virt);
}
uint64_t vmm_get_phys(uint64_t pml4_phys, uint64_t virt)
{
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t *pml4 = (uint64_t *)pml4_phys;
    if (!(pml4[pml4_idx] & VMM_FLAG_PRESENT)) return 0;

    uint64_t* pdpt = (uint64_t*)(pml4[pml4_idx] & ~0xFFF);
    if (!(pdpt[pdpt_idx] & VMM_FLAG_PRESENT)) return 0;

    uint64_t* pd = (uint64_t*)(pdpt[pdpt_idx] & ~0xFFF);
    if (!(pd[pd_idx] & VMM_FLAG_PRESENT)) return 0;

    uint64_t* pt = (uint64_t*)(pd[pd_idx] & ~0xFFF);
    if (!(pt[pt_idx] & VMM_FLAG_PRESENT)) return 0;

    return pt[pt_idx] & ~0xFFF;
}

uint64_t vmm_get_page_flags(uint64_t pml4_phys, uint64_t virt)
{
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx = (virt >> 21) & 0x1FF;
    uint64_t pt_idx = (virt >> 12) & 0x1FF;
    uint64_t *pml4 = (uint64_t *)pml4_phys;
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t *pt;

    if (!pml4 || !(pml4[pml4_idx] & VMM_FLAG_PRESENT))
        return 0;
    pdpt = (uint64_t *)(pml4[pml4_idx] & ~0xFFFULL);
    if (!(pdpt[pdpt_idx] & VMM_FLAG_PRESENT))
        return 0;
    pd = (uint64_t *)(pdpt[pdpt_idx] & ~0xFFFULL);
    if (!(pd[pd_idx] & VMM_FLAG_PRESENT))
        return 0;
    pt = (uint64_t *)(pd[pd_idx] & ~0xFFFULL);
    return pt[pt_idx];
}

/* -------------------------------------------------------------------------
 * VMA registry — authorized user mappings per address space
 * ---------------------------------------------------------------------- */

bool vma_add(struct mm_struct *mm, uint64_t start, uint64_t end,
             uint64_t flags, int type) {
    if (!mm || start >= end) return false;
    start &= ~(uint64_t)(PAGE_SIZE - 1);
    end = (end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (start >= end) return false;
    if (start < VMM_USER_MIN || end - 1 > VMM_USER_MAX) return false;

    vma_t *v = (vma_t *)kmalloc(sizeof(vma_t));
    if (!v) return false;
    v->start = start;
    v->end = end;
    v->flags = flags;
    v->type = (vma_type_t)type;
    v->next = NULL;

    unsigned long irq;
    spin_lock_irqsave(&mm->lock, &irq);
    /* Reject overlaps: double-registration is always a caller bug
     * (brk/mmap bases are per-process monotonic; ELF segments must not
     * overlap). Adjacent ranges are kept separate — no merge yet. */
    for (vma_t *c = mm->vmas; c; c = c->next) {
        if (start < c->end && end > c->start) {
            spin_unlock_irqrestore(&mm->lock, irq);
            kfree(v);
            return false;
        }
    }
    /* Sorted insert. */
    vma_t **link = &mm->vmas;
    while (*link && (*link)->start < start) link = &(*link)->next;
    v->next = *link;
    *link = v;
    spin_unlock_irqrestore(&mm->lock, irq);
    return true;
}

struct vma *vma_find(struct mm_struct *mm, uint64_t addr) {
    if (!mm) return NULL;
    unsigned long irq;
    spin_lock_irqsave(&mm->lock, &irq);
    vma_t *found = NULL;
    for (vma_t *c = mm->vmas; c; c = c->next) {
        if (addr >= c->start && addr < c->end) { found = c; break; }
    }
    spin_unlock_irqrestore(&mm->lock, irq);
    /* Lifetime: VMAs die only via vma_remove/clear from the owning task
     * itself or its exit path — no use-after-free from the fault path. */
    return (struct vma *)found;
}

void vma_remove(struct mm_struct *mm, uint64_t start, uint64_t end) {
    if (!mm || start >= end) return;
    start &= ~(uint64_t)(PAGE_SIZE - 1);
    end = (end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (start >= end) return;
    unsigned long irq;
    spin_lock_irqsave(&mm->lock, &irq);
    vma_t **link = &mm->vmas;
    while (*link) {
        vma_t *c = *link;
        if (c->end <= start || c->start >= end) { link = &c->next; continue; }
        if (c->start >= start && c->end <= end) {
            /* Fully covered: drop. */
            *link = c->next;
            kfree(c);
            continue;
        }
        if (c->start < start && c->end > end) {
            /* Punch a hole: split into [c->start,start) + [end,c->end). */
            vma_t *right = (vma_t *)kmalloc(sizeof(vma_t));
            if (!right) return; /* keep original on OOM, unlock below */
            *right = *c;
            right->start = end;
            c->end = start;
            right->next = c->next;
            c->next = right;
            break;
        }
        /* Partial overlap: trim. */
        if (c->start < start) c->end = start;
        else c->start = end;
        link = &c->next;
    }
    spin_unlock_irqrestore(&mm->lock, irq);
}

void vma_clear(struct mm_struct *mm) {
    if (!mm) return;
    unsigned long irq;
    spin_lock_irqsave(&mm->lock, &irq);
    vma_t *c = mm->vmas;
    mm->vmas = NULL;
    spin_unlock_irqrestore(&mm->lock, irq);
    while (c) { vma_t *n = c->next; kfree(c); c = n; }
}

/* -------------------------------------------------------------------------
 * Page Fault Handler
 * ---------------------------------------------------------------------- */

static bool handle_demand_page_fault(uint64_t pml4_phys, uint64_t virt, uint64_t flags) {
    uint64_t phys = pmm_alloc_block();
    if (!phys) {
        screen_log("FAIL", COLOR_LIGHT_RED, "Demand paging: OOM");
        return false;
    }

    /* Zero the page */
    uint8_t *dst = (uint8_t *)phys;
    for (int i = 0; i < 4096; i++) dst[i] = 0;

    uint64_t map_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (flags & VMM_FLAG_WRITE) map_flags |= VMM_FLAG_WRITE;
    if (flags & VMM_FLAG_NX)    map_flags |= VMM_FLAG_NX;

    vmm_map_page_in_pml4(pml4_phys, virt, phys, map_flags);

    /* vmm_map_page_in_pml4() has a void interface and can fail while allocating
     * an intermediate paging structure. Verify that the requested page became
     * visible and release the physical page on failure. */
    if (vmm_get_phys(pml4_phys, virt) != phys) {
        pmm_free_block(phys);
        screen_log("FAIL", COLOR_LIGHT_RED, "Demand paging: mapping failed");
        return false;
    }

    return true;
}

static bool handle_stack_growth(uint64_t pml4_phys, uint64_t virt,
                                const struct interrupt_frame *frame) {
    /* With a VMA list, only faults inside a STACK/ANON VMA are authorized.
     * Without one (legacy boot tasks), keep the old bounded heuristic. */
    if (!current || !current->mm || !frame) return false;
    uint64_t stack_top = current->mm->start_stack;
    if (stack_top == 0 || virt >= stack_top) return false;

    vma_t *v = (vma_t *)vma_find(current->mm, virt);
    if (v) {
        if (v->type != VMA_TYPE_STACK && v->type != VMA_TYPE_ANON) return false;
        /* Guard page: first page of a STACK VMA never maps. */
        if (v->type == VMA_TYPE_STACK && virt < v->start + PAGE_SIZE) return false;
    } else {
        /* No VMA covers it: allow only the legacy 1 MiB window so old
         * tasks keep working until their creators register VMAs. */
        int has_any = 0;
        {
            unsigned long irq;
            spin_lock_irqsave(&current->mm->lock, &irq);
            has_any = (current->mm->vmas != NULL);
            spin_unlock_irqrestore(&current->mm->lock, irq);
        }
        if (has_any) return false;
        uint64_t stack_limit = stack_top - (1024 * 1024);
        if (virt < stack_limit + PAGE_SIZE) return false;
    }
    if (frame->rsp < virt || frame->rsp - virt > (64 * 1024)) return false;

    return handle_demand_page_fault(pml4_phys, virt,
                                    VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NX);
}

void vmm_page_fault_handler(struct interrupt_frame *frame)
{
    uint64_t fault_addr;
    uint64_t error_code;

    if (!frame)
        return;

    /* CR2 is read at the beginning of the C exception path, before any
     * operation that could itself fault and overwrite it. The error code and
     * RIP come from the canonical interrupt frame shared by all exception
     * stubs. */
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));
    error_code = frame->err_code;

    /* Align to page boundary */
    uint64_t virt = fault_addr & ~0xFFF;
    uint64_t pml4_phys = read_cr3();

    /* If kernel fault, panic */
    if (!(error_code & PF_ERR_U)) {
        screen_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        screen_print("\n!!! KERNEL PAGE FAULT !!!\n");
        screen_print("Addr: "); /* simplified */
        screen_print("RIP: ");
        while (1) __asm__ volatile("hlt");
    }

    /* Reserved-bit faults indicate malformed paging structures and must never
     * be interpreted as a request for anonymous memory. */
    if ((error_code & PF_ERR_RSVD) || !current || !current->mm) {
        task_exit(-14);
    }

    /* Protection violations are never recoverable through demand paging. */
    if (error_code & PF_ERR_P) {
        task_exit(-14);
    }

    /* Generic anonymous demand paging is intentionally disabled until the mm
     * owns a real VMA list. Only controlled, writable, non-executable stack
     * growth is currently authorized. */
    if ((error_code & PF_ERR_I) || !(error_code & PF_ERR_W) ||
        !handle_stack_growth(pml4_phys, virt, frame)) {
        task_exit(-14);
    }
    return;
}
