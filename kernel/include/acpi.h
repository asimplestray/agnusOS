#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include <stddef.h>

/* ================================================================== */
/* ACPI Table Signatures                                                */
/* ================================================================== */

#define ACPI_SIG_RSDP  "RSD PTR "
#define ACPI_SIG_RSDT  "RSDT"
#define ACPI_SIG_XSDT  "XSDT"
#define ACPI_SIG_DMAR  "DMAR"
#define ACPI_SIG_MADT  "APIC"
#define ACPI_SIG_FADT  "FACP"

/* ================================================================== */
/* RSDP — Root System Description Pointer (ACPI 1.0 / 2.0)             */
/* ================================================================== */

typedef struct acpi_rsdp {
    char     signature[8];      /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;          /* 0 = ACPI 1.0, 2 = ACPI 2.0+ */
    uint32_t rsdt_address;      /* ACPI 1.0: physical address of RSDT */
    uint32_t length;            /* ACPI 2.0+: total length of RSDP */
    uint64_t xsdt_address;      /* ACPI 2.0+: physical address of XSDT */
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

/* ================================================================== */
/* ACPI Table Header (common to all tables)                            */
/* ================================================================== */

typedef struct acpi_table_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    char     creator_id[4];
    uint32_t creator_revision;
} __attribute__((packed)) acpi_table_header_t;

/* ================================================================== */
/* RSDT / XSDT — Root/Extended System Description Table                */
/* ================================================================== */

typedef struct acpi_rsdt {
    acpi_table_header_t header;
    uint32_t entries[0];        /* Physical addresses of other tables */
} __attribute__((packed)) acpi_rsdt_t;

typedef struct acpi_xsdt {
    acpi_table_header_t header;
    uint64_t entries[0];        /* Physical addresses of other tables (ACPI 2.0+) */
} __attribute__((packed)) acpi_xsdt_t;

/* ================================================================== */
/* DMAR — DMA Remapping Reporting Table                                */
/* ================================================================== */

/* DMAR table structure */
typedef struct acpi_table_dmar {
    acpi_table_header_t header;
    uint8_t  host_address_width; /* Maximum DMA physical address ability */
    uint8_t  flags;
    uint8_t  reserved[10];
} __attribute__((packed)) acpi_table_dmar_t;

/* DMAR flags */
#define DMAR_FLAGS_INTR_REMAP  (1 << 0)
#define DMAR_FLAGS_X2APIC_OPT  (1 << 1)

/* DMAR Device Scope Entry types */
#define DMAR_SCOPE_ENDPOINT     0x01
#define DMAR_SCOPE_BRIDGE       0x02
#define DMAR_SCOPE_IOAPIC       0x03
#define DMAR_SCOPE_MSI_CAPABLE  0x04
#define DMAR_SCOPE_NAMESPACE    0x05

/* DMAR Remapping Hardware Structure types */
#define DMAR_DRHD_TYPE    0   /* DMAR Remapping Hardware Unit */
#define DMAR_RMRR_TYPE    1   /* Reserved Memory Region Reporting */
#define DMAR_ATSR_TYPE    2   /* ACS: Root Port Special Capabilities */
#define DMAR_RHSA_TYPE    3   /* Remapping Hardware Static Affinity */
#define DMAR_ANDD_TYPE    4   /* ACPI Name-space Device Declaration */

/* DMAR Remapping Hardware Unit Definition (DRHD) */
typedef struct acpi_dmar_hardware_unit {
    uint8_t  type;              /* DMAR_DRHD_TYPE = 0 */
    uint8_t  length;
    uint8_t  flags;
    uint8_t  reserved;
    uint16_t segment;           /* PCI Segment Number */
    uint64_t register_base;     /* Base address of remapping hardware */
} __attribute__((packed)) acpi_dmar_hardware_unit_t;

/* DMAR DRHD flags */
#define DMAR_DRHD_INCLUDE_ALL  (1 << 0)

/* DMAR Device Scope Entry */
typedef struct acpi_dmar_device_scope {
    uint8_t  type;
    uint8_t  length;
    uint16_t enumeration_id;
    uint8_t  bus;               /* PCI Bus Number */
    uint8_t  path[0];           /* PCI Device/Function pairs */
} __attribute__((packed)) acpi_dmar_device_scope_t;

/* DMAR Reserved Memory Region Reporting (RMRR) */
typedef struct acpi_dmar_reserved_memory {
    uint8_t  type;              /* DMAR_RMRR_TYPE = 1 */
    uint8_t  length;
    uint16_t reserved;
    uint16_t segment;
    uint64_t reserved_memory_base;
    uint64_t reserved_memory_limit;
} __attribute__((packed)) acpi_dmar_reserved_memory_t;

/* ================================================================== */
/* DMAR parsed IOMMU unit info                                          */
/* ================================================================== */

#define ACPI_MAX_IOMMU_UNITS  4

typedef struct acpi_iommu_unit {
    uint16_t segment;
    uint64_t mmio_base;        /* Physical address of VT-d MMIO registers */
    uint8_t  flags;
    uint8_t  bus;              /* Device scope bus (0xFF = include-all) */
    uint8_t  dev;
    uint8_t  func;
    int      enabled;
} acpi_iommu_unit_t;

/* ================================================================== */
/* ACPI Public API                                                     */
/* ================================================================== */

/* Initialize ACPI — parse RSDP from multiboot2, discover tables */
int acpi_init(uint64_t multiboot_info);

/* Find a table by signature (from XSDT or RSDT) */
acpi_table_header_t *acpi_find_table(const char *signature);

/* Get parsed IOMMU units (from DMAR table) */
int acpi_get_iommu_units(acpi_iommu_unit_t *units, int max_units);

/* Get host address width from DMAR */
int acpi_dmar_get_max_addr_width(void);

#endif
