/*
 * ACPI — Table discovery and parsing
 *
 * RSDP discovery via EBDA/BIOS ROM scan (Limine does not pass
 * ACPI tags via multiboot2). Parses XSDT/RSDT → DMAR table.
 * Extracts IOMMU unit info (DRHD) for Intel VT-d initialization.
 */

#include <acpi.h>
#include <multiboot2.h>
#include <screen.h>
#include <serial.h>
#include <string.h>
#include <vmm.h>

/* ================================================================== */
/* Internal state                                                       */
/* ================================================================== */

static acpi_rsdp_t *acpi_rsdp = NULL;
static acpi_table_header_t *acpi_xsdt = NULL;
static acpi_table_header_t *acpi_rsdt = NULL;
static int acpi_xsdt_tables = 0;

static acpi_iommu_unit_t iommu_units[ACPI_MAX_IOMMU_UNITS];
static int iommu_unit_count = 0;
static int dmar_max_addr_width = 0;

/* ================================================================== */
/* Helpers                                                              */
/* ================================================================== */

static uint8_t acpi_checksum(void *table, uint32_t length) {
    uint8_t *p = (uint8_t *)table;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; i++) sum += p[i];
    return sum;
}

/* Safe probe: read 8 bytes from a physical address, returns 0 on fault */
static uint64_t safe_read_u64(uint64_t phys) {
    if (phys == 0 || phys >= 0x80000000ULL) return 0;
    return *(volatile uint64_t *)(uintptr_t)phys;
}

static uint32_t safe_read_u32(uint64_t phys) {
    if (phys == 0 || phys >= 0x80000000ULL) return 0;
    return *(volatile uint32_t *)(uintptr_t)phys;
}

/* ================================================================== */
/* RSDP scan: classic EBDA + BIOS ROM approach                          */
/* ================================================================== */

static acpi_rsdp_t *validate_rsdp(uint64_t phys) {
    if (phys == 0 || phys >= 0x80000000ULL) return NULL;
    acpi_rsdp_t *rsdp = (acpi_rsdp_t *)(uintptr_t)phys;
    /* Must start with "RSD PTR " signature */
    if (memcmp(rsdp->signature, ACPI_SIG_RSDP, 8) != 0) return NULL;
    /* ACPI 2.0 checksum (full struct) */
    if (rsdp->length >= 36 && acpi_checksum(rsdp, rsdp->length) == 0) return rsdp;
    /* ACPI 1.0 checksum (first 20 bytes) */
    if (acpi_checksum(rsdp, 20) == 0) return rsdp;
    return NULL;
}

static acpi_rsdp_t *find_rsdp_multiboot(uint64_t mbi) {
    /* Try ACPI 2.0+ (tag type 15) */
    struct multiboot_tag *tag = multiboot_find_tag(mbi, MULTIBOOT_TAG_TYPE_ACPI_NEW);
    if (tag) {
        struct multiboot_tag_acpi *acpi_tag = (struct multiboot_tag_acpi *)tag;
        uint64_t rsdp_phys = acpi_tag->rsdp;
        acpi_rsdp_t *rsdp = validate_rsdp(rsdp_phys);
        if (rsdp) {
            serial_print("ACPI: RSDP found via multiboot2 tag 15\n");
            return rsdp;
        }
    }

    /* Try ACPI 1.0 (tag type 14) */
    tag = multiboot_find_tag(mbi, MULTIBOOT_TAG_TYPE_ACPI_OLD);
    if (tag) {
        struct multiboot_tag_acpi *acpi_tag = (struct multiboot_tag_acpi *)tag;
        uint64_t rsdp_phys = acpi_tag->rsdp;
        acpi_rsdp_t *rsdp = validate_rsdp(rsdp_phys);
        if (rsdp) {
            serial_print("ACPI: RSDP found via multiboot2 tag 14\n");
            return rsdp;
        }
    }

    return NULL;
}

/* Scan EBDA segment and BIOS ROM area for RSDP signature */
static acpi_rsdp_t *find_rsdp_scan(void) {
    /* Read EBDA address from BIOS Data Area (physical 0x40E) */
    uint16_t ebda_seg = *(volatile uint16_t *)(uintptr_t)0x40E;
    if (ebda_seg > 0 && ebda_seg < 0xA000) {
        uint64_t ebda = (uint64_t)ebda_seg << 4;
        serial_print("ACPI: scanning EBDA...\n");
        /* EBDA is typically 128KB or less */
        for (uint64_t addr = ebda; addr < ebda + 0x20000; addr += 16) {
            acpi_rsdp_t *rsdp = validate_rsdp(addr);
            if (rsdp) {
                serial_print("ACPI: RSDP found in EBDA\n");
                return rsdp;
            }
        }
    }

    /* Scan BIOS ROM area: 0xE0000 - 0xFFFFF */
    serial_print("ACPI: scanning BIOS ROM (0xE0000-0xFFFFF)...\n");
    for (uint64_t addr = 0xE0000; addr < 0x100000; addr += 16) {
        acpi_rsdp_t *rsdp = validate_rsdp(addr);
        if (rsdp) {
            serial_print("ACPI: RSDP found in BIOS ROM\n");
            return rsdp;
        }
    }

    return NULL;
}

static acpi_rsdp_t *find_rsdp(uint64_t mbi) {
    serial_print("ACPI: searching for RSDP...\n");

    /* 1. Try multiboot2 tags first */
    acpi_rsdp_t *rsdp = find_rsdp_multiboot(mbi);
    if (rsdp) return rsdp;

    /* 2. Fallback: scan EBDA and BIOS ROM */
    return find_rsdp_scan();
}

/* ================================================================== */
/* XSDT / RSDT parsing                                                 */
/* ================================================================== */

static void map_table_page(uint64_t phys) {
    vmm_map_page(phys & ~0xFFFULL, phys & ~0xFFFULL,
                 VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
}

static int parse_xsdt(void) {
    if (!acpi_rsdp || acpi_rsdp->xsdt_address == 0) return -1;

    uint64_t xsdt_phys = acpi_rsdp->xsdt_address;
    map_table_page(xsdt_phys);
    acpi_xsdt = (acpi_table_header_t *)(uintptr_t)xsdt_phys;
    if (memcmp(acpi_xsdt->signature, "XSDT", 4) != 0) {
        acpi_xsdt = NULL;
        return -1;
    }
    if (acpi_checksum(acpi_xsdt, acpi_xsdt->length) != 0) {
        acpi_xsdt = NULL;
        return -1;
    }

    uint32_t header_size = sizeof(acpi_table_header_t);
    uint32_t table_size = acpi_xsdt->length;
    acpi_xsdt_tables = (table_size > header_size + 8) ? (table_size - header_size) / 8 : 0;

    serial_print("ACPI: XSDT found\n");
    return 0;
}

static int parse_rsdt(void) {
    if (!acpi_rsdp || acpi_rsdp->rsdt_address == 0) return -1;

    uint32_t rsdt_phys = (uint32_t)acpi_rsdp->rsdt_address;
    map_table_page(rsdt_phys);
    acpi_rsdt = (acpi_table_header_t *)(uintptr_t)rsdt_phys;
    if (memcmp(acpi_rsdt->signature, "RSDT", 4) != 0) {
        acpi_rsdt = NULL;
        return -1;
    }
    if (acpi_checksum(acpi_rsdt, acpi_rsdt->length) != 0) {
        acpi_rsdt = NULL;
        return -1;
    }

    serial_print("ACPI: RSDT found\n");
    return 0;
}

/* ================================================================== */
/* Table lookup                                                         */
/* ================================================================== */

acpi_table_header_t *acpi_find_table(const char *signature) {
    /* Prefer XSDT (ACPI 2.0+) */
    if (acpi_xsdt && acpi_xsdt_tables > 0) {
        uint64_t *ptrs = (uint64_t *)((uint8_t *)acpi_xsdt + sizeof(acpi_table_header_t));
        for (int i = 0; i < acpi_xsdt_tables; i++) {
            uint64_t tbl_phys = ptrs[i];
            map_table_page(tbl_phys);
            acpi_table_header_t *tbl = (acpi_table_header_t *)(uintptr_t)tbl_phys;
            if (memcmp(tbl->signature, signature, 4) == 0) return tbl;
        }
    }

    /* Fallback to RSDT (ACPI 1.0) */
    if (acpi_rsdt) {
        uint32_t *ptrs = (uint32_t *)((uint8_t *)acpi_rsdt + sizeof(acpi_table_header_t));
        int count = (acpi_rsdt->length - sizeof(acpi_table_header_t)) / 4;
        for (int i = 0; i < count; i++) {
            uint32_t tbl_phys = ptrs[i];
            map_table_page(tbl_phys);
            acpi_table_header_t *tbl = (acpi_table_header_t *)(uintptr_t)tbl_phys;
            if (memcmp(tbl->signature, signature, 4) == 0) return tbl;
        }
    }

    return NULL;
}

/* ================================================================== */
/* DMAR table parsing                                                   */
/* ================================================================== */

static void parse_dmar(acpi_table_dmar_t *dmar) {
    if (!dmar) return;

    dmar_max_addr_width = dmar->host_address_width + 1;
    serial_print("ACPI: DMAR found\n");

    /* Walk DRHD structures after the DMAR header */
    uint8_t *ptr = (uint8_t *)dmar + sizeof(acpi_table_dmar_t);
    uint8_t *end = (uint8_t *)dmar + dmar->header.length;

    while (ptr + 2 <= end) {
        uint8_t type = ptr[0];
        uint8_t len  = ptr[1];
        if (len < 2 || ptr + len > end) break;

        if (type == DMAR_DRHD_TYPE && len >= sizeof(acpi_dmar_hardware_unit_t)) {
            acpi_dmar_hardware_unit_t *drhd = (acpi_dmar_hardware_unit_t *)ptr;

            if (iommu_unit_count < ACPI_MAX_IOMMU_UNITS) {
                acpi_iommu_unit_t *unit = &iommu_units[iommu_unit_count];
                unit->segment  = drhd->segment;
                unit->mmio_base = drhd->register_base;
                unit->flags    = drhd->flags;
                unit->bus      = 0xFF;
                unit->dev      = 0;
                unit->func     = 0;
                unit->enabled  = 1;

                /* Parse device scope entries for endpoint devices */
                uint8_t *scope = ptr + sizeof(acpi_dmar_hardware_unit_t);
                while (scope + 2 <= ptr + len) {
                    uint8_t stype = scope[0];
                    uint8_t slen  = scope[1];
                    if (slen < 2 || scope + slen > ptr + len) break;
                    if (stype == DMAR_SCOPE_ENDPOINT && slen >= 6) {
                        unit->bus  = scope[4];
                        unit->dev  = scope[5] >> 3;
                        unit->func = scope[5] & 7;
                    }
                    scope += slen;
                }

                serial_print("  DRHD: segment=");
                serial_putc('0' + unit->segment);
                if (unit->flags & DMAR_DRHD_INCLUDE_ALL)
                    serial_print(" (include-all)");
                serial_print("\n");

                iommu_unit_count++;
            }
        }
        ptr += len;
    }
}

/* ================================================================== */
/* Public API                                                           */
/* ================================================================== */

int acpi_init(uint64_t multiboot_info) {
    serial_print("ACPI: Initializing...\n");

    acpi_rsdp = find_rsdp(multiboot_info);
    if (!acpi_rsdp) {
        screen_log("INFO", COLOR_LIGHT_CYAN, "ACPI: RSDP not found, no IOMMU");
        serial_print("ACPI: RSDP not found\n");
        return -1;
    }

    serial_print("ACPI: RSDP valid\n");

    /* Parse XSDT (preferred) or RSDT */
    if (parse_xsdt() != 0) parse_rsdt();

    /* Find and parse DMAR table */
    acpi_table_dmar_t *dmar = (acpi_table_dmar_t *)acpi_find_table("DMAR");
    if (dmar) {
        parse_dmar(dmar);
        screen_log("OK", COLOR_LIGHT_GREEN, "ACPI: DMAR found, IOMMU units detected");
    } else {
        serial_print("ACPI: No DMAR table\n");
        screen_log("INFO", COLOR_LIGHT_CYAN, "ACPI: No DMAR (IOMMU not present)");
    }

    return 0;
}

int acpi_get_iommu_units(acpi_iommu_unit_t *units, int max_units) {
    int count = (max_units < iommu_unit_count) ? max_units : iommu_unit_count;
    for (int i = 0; i < count; i++) units[i] = iommu_units[i];
    return iommu_unit_count;
}

int acpi_dmar_get_max_addr_width(void) {
    return dmar_max_addr_width;
}
