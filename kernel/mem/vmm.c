#include <vmm.h>
#include <pmm.h>
#include <screen.h>
#include <task.h>

static uint64_t* active_pml4 = 0;
uint64_t kernel_pml4_phys = 0;

static inline uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
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

void vmm_map_page_in_pml4(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t *pml4 = (uint64_t *)pml4_phys;
    uint64_t pml4_entry = pml4[pml4_idx];
    uint64_t* pdpt = 0;
    if (!(pml4_entry & VMM_FLAG_PRESENT)) {
        uint64_t new_table_phys = pmm_alloc_block();
        if (!new_table_phys) return;
        
        uint64_t* new_table = (uint64_t*)new_table_phys;
        for (int i = 0; i < 512; i++) {
            new_table[i] = 0;
        }
        
        pml4[pml4_idx] = new_table_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER;
        pdpt = new_table;
    } else {
        pdpt = (uint64_t*)(pml4_entry & ~0xFFF);
    }

    uint64_t pdpt_entry = pdpt[pdpt_idx];
    uint64_t* pd = 0;
    if (!(pdpt_entry & VMM_FLAG_PRESENT)) {
        uint64_t new_table_phys = pmm_alloc_block();
        if (!new_table_phys) return;
        
        uint64_t* new_table = (uint64_t*)new_table_phys;
        for (int i = 0; i < 512; i++) {
            new_table[i] = 0;
        }
        
        pdpt[pdpt_idx] = new_table_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER;
        pd = new_table;
    } else {
        pd = (uint64_t*)(pdpt_entry & ~0xFFF);
    }

    uint64_t pd_entry = pd[pd_idx];
    uint64_t* pt = 0;
    if (!(pd_entry & VMM_FLAG_PRESENT)) {
        uint64_t new_table_phys = pmm_alloc_block();
        if (!new_table_phys) return;
        
        uint64_t* new_table = (uint64_t*)new_table_phys;
        for (int i = 0; i < 512; i++) {
            new_table[i] = 0;
        }
        
        pd[pd_idx] = new_table_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER;
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
}

void vmm_map_region_in_pml4(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint32_t size, uint64_t flags) {
    for (uint32_t offset = 0; offset < size; offset += 4096) {
        vmm_map_page_in_pml4(pml4_phys, virt + offset, phys + offset, flags);
    }
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

uint64_t vmm_clone_user_pml4(uint64_t parent_pml4_phys) {
    uint64_t child_pml4_phys = vmm_create_pml4();
    if (!child_pml4_phys) return 0;
    
    uint64_t *parent_pml4 = (uint64_t *)parent_pml4_phys;
    
    for (int i = 0; i < 256; i++) { // user space only
        if (parent_pml4[i] & VMM_FLAG_PRESENT) {
            uint64_t *parent_pdpt = (uint64_t *)(parent_pml4[i] & ~0xFFF);
            for (int j = 0; j < 512; j++) {
                if (parent_pdpt[j] & VMM_FLAG_PRESENT) {
                    uint64_t *parent_pd = (uint64_t *)(parent_pdpt[j] & ~0xFFF);
                    for (int k = 0; k < 512; k++) {
                        if (parent_pd[k] & VMM_FLAG_PRESENT) {
                            uint64_t *parent_pt = (uint64_t *)(parent_pd[k] & ~0xFFF);
                            for (int l = 0; l < 512; l++) {
                                if (parent_pt[l] & VMM_FLAG_PRESENT) {
                                    uint64_t parent_page_phys = parent_pt[l] & ~0xFFF;
                                    uint64_t flags = parent_pt[l] & 0xFFF;
                                    
                                    // Allocate a new page for the child
                                    uint64_t child_page_phys = pmm_alloc_block();
                                    if (!child_page_phys) {
                                        // OOM, return what we have so far
                                        return child_pml4_phys;
                                    }
                                    
                                    // Copy parent page contents to child page
                                    uint8_t *src = (uint8_t *)parent_page_phys;
                                    uint8_t *dst = (uint8_t *)child_page_phys;
                                    for (int m = 0; m < 4096; m++) {
                                        dst[m] = src[m];
                                    }
                                    
                                    // Map the page in the child's PML4
                                    uint64_t virt = ((uint64_t)i << 39) | ((uint64_t)j << 30) | ((uint64_t)k << 21) | ((uint64_t)l << 12);
                                    vmm_map_page_in_pml4(child_pml4_phys, virt, child_page_phys, flags);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return child_pml4_phys;
}

void vmm_free_pml4(uint64_t pml4_phys) {
    uint64_t *pml4 = (uint64_t *)pml4_phys;
    for (int i = 0; i < 256; i++) { // user space only
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

/* -------------------------------------------------------------------------
 * Page Fault Handler
 * ---------------------------------------------------------------------- */

static inline uint64_t pte_flags(uint64_t pte) {
    return pte & 0xFFF;
}

static inline uint64_t pte_phys(uint64_t pte) {
    return pte & ~0xFFF;
}

static void handle_cow_fault(uint64_t pml4_phys, uint64_t virt, uint64_t pte) {
    uint64_t phys = pte_phys(pte);
    uint64_t flags = pte_flags(pte);

    /* Allocate new page */
    uint64_t new_phys = pmm_alloc_block();
    if (!new_phys) {
        screen_log("FAIL", COLOR_LIGHT_RED, "COW: OOM allocating new page");
        return;
    }

    /* Copy contents */
    uint8_t *src = (uint8_t *)phys;
    uint8_t *dst = (uint8_t *)new_phys;
    for (int i = 0; i < 4096; i++) dst[i] = src[i];

    /* Update PTE: new phys, clear COW, set WRITE, keep USER/PRESENT/NX */
    uint64_t new_flags = (flags & ~VMM_FLAG_COW) | VMM_FLAG_WRITE | VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (flags & VMM_FLAG_NX) new_flags |= VMM_FLAG_NX;

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t *pml4 = (uint64_t *)pml4_phys;
    uint64_t *pdpt = (uint64_t*)(pml4[pml4_idx] & ~0xFFF);
    uint64_t *pd   = (uint64_t*)(pdpt[pdpt_idx] & ~0xFFF);
    uint64_t *pt   = (uint64_t*)(pd[pd_idx] & ~0xFFF);

    pt[pt_idx] = (new_phys & ~0xFFF) | new_flags;
    vmm_tlb_flush(virt);
}

static void handle_demand_page_fault(uint64_t pml4_phys, uint64_t virt, uint64_t flags, uint64_t error_code) {
    uint64_t phys = pmm_alloc_block();
    if (!phys) {
        screen_log("FAIL", COLOR_LIGHT_RED, "Demand paging: OOM");
        return;
    }

    /* Zero the page */
    uint8_t *dst = (uint8_t *)phys;
    for (int i = 0; i < 4096; i++) dst[i] = 0;

    uint64_t map_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (flags & VMM_FLAG_WRITE) map_flags |= VMM_FLAG_WRITE;
    if (flags & VMM_FLAG_NX)    map_flags |= VMM_FLAG_NX;

    vmm_map_page_in_pml4(pml4_phys, virt, phys, map_flags);
}

static void handle_stack_growth(uint64_t pml4_phys, uint64_t virt) {
    /* Check if fault is just below current stack (within 1MB guard) */
    if (!current || !current->mm) return;
    uint64_t stack_top = current->mm->start_stack;
    if (stack_top == 0) return;
    if (virt >= stack_top) return; /* Above stack - not stack growth */
    if (stack_top - virt > 1024 * 1024) return; /* Too far - not stack growth */

    handle_demand_page_fault(pml4_phys, virt, VMM_FLAG_WRITE | VMM_FLAG_USER, 0);
}

void vmm_page_fault_handler(uint64_t fault_addr, uint64_t error_code, uint64_t rip)
{
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

    /* Walk page tables to find PTE */
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t *pml4 = (uint64_t *)pml4_phys;
    uint64_t pml4_entry = pml4[pml4_idx];
    if (!(pml4_entry & VMM_FLAG_PRESENT)) {
        /* Page table not present - demand paging */
        handle_demand_page_fault(pml4_phys, virt, 0, error_code);
        return;
    }

    uint64_t *pdpt = (uint64_t*)(pml4_entry & ~0xFFF);
    uint64_t pdpt_entry = pdpt[pdpt_idx];
    if (!(pdpt_entry & VMM_FLAG_PRESENT)) {
        handle_demand_page_fault(pml4_phys, virt, 0, error_code);
        return;
    }

    uint64_t *pd = (uint64_t*)(pdpt_entry & ~0xFFF);
    uint64_t pd_entry = pd[pd_idx];
    if (!(pd_entry & VMM_FLAG_PRESENT)) {
        handle_demand_page_fault(pml4_phys, virt, 0, error_code);
        return;
    }

    uint64_t *pt = (uint64_t*)(pd_entry & ~0xFFF);
    uint64_t pte = pt[pt_idx];

    if (!(pte & VMM_FLAG_PRESENT)) {
        /* Page not present - could be demand paging, COW, or stack growth */
        if (current && current->mm) {
            /* Check for stack growth */
            uint64_t stack_top = current->mm->start_stack;
            if (stack_top && virt < stack_top && (stack_top - virt) <= (1024 * 1024)) {
                handle_stack_growth(pml4_phys, virt);
                return;
            }
        }
        handle_demand_page_fault(pml4_phys, virt, 0, error_code);
        return;
    }

    /* Page present - check for COW */
    if ((pte & VMM_FLAG_COW) && (error_code & PF_ERR_W)) {
        handle_cow_fault(pml4_phys, virt, pte);
        return;
    }

    /* Protection violation (write to read-only, execute NX, etc.) */
    if (current && (error_code & PF_ERR_U)) {
        /* User mode fault — send force signal */
        extern void force_sig(int sig, task_struct_t *t);
        force_sig(SIGBIT_FORCE, current);
        return;
    }
    
    /* Kernel mode fault - panic */
    screen_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
    screen_print("\n!!! KERNEL PAGE FAULT: PROTECTION VIOLATION !!!\n");
    while (1) __asm__ volatile("hlt");
}

/* -------------------------------------------------------------------------
 * COW Support: Mark user pages as COW during fork
 * ---------------------------------------------------------------------- */

void vmm_mark_cow_user_pages(uint64_t pml4_phys)
{
    uint64_t *pml4 = (uint64_t *)pml4_phys;
    for (int i = 0; i < 256; i++) { /* user space only */
        if (!(pml4[i] & VMM_FLAG_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t*)(pml4[i] & ~0xFFF);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & VMM_FLAG_PRESENT)) continue;
            uint64_t *pd = (uint64_t*)(pdpt[j] & ~0xFFF);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & VMM_FLAG_PRESENT)) continue;
                uint64_t *pt = (uint64_t*)(pd[k] & ~0xFFF);
                for (int l = 0; l < 512; l++) {
                    uint64_t pte = pt[l];
                    if (pte & VMM_FLAG_PRESENT) {
                        /* Only mark writable user pages as COW */
                        if ((pte & VMM_FLAG_WRITE) && (pte & VMM_FLAG_USER)) {
                            /* Clear WRITE, set COW */
                            pt[l] = (pte & ~VMM_FLAG_WRITE) | VMM_FLAG_COW;
                        }
                    }
                }
            }
        }
    }
}
