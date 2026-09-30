#include <vmm.h>
#include <pmm.h>
#include <screen.h>
#include <serial.h>
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

/* Large-page helpers: o mapeador abaixo só entende páginas 4K, mas o boot
 * instala identity mapping com páginas grandes (2 MiB). Sem isto, descer
 * numa entrada PS trata memória de página como memória de tabela:
 * corrompe o conteúdo e retorna sucesso bogus (foi exatamente o que
 * quebrou o primeiro spawn Ring 3 em 0x400000: #PF U/S no iret). */
#define VMM_FLAG_PS        (1ULL << 7)
#define VMM_ADDR_MASK      0x000FFFFFFFFFF000ULL
#define VMM_FLAG_KEEP_SPLIT (VMM_FLAG_PRESENT | VMM_FLAG_WRITE | \
                             VMM_FLAG_USER | VMM_FLAG_PWT | VMM_FLAG_PCD | \
                             VMM_FLAG_NX | (1ULL << 8)) /* +G: transparente */

void vmm_init(void) {
    // Read the active PML4 physical address from CR3 (identity mapped during boot)
    kernel_pml4_phys = read_cr3();
    active_pml4 = (uint64_t*)kernel_pml4_phys;
    screen_log(" OK ", COLOR_LIGHT_GREEN, "Virtual Memory Manager (VMM) initialized successfully.");
}

/* Auditoria de isolamento do PML4 do kernel (P0 §3, diagnóstico de boot).
 * Percorre as tabelas a partir do PML4 ativo e conta entradas PRESENT com
 * bit USER. Invariante: o address space do próprio kernel não tem nenhuma
 * (half alto supervisor-only; half baixo do kernel sem mappings de user).
 * Páginas grandes (PS em PDPT/PD) contam como folhas. Só reporta via
 * serial/tela — sem PANIC: é gate de diagnóstico enquanto o §3 não fecha
 * (PML4s de usuário ainda forçam USER nos ancestrais compartilhados da
 * entry 0; wart documentado em vmm_map_page_in_pml4). */
void vmm_audit_isolation(void) {
    if (!kernel_pml4_phys) {
        serial_print("[VMM-AUDIT] SKIP: sem PML4 do kernel\n");
        return;
    }
    uint64_t user_entries = 0, user_leaves = 0, present_top = 0;
    uint64_t *pml4 = (uint64_t *)kernel_pml4_phys;
    for (int i = 0; i < 512; i++) {
        uint64_t e = pml4[i];
        if (!(e & VMM_FLAG_PRESENT))
            continue;
        present_top++;
        if (e & VMM_FLAG_USER)
            user_entries++;
        uint64_t *pdpt = (uint64_t *)(e & VMM_ADDR_MASK);
        for (int j = 0; j < 512; j++) {
            uint64_t e1 = pdpt[j];
            if (!(e1 & VMM_FLAG_PRESENT))
                continue;
            if (e1 & VMM_FLAG_USER)
                user_entries++;
            if (e1 & VMM_FLAG_PS) {
                if (e1 & VMM_FLAG_USER)
                    user_leaves++;
                continue;
            }
            uint64_t *pd = (uint64_t *)(e1 & VMM_ADDR_MASK);
            for (int k = 0; k < 512; k++) {
                uint64_t e2 = pd[k];
                if (!(e2 & VMM_FLAG_PRESENT))
                    continue;
                if (e2 & VMM_FLAG_USER)
                    user_entries++;
                if (e2 & VMM_FLAG_PS) {
                    if (e2 & VMM_FLAG_USER)
                        user_leaves++;
                    continue;
                }
                uint64_t *pt = (uint64_t *)(e2 & VMM_ADDR_MASK);
                for (int l = 0; l < 512; l++) {
                    if ((pt[l] & VMM_FLAG_PRESENT) && (pt[l] & VMM_FLAG_USER))
                        user_leaves++;
                }
            }
        }
    }

    /* Readback das proteções de CPU ligadas por cpu_harden(). */
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

    char nbuf[24];
    serial_print("[VMM-AUDIT] top-present=");
    itoa((int64_t)present_top, nbuf, 10);
    serial_print(nbuf);
    serial_print(" user-entries=");
    itoa((int64_t)user_entries, nbuf, 10);
    serial_print(nbuf);
    serial_print(" user-leaves=");
    itoa((int64_t)user_leaves, nbuf, 10);
    serial_print(nbuf);
    serial_print((cr0 & (1ULL << 16)) ? " WP=1" : " WP=0");
    serial_print((cr4 & (1ULL << 20)) ? " SMEP=1\n" : " SMEP=0\n");

    if (user_entries == 0 && user_leaves == 0)
        screen_log("OK", COLOR_LIGHT_GREEN, "VMM-AUDIT PASS (kernel PML4 sem USER).");
    else
        screen_log("FAIL", COLOR_LIGHT_RED, "VMM-AUDIT falhou (USER no PML4 do kernel).");
}

/* Divide uma entrada PDPT 1 GiB em uma PD de 512×2 MiB. Retorna false em OOM. */
static bool split_pdpt_entry(uint64_t *pdpt, uint64_t idx) {
    uint64_t e = pdpt[idx];
    if (!(e & VMM_FLAG_PRESENT) || !(e & VMM_FLAG_PS))
        return true;  /* nada a fazer */
    uint64_t new_pd_phys = pmm_alloc_block();
    if (!new_pd_phys) return false;
    uint64_t *new_pd = (uint64_t *)new_pd_phys;
    uint64_t base = e & 0xFFFFFC0000000ULL; /* bits 30+ */
    uint64_t keep = e & VMM_FLAG_KEEP_SPLIT;
    for (int i = 0; i < 512; i++)
        new_pd[i] = (base + (uint64_t)i * 0x200000ULL) | VMM_FLAG_PS | keep;
    /* Ponteiro de tabela: U forçado — a CPU exige U/S=1 em TODOS os níveis
     * para acesso Ring 3 (a página grande original do kernel é supervisor).
     * NX preservado do original. Sem isto, o iret para Ring 3 falha com
     * #PF U/S mesmo com a PTE final P|U. */
    pdpt[idx] = new_pd_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITE |
                VMM_FLAG_USER | (keep & VMM_FLAG_NX);
    return true;
}

/* Divide uma entrada PD 2 MiB em uma PT de 512×4 KiB. Retorna false em OOM. */
static bool split_pd_entry(uint64_t *pd, uint64_t idx) {
    uint64_t e = pd[idx];
    if (!(e & VMM_FLAG_PRESENT) || !(e & VMM_FLAG_PS))
        return true;  /* nada a fazer */
    uint64_t new_pt_phys = pmm_alloc_block();
    if (!new_pt_phys) return false;
    uint64_t *new_pt = (uint64_t *)new_pt_phys;
    uint64_t base = e & 0xFFFFFFFFFFE00000ULL; /* bits 21+ */
    uint64_t keep = e & VMM_FLAG_KEEP_SPLIT;
    keep &= ~VMM_FLAG_PS;
    for (int i = 0; i < 512; i++)
        new_pt[i] = (base + (uint64_t)i * 0x1000ULL) | keep;
    /* Ponteiro de tabela: U forçado (ver split_pdpt_entry). */
    pd[idx] = new_pt_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITE |
              VMM_FLAG_USER | (keep & VMM_FLAG_NX);
    return true;
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
        /* Caminho USER por tabelas compartilhadas (entry 0 herdada do kernel):
         * a CPU exige U/S=1 em TODOS os níveis da caminhada. Marca o ancestral.
         * Tradeoff M1 documentado: isto expõe as páginas supervisoras irmãs ao
         * Ring 3 no MMU; a invariante de segurança segue no access_ok(), que
         * exige a FOLHA P|U (isolamento real é o épico §3). */
        if ((flags & VMM_FLAG_USER) && !(pml4[pml4_idx] & VMM_FLAG_USER))
            pml4[pml4_idx] |= VMM_FLAG_USER;
        pdpt = (uint64_t*)(pml4[pml4_idx] & VMM_ADDR_MASK);
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
        /* Entrada grande (1 GiB)? Divide antes de descer — ver split_*. */
        if ((pdpt_entry & VMM_FLAG_PS) &&
            !split_pdpt_entry(pdpt, pdpt_idx))
            return false;
        if ((flags & VMM_FLAG_USER) && !(pdpt[pdpt_idx] & VMM_FLAG_USER))
            pdpt[pdpt_idx] |= VMM_FLAG_USER;
        pd = (uint64_t*)(pdpt[pdpt_idx] & VMM_ADDR_MASK);
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
        /* Entrada grande (2 MiB)? Divide antes de descer — ver split_*. */
        if ((pd_entry & VMM_FLAG_PS) &&
            !split_pd_entry(pd, pd_idx))
            return false;
        if ((flags & VMM_FLAG_USER) && !(pd[pd_idx] & VMM_FLAG_USER))
            pd[pd_idx] |= VMM_FLAG_USER;
        pt = (uint64_t*)(pd[pd_idx] & VMM_ADDR_MASK);
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

    uint64_t* pdpt = (uint64_t*)(pml4_entry & VMM_ADDR_MASK);
    uint64_t pdpt_entry = pdpt[pdpt_idx];
    if (!(pdpt_entry & VMM_FLAG_PRESENT)) return;
    /* Página grande? Divide primeiro (simetria com o map); OOM = no-op. */
    if ((pdpt_entry & VMM_FLAG_PS) && !split_pdpt_entry(pdpt, pdpt_idx))
        return;

    uint64_t* pd = (uint64_t*)(pdpt[pdpt_idx] & VMM_ADDR_MASK);
    uint64_t pd_entry = pd[pd_idx];
    if (!(pd_entry & VMM_FLAG_PRESENT)) return;
    if ((pd_entry & VMM_FLAG_PS) && !split_pd_entry(pd, pd_idx))
        return;

    uint64_t* pt = (uint64_t*)(pd[pd_idx] & VMM_ADDR_MASK);
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
            uint64_t *pdpt = (uint64_t *)(pml4[i] & VMM_ADDR_MASK);
            for (int j = 0; j < 512; j++) {
                if (pdpt[j] & VMM_FLAG_PRESENT) {
                    uint64_t *pd = (uint64_t *)(pdpt[j] & VMM_ADDR_MASK);
                    for (int k = 0; k < 512; k++) {
                        if (pd[k] & VMM_FLAG_PRESENT) {
                            uint64_t *pt = (uint64_t *)(pd[k] & VMM_ADDR_MASK);
                            for (int l = 0; l < 512; l++) {
                                if (pt[l] & VMM_FLAG_PRESENT) {
                                    /* Máscara limpa: páginas NX (bit 63) não
                                     * podem vazar para o endereço físico. */
                                    uint64_t phys_page = pt[l] & VMM_ADDR_MASK;
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

    uint64_t* pdpt = (uint64_t*)(pml4[pml4_idx] & VMM_ADDR_MASK);
    if (!(pdpt[pdpt_idx] & VMM_FLAG_PRESENT)) return 0;
    if (pdpt[pdpt_idx] & VMM_FLAG_PS)
        return (pdpt[pdpt_idx] & 0xFFFFFC0000000ULL) +
               (virt & 0x3FFFFFFFULL);

    uint64_t* pd = (uint64_t*)(pdpt[pdpt_idx] & VMM_ADDR_MASK);
    if (!(pd[pd_idx] & VMM_FLAG_PRESENT)) return 0;
    if (pd[pd_idx] & VMM_FLAG_PS)
        return (pd[pd_idx] & 0xFFFFFFFFFFE00000ULL) + (virt & 0x1FFFFFULL);

    uint64_t* pt = (uint64_t*)(pd[pd_idx] & VMM_ADDR_MASK);
    if (!(pt[pt_idx] & VMM_FLAG_PRESENT)) return 0;

    return pt[pt_idx] & VMM_ADDR_MASK;
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
    pdpt = (uint64_t *)(pml4[pml4_idx] & VMM_ADDR_MASK);
    if (!(pdpt[pdpt_idx] & VMM_FLAG_PRESENT))
        return 0;
    if (pdpt[pdpt_idx] & VMM_FLAG_PS)
        return pdpt[pdpt_idx];  /* mapeamento 1 GiB: a própria entrada */
    pd = (uint64_t *)(pdpt[pdpt_idx] & VMM_ADDR_MASK);
    if (!(pd[pd_idx] & VMM_FLAG_PRESENT))
        return 0;
    if (pd[pd_idx] & VMM_FLAG_PS)
        return pd[pd_idx];  /* mapeamento 2 MiB: a própria entrada */
    pt = (uint64_t *)(pd[pd_idx] & VMM_ADDR_MASK);
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
