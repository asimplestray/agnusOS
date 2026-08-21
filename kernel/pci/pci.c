#include <pci.h>
#include <io.h>
#include <screen.h>
#include <vmm.h>
#include <kheap.h>
#include <dma.h>

static uint32_t pci_config_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    return (uint32_t)((bus << 16) | (dev << 11) | (func << 8) | (offset & 0xFC) | 0x80000000);
}

static void pci_write_config_dword(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val) {
    outl(0xCF8, pci_config_addr(bus, dev, func, offset));
    outl(0xCFC, val);
}

static void pci_write_config_word(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint16_t val) {
    uint32_t cur = pci_read_dword(bus, dev, func, offset & ~3);
    if (offset & 2) {
        cur = (cur & 0xFFFF) | ((uint32_t)val << 16);
    } else {
        cur = (cur & 0xFFFF0000) | val;
    }
    pci_write_config_dword(bus, dev, func, offset & ~3, cur);
}

uint32_t pci_read_dword(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    outl(0xCF8, pci_config_addr(bus, dev, func, offset));
    return inl(0xCFC);
}

uint16_t pci_read_word(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t val = pci_read_dword(bus, dev, func, offset & ~3);
    return (uint16_t)((val >> ((offset & 2) * 8)) & 0xFFFF);
}

void pci_write_dword(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val) {
    pci_write_config_dword(bus, dev, func, offset, val);
}

void pci_write_word(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint16_t val) {
    pci_write_config_word(bus, dev, func, offset, val);
}

/* Find PCI capability pointer */
int pci_find_capability(uint8_t bus, uint8_t dev, uint8_t func, uint8_t cap_id) {
    uint8_t cap_ptr = pci_read_word(bus, dev, func, PCI_CONFIG_CAP_PTR) & 0xFF;
    
    while (cap_ptr >= 0x40 && cap_ptr < 0xFF) {
        uint8_t id = pci_read_word(bus, dev, func, cap_ptr) & 0xFF;
        if (id == cap_id) {
            return cap_ptr;
        }
        cap_ptr = (pci_read_word(bus, dev, func, cap_ptr + 1) >> 8) & 0xFF;
    }
    return -1;
}

/* Enable MSI */
int pci_enable_msi(uint8_t bus, uint8_t dev, uint8_t func, uint32_t addr, uint16_t data) {
    int cap = pci_find_capability(bus, dev, func, PCI_CAP_ID_MSI);
    if (cap < 0) return -1;
    
    uint16_t ctrl = pci_read_word(bus, dev, func, cap + PCI_MSI_CAP_CONTROL);
    
    /* Check 64-bit support */
    bool is_64bit = (ctrl & PCI_MSI_CTRL_64BIT);
    
    /* Set message address */
    pci_write_config_dword(bus, dev, func, cap + PCI_MSI_CAP_ADDRESS_LO, addr);
    if (is_64bit) {
        pci_write_config_dword(bus, dev, func, cap + PCI_MSI_CAP_ADDRESS_HI, 0);
    }
    
    /* Set message data */
    if (is_64bit) {
        pci_write_config_dword(bus, dev, func, cap + PCI_MSI_CAP_DATA_64, data);
    } else {
        pci_write_config_dword(bus, dev, func, cap + PCI_MSI_CAP_DATA_32, data);
    }
    
    /* Enable MSI */
    ctrl |= PCI_MSI_CTRL_ENABLE;
    pci_write_word(bus, dev, func, cap + PCI_MSI_CAP_CONTROL, ctrl);
    
    /* Disable legacy INTx */
    uint16_t cmd = pci_read_word(bus, dev, func, PCI_CONFIG_COMMAND);
    cmd |= PCI_CMD_INTX_DISABLE;
    pci_write_word(bus, dev, func, PCI_CONFIG_COMMAND, cmd);
    
    return 0;
}

/* Disable MSI */
void pci_disable_msi(uint8_t bus, uint8_t dev, uint8_t func) {
    int cap = pci_find_capability(bus, dev, func, PCI_CAP_ID_MSI);
    if (cap < 0) return;
    
    uint16_t ctrl = pci_read_word(bus, dev, func, cap + PCI_MSI_CAP_CONTROL);
    ctrl &= ~PCI_MSI_CTRL_ENABLE;
    pci_write_word(bus, dev, func, cap + PCI_MSI_CAP_CONTROL, ctrl);
    
    /* Re-enable INTx */
    uint16_t cmd = pci_read_word(bus, dev, func, PCI_CONFIG_COMMAND);
    cmd &= ~PCI_CMD_INTX_DISABLE;
    pci_write_word(bus, dev, func, PCI_CONFIG_COMMAND, cmd);
}

/* MSI-X vector masking */
void pci_msix_mask_vector(uint8_t bus, uint8_t dev, uint8_t func, int vector) {
    int cap = pci_find_capability(bus, dev, func, PCI_CAP_ID_MSIX);
    if (cap < 0) return;
    
    uint32_t table_info = pci_read_dword(bus, dev, func, cap + PCI_MSIX_CAP_TABLE);
    uint8_t bar_idx = table_info & 0x7;
    uint32_t table_offset = table_info & ~0x7;
    
    uint32_t table_bar = pci_read_bar(bus, dev, func, bar_idx);
    uint32_t table_base = table_bar & ~0xF;
    
    vmm_map_region(0xFFFF800200000000ULL, (uint64_t)table_base + table_offset, 4096, VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    volatile uint32_t *table = (volatile uint32_t *)0xFFFF800200000000ULL;
    
    /* Set mask bit in vector control */
    table[vector * 4 + 3] |= 1;
    
    vmm_unmap_page(0xFFFF800200000000ULL);
}

void pci_msix_unmask_vector(uint8_t bus, uint8_t dev, uint8_t func, int vector) {
    int cap = pci_find_capability(bus, dev, func, PCI_CAP_ID_MSIX);
    if (cap < 0) return;
    
    uint32_t table_info = pci_read_dword(bus, dev, func, cap + PCI_MSIX_CAP_TABLE);
    uint8_t bar_idx = table_info & 0x7;
    uint32_t table_offset = table_info & ~0x7;
    
    uint32_t table_bar = pci_read_bar(bus, dev, func, bar_idx);
    uint32_t table_base = table_bar & ~0xF;
    
    vmm_map_region(0xFFFF800200000000ULL, (uint64_t)table_base + table_offset, 4096, VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    volatile uint32_t *table = (volatile uint32_t *)0xFFFF800200000000ULL;
    
    /* Clear mask bit in vector control */
    table[vector * 4 + 3] &= ~1;
    
    vmm_unmap_page(0xFFFF800200000000ULL);
}

/* Enable MSI-X */
int pci_enable_msix(uint8_t bus, uint8_t dev, uint8_t func, uint32_t *vectors, int num_vectors) {
    int cap = pci_find_capability(bus, dev, func, PCI_CAP_ID_MSIX);
    if (cap < 0) return -1;
    
    uint16_t ctrl = pci_read_word(bus, dev, func, cap + PCI_MSIX_CAP_CONTROL);
    int table_size = (ctrl & PCI_MSIX_CTRL_TABLE_SIZE) + 1;
    
    if (num_vectors > table_size) return -1;
    
    /* Read table location */
    uint32_t table_info = pci_read_dword(bus, dev, func, cap + PCI_MSIX_CAP_TABLE);
    uint8_t bar_idx = table_info & 0x7;
    uint32_t table_offset = table_info & ~0x7;
    
    /* Read PBA location */
    uint32_t pba_info = pci_read_dword(bus, dev, func, cap + PCI_MSIX_CAP_PBA);
    (void)pba_info; /* PBA handling reserved for future use */
    
    /* Map table BAR if needed */
    uint32_t table_bar = pci_read_bar(bus, dev, func, bar_idx);
    uint32_t table_base = table_bar & ~0xF;
    vmm_map_region(0xFFFF800200000000ULL, (uint64_t)table_base + table_offset, 
                   table_size * 16, VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    
    volatile uint32_t *table = (volatile uint32_t *)0xFFFF800200000000ULL;
    
    /* Configure each vector */
    for (int i = 0; i < num_vectors; i++) {
        table[i * 4 + 0] = vectors[i];           /* Message address low */
        table[i * 4 + 1] = 0;                    /* Message address high */
        table[i * 4 + 2] = 0;                    /* Message data (will be set by APIC) */
        table[i * 4 + 3] = 0;                    /* Vector control (unmasked) */
    }
    
    /* Enable MSI-X */
    ctrl |= PCI_MSIX_CTRL_ENABLE;
    ctrl &= ~PCI_MSIX_CTRL_MASKALL;
    pci_write_word(bus, dev, func, cap + PCI_MSIX_CAP_CONTROL, ctrl);
    
    /* Disable legacy INTx */
    uint16_t cmd = pci_read_word(bus, dev, func, PCI_CONFIG_COMMAND);
    cmd |= PCI_CMD_INTX_DISABLE;
    pci_write_word(bus, dev, func, PCI_CONFIG_COMMAND, cmd);
    
    return 0;
}

/* Disable MSI-X */
void pci_disable_msix(uint8_t bus, uint8_t dev, uint8_t func) {
    int cap = pci_find_capability(bus, dev, func, PCI_CAP_ID_MSIX);
    if (cap < 0) return;
    
    uint16_t ctrl = pci_read_word(bus, dev, func, cap + PCI_MSIX_CAP_CONTROL);
    ctrl &= ~PCI_MSIX_CTRL_ENABLE;
    pci_write_word(bus, dev, func, cap + PCI_MSIX_CAP_CONTROL, ctrl);
    
    /* Re-enable INTx */
    uint16_t cmd = pci_read_word(bus, dev, func, PCI_CONFIG_COMMAND);
    cmd &= ~PCI_CMD_INTX_DISABLE;
    pci_write_word(bus, dev, func, PCI_CONFIG_COMMAND, cmd);
}

/* Read BAR value */
uint32_t pci_read_bar(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar) {
    return pci_read_dword(bus, dev, func, 0x10 + bar * 4);
}

/* Get BAR size by writing all 1s and reading back */
uint64_t pci_get_bar_size(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar) {
    uint32_t orig = pci_read_dword(bus, dev, func, 0x10 + bar * 4);
    pci_write_dword(bus, dev, func, 0x10 + bar * 4, 0xFFFFFFFF);
    uint32_t size = pci_read_dword(bus, dev, func, 0x10 + bar * 4);
    pci_write_dword(bus, dev, func, 0x10 + bar * 4, orig);
    
    if (size == 0) return 0;
    return (~(size & ~0xF)) + 1;
}

/* Map PCI BAR with write-combine support for framebuffer/MMIO */
int pci_map_bar_wc(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar, uint64_t virt, bool write_combine) {
    uint32_t bar_val = pci_read_bar(bus, dev, func, bar);
    int is_io = bar_val & 1;
    if (is_io) {
        return -1; /* I/O space BARs not supported */
    }
    
    uint64_t base = bar_val & ~0xFULL;
    uint64_t size = pci_get_bar_size(bus, dev, func, bar);
    if (size == 0) {
        size = 4096; /* Default to 4KB if size detection fails */
    }
    
    /* Align size to page boundary */
    size = (size + 4095) & ~4095;
    
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITE;
    if (write_combine) {
        flags |= VMM_FLAG_WC;
    }
    
    vmm_map_region(virt, base, (uint32_t)size, flags);
    return 0;
}

/* Legacy BAR mapping (no write-combine) */
void pci_map_bar(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar, uint64_t virt) {
    pci_map_bar_wc(bus, dev, func, bar, virt, false);
}

/* Unmap PCI BAR */
void pci_unmap_bar(uint64_t virt, uint64_t size) {
    for (uint64_t offset = 0; offset < size; offset += 4096) {
        vmm_unmap_page(virt + offset);
    }
}

/* Enable bus mastering and memory space */
void pci_enable_device(uint8_t bus, uint8_t dev, uint8_t func) {
    uint16_t cmd = pci_read_word(bus, dev, func, PCI_CONFIG_COMMAND);
    cmd |= PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER;
    pci_write_word(bus, dev, func, PCI_CONFIG_COMMAND, cmd);
}

/* Disable device */
void pci_disable_device(uint8_t bus, uint8_t dev, uint8_t func) {
    uint16_t cmd = pci_read_word(bus, dev, func, PCI_CONFIG_COMMAND);
    cmd &= ~(PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER | PCI_CMD_IO_SPACE);
    pci_write_word(bus, dev, func, PCI_CONFIG_COMMAND, cmd);
}

/* IOMMU stub: identity mapping for DMA */
int pci_iommu_map(uint8_t bus, uint8_t dev, uint8_t func, uint64_t iova, uint64_t paddr, size_t size) {
    (void)bus; (void)dev; (void)func;
    return iommu_map(NULL, iova, paddr, size, 0);
}

void pci_iommu_unmap(uint8_t bus, uint8_t dev, uint8_t func, uint64_t iova, size_t size) {
    (void)bus; (void)dev; (void)func;
    iommu_unmap(NULL, iova, size);
}

void pci_enum(void) {
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint16_t vendor = pci_read_word((uint8_t)bus, (uint8_t)dev, 0, 0);
            if (vendor == 0xFFFF) continue;

            uint16_t device = pci_read_word((uint8_t)bus, (uint8_t)dev, 0, 2);
            uint8_t class_code = (uint8_t)(pci_read_word((uint8_t)bus, (uint8_t)dev, 0, 10) >> 8);
            uint8_t subclass = (uint8_t)(pci_read_word((uint8_t)bus, (uint8_t)dev, 0, 10) & 0xFF);
            (void)device; (void)class_code; (void)subclass;

            screen_set_color(COLOR_LIGHT_GREY, COLOR_BLACK);
            screen_print("PCI ");
            screen_set_color(COLOR_LIGHT_BLUE, COLOR_BLACK);
            screen_print("V:");
            char hex[] = "0123456789ABCDEF";
            char buf[5];
            buf[0] = hex[(vendor >> 12) & 0xF];
            buf[1] = hex[(vendor >> 8) & 0xF];
            buf[2] = hex[(vendor >> 4) & 0xF];
            buf[3] = hex[vendor & 0xF];
            buf[4] = '\0';
            screen_print(buf);
            screen_set_color(COLOR_WHITE, COLOR_BLACK);
            screen_print("\n");
        }
    }
}