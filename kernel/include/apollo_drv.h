#ifndef _APOLLO_DRV_H_
#define _APOLLO_DRV_H_

#include <stdint.h>

struct drm_device;

// Chip families
#define APOLLO_CHIP_FAMILY_POLARIS    0x10
#define APOLLO_CHIP_FAMILY_KEPLER     0x20
#define APOLLO_CHIP_FAMILY_XE         0x30

// Chip generations (Polaris family)
#define APOLLO_CHIP_GEN_POLARIS30     0x30  // RX 590 GME
#define APOLLO_CHIP_GEN_POLARIS20     0x20  // RX 480/580
#define APOLLO_CHIP_GEN_POLARIS10     0x10  // RX 470/570
#define APOLLO_CHIP_GEN_POLARIS11     0x11  // RX 460/560

// Chip generations (Kepler family)
#define APOLLO_CHIP_GEN_KEPLER        0x00  // GK208/GK107/etc
#define APOLLO_CHIP_GEN_KEPLER2       0x01  // GK104/GK110/etc

// Chip generations (Xe family)
#define APOLLO_CHIP_GEN_XE_LP         0x00  // Intel Xe-LP
#define APOLLO_CHIP_GEN_XE_HP         0x01  // Intel Xe-HP
#define APOLLO_CHIP_GEN_XE_HPG        0x02  // Intel Xe-HPG

// Register offsets (would be chip-specific in a full implementation)
#define APOLLO_REG_GPU_STATUS         0x0000
#define APOLLO_REG_GPU_CONTROL        0x0004
#define APOLLO_REG_RING_WPTR          0x0300
#define APOLLO_REG_RING_RPTR          0x0304
#define APOLLO_REG_FENCE_STATUS       0x0400
#define APOLLO_REG_IRQ_STATUS         0x0500
#define APOLLO_REG_IRQ_MASK           0x504

// GPU status bits
#define APOLLO_STATUS_GPU_IDLE        (1 << 0)
#define APOLLO_STATUS_RING_FULL       (1 << 1)
#define APOLLO_STATUS_IRQ_PENDING     (1 << 2)

// GPU control bits
#define APOLLO_CTRL_ENABLE           (1 << 0)
#define APOLLO_CTRL_RESET            (1 << 1)
#define APOLLO_CTRL_IRQ_ENABLE       (1 << 2)

// External functions for PCI subsystem to call
int apollo_gpu_pci_probe(uint8_t bus, uint8_t dev, uint8_t func);
void apollo_gpu_pci_remove(uint8_t bus, uint8_t dev, uint8_t func);

#endif /* _APOLLO_DRV_H_ */