#ifndef _POLARIS_H_
#define _POLARIS_H_

#include <stdint.h>

#define POLARIS_VENDOR_ID     0x1002
#define POLARIS_DEVICE_ID_480 0x67DF
#define POLARIS_DEVICE_ID_580 0x67EF
#define POLARIS_DEVICE_ID_570 0x67FF

struct polaris_dev {
    uint8_t bus, dev, func;
    uint64_t mmio_virt;
    uint64_t vram_virt;
    uint64_t vram_phys;
    uint32_t vram_size;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
};

int polaris_init(struct polaris_dev *dev, uint8_t bus, uint8_t device, uint8_t func);
int polaris_set_mode(struct polaris_dev *dev, uint32_t width, uint32_t height, uint32_t bpp);
void polaris_fill_rect(struct polaris_dev *dev, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void polaris_test_pattern(struct polaris_dev *dev);

#endif