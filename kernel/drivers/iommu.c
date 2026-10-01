/*
 * Intel VT-d IOMMU Driver
 *
 * Provides DMA address translation for Intel IOMMU (VT-d).
 * Identity-maps the entire physical address space by default.
 * Used when DMAR table is present in ACPI.
 *
 * Based on Intel VT-d Specification (3.0).
 */

#include <iommu.h>
#include <acpi.h>
#include <kheap.h>
#include <pmm.h>
#include <vmm.h>
#include <screen.h>
#include <serial.h>
#include <string.h>

/* ================================================================== */
/* Internal state                                                       */
/* ================================================================== */

static volatile uint8_t *vt_d_mmio = NULL;
static uint64_t vt_d_mmio_phys = 0;
static int vt_d_enabled = 0;
static uint32_t vt_d_domain_id = 1;

/* Root table: single entry (domain 1) */
static uint64_t *vt_d_root_table = NULL;
static uint64_t vt_d_root_table_phys = 0;

/* Context table for domain 1 */
static uint64_t *vt_d_context_table = NULL;
static uint64_t vt_d_context_table_phys = 0;

/* Second-level PML4 for domain 1 */
static uint64_t *vt_d_sl_pml4 = NULL;
static uint64_t vt_d_sl_pml4_phys = 0;

/* ================================================================== */
/* MMIO access helpers                                                  */
/* ================================================================== */

static inline uint32_t vt_d_read32(uint32_t offset) {
    return *((volatile uint32_t *)(vt_d_mmio + offset));
}

static inline void vt_d_write32(uint32_t offset, uint32_t val) {
    *((volatile uint32_t *)(vt_d_mmio + offset)) = val;
}

static inline uint64_t vt_d_read64(uint32_t offset) {
    volatile uint32_t *lo = (volatile uint32_t *)(vt_d_mmio + offset);
    volatile uint32_t *hi = (volatile uint32_t *)(vt_d_mmio + offset + 4);
    return ((uint64_t)*hi << 32) | *lo;
}

static inline void vt_d_write64(uint32_t offset, uint64_t val) {
    volatile uint32_t *lo = (volatile uint32_t *)(vt_d_mmio + offset);
    volatile uint32_t *hi = (volatile uint32_t *)(vt_d_mmio + offset + 4);
    *lo = (uint32_t)(val & 0xFFFFFFFF);
    *hi = (uint32_t)(val >> 32);
}

/* ================================================================== */
/* Wait helpers                                                         */
/* ================================================================== */

static void vt_d_wait_sync(uint64_t mask) {
    for (int i = 0; i < 100000; i++) {
        uint32_t status = vt_d_read32(VT_D_GLOBAL_STATUS);
        if (!(status & (uint32_t)mask)) return;
        __asm__ volatile("pause");
    }
    serial_print("VT-d: WARNING: sync timeout\n");
}

/* ================================================================== */
/* Fault logging (disable to avoid storms)                              */
/* ================================================================== */

static void vt_d_disable_faults(void) {
    vt_d_write32(VT_D_FECTL, 0);
    (void)vt_d_read32(VT_D_FECTL);
}

/* ================================================================== */
/* Identity mapping: 4-level page tables                                */
/* ================================================================== */

/* Helper: allocate a zeroed page from PMM (identity-mapped) */
static uint64_t *alloc_page_table(void) {
    uint64_t phys = pmm_alloc_block();
    if (!phys) return NULL;
    uint64_t *virt = (uint64_t *)(uintptr_t)phys;
    memset(virt, 0, VT_D_SL_PAGE_SIZE);
    return virt;
}

/* Map a 4KB page in the second-level page table */
static int sl_map_4k(uint64_t *pml4, uint64_t iova, uint64_t paddr, uint64_t flags) {
    uint64_t pml4_idx = (iova >> 39) & 0x1FF;
    uint64_t pdpt_idx = (iova >> 30) & 0x1FF;
    uint64_t pd_idx   = (iova >> 21) & 0x1FF;
    uint64_t pt_idx   = (iova >> 12) & 0x1FF;

    /* PML4 -> PDPT */
    if (!(pml4[pml4_idx] & VT_D_SL_PTE_P)) {
        uint64_t *pdpt = alloc_page_table();
        if (!pdpt) return -1;
        pml4[pml4_idx] = (uint64_t)(uintptr_t)pdpt | VT_D_SL_PTE_P | VT_D_SL_PTE_W;
    }
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[pml4_idx] & VT_D_SL_PTE_ADDR_MASK);

    /* PDPT -> PD */
    if (!(pdpt[pdpt_idx] & VT_D_SL_PTE_P)) {
        uint64_t *pd = alloc_page_table();
        if (!pd) return -1;
        pdpt[pdpt_idx] = (uint64_t)(uintptr_t)pd | VT_D_SL_PTE_P | VT_D_SL_PTE_W;
    }
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[pdpt_idx] & VT_D_SL_PTE_ADDR_MASK);

    /* PD -> PT */
    if (!(pd[pd_idx] & VT_D_SL_PTE_P)) {
        uint64_t *pt = alloc_page_table();
        if (!pt) return -1;
        pd[pd_idx] = (uint64_t)(uintptr_t)pt | VT_D_SL_PTE_P | VT_D_SL_PTE_W;
    }
    uint64_t *pt = (uint64_t *)(uintptr_t)(pd[pd_idx] & VT_D_SL_PTE_ADDR_MASK);

    /* PT -> Page */
    pt[pt_idx] = (paddr & VT_D_SL_PTE_ADDR_MASK) | flags | VT_D_SL_PTE_P;

    return 0;
}

/* Map a 2MB page in the second-level page table */
static int sl_map_2m(uint64_t *pml4, uint64_t iova, uint64_t paddr, uint64_t flags) {
    uint64_t pml4_idx = (iova >> 39) & 0x1FF;
    uint64_t pdpt_idx = (iova >> 30) & 0x1FF;
    uint64_t pd_idx   = (iova >> 21) & 0x1FF;

    /* PML4 -> PDPT */
    if (!(pml4[pml4_idx] & VT_D_SL_PTE_P)) {
        uint64_t *pdpt = alloc_page_table();
        if (!pdpt) return -1;
        pml4[pml4_idx] = (uint64_t)(uintptr_t)pdpt | VT_D_SL_PTE_P | VT_D_SL_PTE_W;
    }
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[pml4_idx] & VT_D_SL_PTE_ADDR_MASK);

    /* PDPT -> PD */
    if (!(pdpt[pdpt_idx] & VT_D_SL_PTE_P)) {
        uint64_t *pd = alloc_page_table();
        if (!pd) return -1;
        pdpt[pdpt_idx] = (uint64_t)(uintptr_t)pd | VT_D_SL_PTE_P | VT_D_SL_PTE_W;
    }
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[pdpt_idx] & VT_D_SL_PTE_ADDR_MASK);

    /* PD -> 2MB large page */
    pd[pd_idx] = (paddr & 0x000FFFFFFFE00000ULL) | flags | VT_D_SL_PTE_P | VT_D_SL_PTE_PS;

    return 0;
}

/* ================================================================== */
/* TLB Invalidation                                                     */
/* ================================================================== */

static void vt_d_iotlb_flush(void) {
    vt_d_write64(VT_D_IVTLEInv, 0);
    vt_d_wait_sync(VT_D_GSTAT_IRTPS);
}

/* ================================================================== */
/* Initialization                                                       */
/* ================================================================== */

int vt_d_init(uint64_t mmio_base) {
    serial_print("VT-d: Initializing...\n");

    vt_d_mmio_phys = mmio_base;

    /* Map VT-d MMIO registers into kernel space */
    vt_d_mmio = (volatile uint8_t *)(uintptr_t)mmio_base;
    vmm_map_region((uint64_t)(uintptr_t)vt_d_mmio, mmio_base, 0x10000,
                   VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_PCD | VMM_FLAG_PWT);

    /* Read version (MMIO read kept; value only informational for now) */
    (void)vt_d_read32(VT_D_VER);
    serial_print("VT-d: version OK\n");

    /* Disable fault logging (prevent fault storms) */
    vt_d_disable_faults();

    /* Allocate root table (4KB, identity-mapped physical) */
    vt_d_root_table = alloc_page_table();
    if (!vt_d_root_table) {
        serial_print("VT-d: Failed to allocate root table\n");
        return -1;
    }
    vt_d_root_table_phys = (uint64_t)(uintptr_t)vt_d_root_table;

    /* Allocate context table for domain 1 */
    vt_d_context_table = alloc_page_table();
    if (!vt_d_context_table) {
        serial_print("VT-d: Failed to allocate context table\n");
        return -1;
    }
    vt_d_context_table_phys = (uint64_t)(uintptr_t)vt_d_context_table;

    /* Allocate second-level PML4 for domain 1 */
    vt_d_sl_pml4 = alloc_page_table();
    if (!vt_d_sl_pml4) {
        serial_print("VT-d: Failed to allocate SL PML4\n");
        return -1;
    }
    vt_d_sl_pml4_phys = (uint64_t)(uintptr_t)vt_d_sl_pml4;

    /* Set up root table entry for domain 1 */
    vt_d_root_table[0] = vt_d_context_table_phys | VT_D_ROOT_ENTRY_P;

    /* Set up context table entry for domain 1:
     * Second-Level translation, DID=1, SLPTR=PML4 */
    uint64_t ctx_lo = vt_d_sl_pml4_phys | VT_D_CTX_ENTRY_P | VT_D_CTX_ENTRY_TT_SL;
    ctx_lo |= ((uint64_t)vt_d_domain_id << VT_d_CTX_ENTRY_DID_SHIFT);
    vt_d_context_table[0] = ctx_lo;
    vt_d_context_table[1] = 0;

    serial_print("VT-d: Tables allocated\n");

    /* Identity map the first 4GB (covers all device DMA addresses) */
    serial_print("VT-d: Setting up identity mapping for 4GB...\n");
    int ret = vt_d_identity_map(4ULL * 1024 * 1024 * 1024);
    if (ret != 0) {
        serial_print("VT-d: Identity map failed\n");
        return -1;
    }
    serial_print("VT-d: Identity mapping done\n");

    /* Set Root Table Pointer */
    vt_d_write64(VT_D_ROOT_PTR, vt_d_root_table_phys | VT_D_GCMD_SRTP);
    vt_d_wait_sync(VT_D_GSTAT_RTPS);

    /* Set Context Table Pointer */
    vt_d_write32(VT_D_CONTEXT_PTR, (uint32_t)(vt_d_context_table_phys & 0xFFFFFFFF));
    vt_d_write32(VT_D_CONTEXT_PTR_HI, (uint32_t)(vt_d_context_table_phys >> 32));
    uint32_t ctx_cmd = 0x10 | (vt_d_domain_id & 0xFF);
    vt_d_write32(VT_D_CONTEXT_CMD, ctx_cmd);
    vt_d_wait_sync(VT_D_GSTAT_CTPS);

    /* Enable Translation */
    uint32_t gcmd = VT_D_GCMD_TE | VT_D_GCMD_WBF;
    vt_d_write32(VT_D_GLOBAL_CMD, gcmd);
    vt_d_wait_sync(VT_D_GSTAT_TES);

    vt_d_enabled = 1;
    serial_print("VT-d: Translation ENABLED\n");
    screen_log("OK", COLOR_LIGHT_GREEN, "VT-d: IOMMU enabled with identity mapping");

    return 0;
}

/* ================================================================== */
/* Map / Unmap                                                          */
/* ================================================================== */

int vt_d_map(uint64_t iova, uint64_t paddr, size_t size, int prot) {
    if (!vt_d_enabled || !vt_d_sl_pml4) return -1;
    (void)prot;   /* permissões SL-PT por entry: futuro (P2 DMA/IOMMU) */

    uint64_t flags = VT_D_SL_PTE_W | ((uint64_t)VT_D_MT_WRITE_BACK << 12);

    /* Align to 4KB pages */
    uint64_t start = iova & ~0xFFFULL;
    uint64_t end = (iova + size + 0xFFF) & ~0xFFFULL;

    for (uint64_t addr = start; addr < end; addr += 4096) {
        uint64_t phys = paddr + (addr - iova);
        if (sl_map_4k(vt_d_sl_pml4, addr, phys, flags) != 0) {
            serial_print("VT-d: map failed\n");
            return -1;
        }
    }

    vt_d_iotlb_flush();
    return 0;
}

void vt_d_unmap(uint64_t iova, size_t size) {
    if (!vt_d_enabled || !vt_d_sl_pml4) return;

    uint64_t start = iova & ~0xFFFULL;
    uint64_t end = (iova + size + 0xFFF) & ~0xFFFULL;

    for (uint64_t addr = start; addr < end; addr += 4096) {
        uint64_t pml4_idx = (addr >> 39) & 0x1FF;
        uint64_t pdpt_idx = (addr >> 30) & 0x1FF;
        uint64_t pd_idx   = (addr >> 21) & 0x1FF;
        uint64_t pt_idx   = (addr >> 12) & 0x1FF;

        if (!(vt_d_sl_pml4[pml4_idx] & VT_D_SL_PTE_P)) continue;
        uint64_t *pdpt = (uint64_t *)(uintptr_t)(vt_d_sl_pml4[pml4_idx] & VT_D_SL_PTE_ADDR_MASK);

        if (!(pdpt[pdpt_idx] & VT_D_SL_PTE_P)) continue;
        uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[pdpt_idx] & VT_D_SL_PTE_ADDR_MASK);

        if (pd[pd_idx] & VT_D_SL_PTE_PS) {
            pd[pd_idx] = 0;
            continue;
        }

        if (!(pd[pd_idx] & VT_D_SL_PTE_P)) continue;
        uint64_t *pt = (uint64_t *)(uintptr_t)(pd[pd_idx] & VT_D_SL_PTE_ADDR_MASK);

        pt[pt_idx] = 0;
    }

    vt_d_iotlb_flush();
}

/* ================================================================== */
/* Identity map entire physical address space                           */
/* ================================================================== */

int vt_d_identity_map(uint64_t max_phys_addr) {
    if (!vt_d_sl_pml4) return -1;

    uint64_t flags = VT_D_SL_PTE_W | ((uint64_t)VT_D_MT_WRITE_BACK << 12);

    /* Use 2MB pages where possible for efficiency */
    for (uint64_t addr = 0; addr < max_phys_addr; addr += 0x200000ULL) {
        if (addr + 0x200000ULL <= max_phys_addr) {
            if (sl_map_2m(vt_d_sl_pml4, addr, addr, flags) != 0) {
                /* Fallback to 4KB pages for this region */
                for (uint64_t p = addr; p < addr + 0x200000ULL; p += 0x1000) {
                    if (sl_map_4k(vt_d_sl_pml4, p, p, flags) != 0) {
                        serial_print("VT-d: identity map 4KB failed\n");
                        return -1;
                    }
                }
            }
        } else {
            for (uint64_t p = addr; p < max_phys_addr; p += 0x1000) {
                if (sl_map_4k(vt_d_sl_pml4, p, p, flags) != 0) return -1;
            }
        }
    }

    return 0;
}

/* ================================================================== */
/* Status                                                               */
/* ================================================================== */

int vt_d_is_enabled(void) {
    return vt_d_enabled;
}

uint64_t vt_d_get_mmio_base(void) {
    return vt_d_mmio_phys;
}
