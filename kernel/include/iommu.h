#ifndef IOMMU_H
#define IOMMU_H

#include <stdint.h>
#include <stddef.h>

/* ================================================================== */
/* Intel VT-d Register Definitions                                      */
/* ================================================================== */

/* VT-d MMIO Register Offsets (Intel VT-d Spec) */
#define VT_D_VER             0x000  /* Version Register */
#define VT_D_CAP             0x008  /* Capability Register */
#define VT_D_EXT_CAP         0x010  /* Extended Capability Register */
#define VT_D_GLOBAL_CMD      0x018  /* Global Command Register */
#define VT_D_GLOBAL_STATUS   0x01C  /* Global Status Register */
#define VT_D_FECTL           0x034  /* Fault Event Control */
#define VT_D_FEDATA          0x038  /* Fault Event Data */
#define VT_D_FEADDR          0x03C  /* Fault Event Address */
#define VT_D_FEUADDR         0x040  /* Fault Event Upper Address */
#define VT_D_AFLOG           0x058  /* Advanced Fault Control */
#define VT_D_PMEN            0x064  /* Page Snoop Control */

/* Context Table */
#define VT_D_CONTEXT_CMD     0x028  /* Context Command */
#define VT_D_CONTEXT_PTR     0x068  /* Context Table Pointer (low) */
#define VT_D_CONTEXT_PTR_HI  0x06C  /* Context Table Pointer (high) */

/* Root Table */
#define VT_D_ROOT_PTR        0x078  /* Root Table Pointer (low) */
#define VT_D_ROOT_PTR_HI     0x07C  /* Root Table Pointer (high) */

/* IOTLB Invalidation */
#define VT_D_IOTLB_INV       0x008  /* IOTLB Invalidation */
#define VT_D_IOTLB_INV_HI    0x00C  /* IOTLB Invalidation High */
#define VT_D_IVTLEInv        0x048  /* IVT Level Invalidation */
#define VT_D_IVTLEInv_HI     0x04C  /* IVT Level Invalidation High */

/* VT-d Global Command Register bits */
#define VT_D_GCMD_TE         (1 << 0)   /* Translation Enable */
#define VT_D_GCMD_SRTP       (1 << 3)   /* Set Root Table Pointer */
#define VT_D_GCMD_SIRTP      (1 << 4)   /* Set Context Table Pointer */
#define VT_D_GCMD_SFL        (1 << 5)   /* Set Fault Log */
#define VT_D_GCMD_EAFL       (1 << 6)   /* Enable Advanced Fault Log */
#define VT_D_GCMD_WBF        (1 << 7)   /* Write Buffer Flush */
#define VT_D_GCMD_QI         (1 << 9)   /* Queued Invalidation Enable */
#define VT_D_GCMD_DI          (1 << 10)  /* Disable Invalidation */

/* VT-d Global Status Register bits */
#define VT_D_GSTAT_TES       (1 << 0)   /* Translation Enabled Status */
#define VT_D_GSTAT_RTPS      (1 << 3)   /* Root Table Pointer Status */
#define VT_D_GSTAT_CTPS      (1 << 4)   /* Context Table Pointer Status */
#define VT_D_GSTAT_FLS       (1 << 5)   /* Fault Log Status */
#define VT_D_GSTAT_WBFS      (1 << 7)   /* Write Buffer Flush Status */
#define VT_D_GSTAT_QIES      (1 << 9)   /* Queued Invalidation Status */
#define VT_D_GSTAT_IRTPS     (1 << 12)  /* IRT Override Status */

/* VT-d Capability bits */
#define VT_D_CAP_READ_DR     (1 << 3)   /* Read Draining */
#define VT_D_CAP_WRITE_DR    (1 << 4)   /* Write Draining */
#define VT_D_CAP_PT          (1 << 6)   /* Page Snoop */
#define VT_D_CAP_SC          (1 << 7)   /* Snoop Control */
#define VT_D_CAP_MGAW        (25)       /* Mask Guest Address Width (bit 25) */
#define VT_D_CAP_MGAW_MASK   (0x3FULL << 25)
#define VT_D_CAP_SAGAW       (18)       /* Supported Adjusted Guest Address Width */
#define VT_D_CAP_SAGAW_MASK  (0x1FULL << 18)
#define VT_D_CAP_FW          (1 << 34)  /* Fault Logging */
#define VT_D_CAP_PI          (1 << 59)  /* Posted Interrupts */

/* VT-d Context Entry flags */
#define VT_D_CTX_TT_SWIO     0x1  /* Software Walking IOTLB */
#define VT_D_CTX_TT_FWD      0x2  /* First-Level / Nested */

/* ================================================================== */
/* Page Table Entry Flags (VT-d)                                        */
/* ================================================================== */

/* Second-level page table entry */
#define VT_D_SL_PTE_P        (1ULL << 0)    /* Present */
#define VT_D_SL_PTE_W        (1ULL << 1)    /* Write */
#define VT_D_SL_PTE_US       (1ULL << 2)    /* User Supervisor */
#define VT_D_SL_PTE_PWT      (1ULL << 3)    /* Page Write-Through */
#define VT_D_SL_PTE_PCD      (1ULL << 4)    /* Page Cache Disable */
#define VT_D_SL_PTE_ACC      (1ULL << 5)    /* Accessed */
#define VT_D_SL_PTE_D        (1ULL << 6)    /* Dirty */
#define VT_D_SL_PTE_PS       (1ULL << 7)    /* Page Size (1=2MB) */
#define VT_D_SL_PTE_MT_MASK  (0FULL << 12)  /* Memory Type mask */
#define VT_D_SL_PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL

/* Memory types for VT-d (same as MTRR) */
#define VT_D_MT_UNCACHABLE   0
#define VT_D_MT_WRITE_COMBINE 1
#define VT_D_MT_WRITE_THROUGH 4
#define VT_D_MT_WRITE_PROTECT 5
#define VT_D_MT_WRITE_BACK   6

/* ================================================================== */
/* VT-d Structures                                                     */
/* ================================================================== */

/* Root Table Entry */
typedef struct vt_d_root_entry {
    uint64_t val[2];    /* [0] = lower 64 bits, [1] = upper (bits 63:12) */
} vt_d_root_entry_t;

#define VT_D_ROOT_ENTRY_P  (1ULL << 0)
#define VT_D_ROOT_ENTRY_CTP_MASK  0x000FFFFFFFFFF000ULL

/* Context Table Entry */
typedef struct vt_d_context_entry {
    uint64_t val[2];
} vt_d_context_entry_t;

#define VT_D_CTX_ENTRY_P    (1ULL << 0)
#define VT_D_CTX_ENTRY_TT_MASK  (3ULL << 2)
#define VT_D_CTX_ENTRY_TT_SL   (0ULL << 2)
#define VT_D_CTX_ENTRY_TT_FL   (2ULL << 2)
#define VT_d_CTX_ENTRY_DID_SHIFT 8
#define VT_D_CTX_ENTRY_SLPTR_MASK 0x000FFFFFFFFFF000ULL

/* Second-Level Page Table (4-level, like x86-64) */
#define VT_D_SL_PAGE_SIZE    4096
#define VT_D_SL_PT_ENTRIES   512

/* ================================================================== */
/* VT-d Device State                                                   */
/* ================================================================== */

typedef struct vt_d_domain {
    uint64_t root_table_phys;     /* Physical address of root table */
    uint64_t *context_table;      /* Virtual address of context table (single root entry) */
    uint64_t context_table_phys;  /* Physical address */
    /* Second-level page table for this domain */
    uint64_t *sl_pml4;           /* Virtual address of PML4 */
    uint64_t sl_pml4_phys;       /* Physical address */
} vt_d_domain_t;

/* ================================================================== */
/* IOMMU Public API                                                     */
/* ================================================================== */

/* Initialize Intel VT-d IOMMU (called from acpi_init or dma_init) */
int vt_d_init(uint64_t mmio_base);

/* Map IOVA → Physical address in the IOMMU page table */
int vt_d_map(uint64_t iova, uint64_t paddr, size_t size, int prot);

/* Unmap IOVA */
void vt_d_unmap(uint64_t iova, size_t size);

/* Identity map the entire physical address space (boot/init) */
int vt_d_identity_map(uint64_t max_phys_addr);

/* Get IOMMU status */
int vt_d_is_enabled(void);

/* Get the MMIO base address */
uint64_t vt_d_get_mmio_base(void);

#endif
