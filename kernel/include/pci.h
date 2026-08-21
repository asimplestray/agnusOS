#ifndef PCI_H
#define PCI_H

#include <stdint.h>
#include <stddef.h>

/* Standard PCI config space offsets */
#define PCI_CONFIG_VENDOR_ID      0x00
#define PCI_CONFIG_DEVICE_ID      0x02
#define PCI_CONFIG_COMMAND        0x04
#define PCI_CONFIG_STATUS         0x06
#define PCI_CONFIG_CLASS          0x08
#define PCI_CONFIG_CACHE_LINE     0x0C
#define PCI_CONFIG_LATENCY        0x0D
#define PCI_CONFIG_HEADER_TYPE    0x0E
#define PCI_CONFIG_BAR0           0x10
#define PCI_CONFIG_BAR1           0x14
#define PCI_CONFIG_BAR2           0x18
#define PCI_CONFIG_BAR3           0x1C
#define PCI_CONFIG_BAR4           0x20
#define PCI_CONFIG_BAR5           0x24
#define PCI_CONFIG_CAP_PTR        0x34
#define PCI_CONFIG_INTERRUPT_LINE 0x3C
#define PCI_CONFIG_INTERRUPT_PIN  0x3D

/* Capability IDs */
#define PCI_CAP_ID_MSI            0x05
#define PCI_CAP_ID_MSIX           0x11

/* MSI Capability structure offsets */
#define PCI_MSI_CAP_CONTROL       0x02
#define PCI_MSI_CAP_ADDRESS_LO    0x04
#define PCI_MSI_CAP_ADDRESS_HI    0x08
#define PCI_MSI_CAP_DATA_32       0x08
#define PCI_MSI_CAP_DATA_64       0x0C
#define PCI_MSI_CAP_MASK          0x0C
#define PCI_MSI_CAP_PENDING       0x10

#define PCI_MSI_CTRL_64BIT        (1 << 7)
#define PCI_MSI_CTRL_MULTIMSG_EN  (0x7 << 4)
#define PCI_MSI_CTRL_MULTIMSG_CAP (0x7 << 1)
#define PCI_MSI_CTRL_ENABLE       (1 << 0)

/* MSIX Capability structure offsets */
#define PCI_MSIX_CAP_CONTROL      0x02
#define PCI_MSIX_CAP_TABLE        0x04
#define PCI_MSIX_CAP_PBA          0x08

#define PCI_MSIX_CTRL_ENABLE      (1 << 15)
#define PCI_MSIX_CTRL_MASKALL     (1 << 14)
#define PCI_MSIX_CTRL_TABLE_SIZE  0x7FF

/* Command register bits */
#define PCI_CMD_IO_SPACE          (1 << 0)
#define PCI_CMD_MEM_SPACE         (1 << 1)
#define PCI_CMD_BUS_MASTER        (1 << 2)
#define PCI_CMD_INTX_DISABLE      (1 << 10)

uint16_t pci_read_word(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint32_t pci_read_dword(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint32_t pci_read_bar(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar);
void pci_write_word(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint16_t val);
void pci_write_dword(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val);
void pci_map_bar(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar, uint64_t virt);
void pci_enum(void);

/* MSI/MSI-X support */
int pci_enable_msi(uint8_t bus, uint8_t dev, uint8_t func, uint32_t addr, uint16_t data);
int pci_enable_msix(uint8_t bus, uint8_t dev, uint8_t func, uint32_t *vectors, int num_vectors);
void pci_disable_msi(uint8_t bus, uint8_t dev, uint8_t func);
void pci_disable_msix(uint8_t bus, uint8_t dev, uint8_t func);
int pci_find_capability(uint8_t bus, uint8_t dev, uint8_t func, uint8_t cap_id);

/* BAR utilities */
uint64_t pci_get_bar_size(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar);
int pci_map_bar_wc(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar, uint64_t virt, bool write_combine);
void pci_unmap_bar(uint64_t virt, uint64_t size);

/* Device enable/disable */
void pci_enable_device(uint8_t bus, uint8_t dev, uint8_t func);
void pci_disable_device(uint8_t bus, uint8_t dev, uint8_t func);

/* MSI-X vector masking */
void pci_msix_mask_vector(uint8_t bus, uint8_t dev, uint8_t func, int vector);
void pci_msix_unmask_vector(uint8_t bus, uint8_t dev, uint8_t func, int vector);

/* IOMMU stub for DMA */
int pci_iommu_map(uint8_t bus, uint8_t dev, uint8_t func, uint64_t iova, uint64_t paddr, size_t size);
void pci_iommu_unmap(uint8_t bus, uint8_t dev, uint8_t func, uint64_t iova, size_t size);

#endif
