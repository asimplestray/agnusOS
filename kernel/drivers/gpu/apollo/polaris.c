#include <apollo_drv.h>
#include <pci.h>
#include <vmm.h>
#include <screen.h>
#include <kheap.h>
#include <serial.h>

#define POLARIS_VENDOR_ID     0x1002
#define POLARIS_DEVICE_ID_480 0x67DF
#define POLARIS_DEVICE_ID_580 0x67EF
#define POLARIS_DEVICE_ID_570 0x67FF

#define POLARIS_BAR_MMIO      0
#define POLARIS_BAR_VRAM      1
#define POLARIS_BAR_IO        2

#define POLARIS_MMIO_SIZE     (16 * 1024 * 1024)

#define GRBM_STATUS           0x8010
#define GRBM_STATUS2          0x8014
#define GRBM_SOFT_RESET       0x8020
#define SRBM_STATUS           0x0E50
#define CONFIG_MEMSIZE        0x5428
#define BIF_FB_EN             0x0504
#define HDP_HOST_PATH_CNTL    0x2C00
#define HDP_FLUSH_INVALIDATE  0x2C10
#define SMC_MSG               0x0228
#define SMC_RESP              0x0230
#define SDMA0_STATUS          0x0D28
#define SDMA1_STATUS          0x1D28
#define DCE_CRTC_CONTROL      0x1B0AC

/* DCE CRTC timing registers (CRTC0 at 0x6500 base) */
#define DCE_CRTC0_H_TOTAL           0x06500
#define DCE_CRTC0_H_BLANK_START_END 0x06504
#define DCE_CRTC0_H_SYNC_A          0x06508
#define DCE_CRTC0_V_TOTAL           0x06510
#define DCE_CRTC0_V_BLANK_START_END 0x06514
#define DCE_CRTC0_V_SYNC_A          0x06518
#define DCE_CRTC0_OFFSET            0x06520
#define DCE_CRTC0_PITCH             0x06524

#define MM_INDEX              0x0000
#define MM_DATA               0x0004

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

static uint32_t polaris_mmio_read(struct polaris_dev *dev, uint32_t reg) {
    return *(volatile uint32_t *)(dev->mmio_virt + reg);
}

static void polaris_mmio_write(struct polaris_dev *dev, uint32_t reg, uint32_t val) {
    serial_print("ApolloOS: polaris_mmio_write reg=0x");
    char hex[] = "0123456789ABCDEF";
    char buf[9];
    buf[0] = hex[(reg >> 28) & 0xF];
    buf[1] = hex[(reg >> 24) & 0xF];
    buf[2] = hex[(reg >> 20) & 0xF];
    buf[3] = hex[(reg >> 16) & 0xF];
    buf[4] = hex[(reg >> 12) & 0xF];
    buf[5] = hex[(reg >> 8) & 0xF];
    buf[6] = hex[(reg >> 4) & 0xF];
    buf[7] = hex[reg & 0xF];
    buf[8] = '\0';
    serial_print(buf);
    serial_print(" val=0x");
    buf[0] = hex[(val >> 28) & 0xF];
    buf[1] = hex[(val >> 24) & 0xF];
    buf[2] = hex[(val >> 20) & 0xF];
    buf[3] = hex[(val >> 16) & 0xF];
    buf[4] = hex[(val >> 12) & 0xF];
    buf[5] = hex[(val >> 8) & 0xF];
    buf[6] = hex[(val >> 4) & 0xF];
    buf[7] = hex[val & 0xF];
    buf[8] = '\0';
    serial_print(buf);
    serial_print("\n");
    *(volatile uint32_t *)(dev->mmio_virt + reg) = val;
}

static void polaris_wait_idle(struct polaris_dev *dev) {
    for (int i = 0; i < 1000000; i++) {
        uint32_t status = polaris_mmio_read(dev, GRBM_STATUS);
        if ((status & 0xFFFFFFFF) == 0) break;
    }
}

static void polaris_soft_reset(struct polaris_dev *dev) {
    polaris_mmio_write(dev, GRBM_SOFT_RESET, 0xFFFFFFFF);
    for (volatile int i = 0; i < 1000; i++);
    polaris_mmio_write(dev, GRBM_SOFT_RESET, 0);
    polaris_wait_idle(dev);
}

static void polaris_enable_fb(struct polaris_dev *dev) {
    polaris_mmio_write(dev, BIF_FB_EN, 0x00000003);
    polaris_wait_idle(dev);
}

static int polaris_init_vram(struct polaris_dev *dev) {
    uint32_t vram_bar = pci_read_bar(dev->bus, dev->dev, dev->func, POLARIS_BAR_VRAM);
    dev->vram_phys = vram_bar & ~0xFULL;
    
    uint32_t orig_cmd = pci_read_dword(dev->bus, dev->dev, dev->func, PCI_CONFIG_COMMAND);
    pci_write_dword(dev->bus, dev->dev, dev->func, PCI_CONFIG_COMMAND, orig_cmd | PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);
    
    dev->vram_size = 0;
    uint32_t orig = pci_read_dword(dev->bus, dev->dev, dev->func, 0x14);
    pci_write_dword(dev->bus, dev->dev, dev->func, 0x14, 0xFFFFFFFF);
    dev->vram_size = (~(pci_read_dword(dev->bus, dev->dev, dev->func, 0x14) & ~0xF)) + 1;
    pci_write_dword(dev->bus, dev->dev, dev->func, 0x14, orig);
    
    dev->vram_virt = 0xFFFF800200000000ULL;
    for (uint32_t off = 0; off < dev->vram_size; off += 4096) {
        vmm_map_region(dev->vram_virt + off, dev->vram_phys + off, 4096, VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    }
    
    return 0;
}

int polaris_init(struct polaris_dev *dev, uint8_t bus, uint8_t device, uint8_t func) {
    dev->bus = bus;
    dev->dev = device;
    dev->func = func;
    
    uint16_t vendor = pci_read_word(bus, device, func, 0);
    uint16_t dev_id = pci_read_word(bus, device, func, 2);
    
    if (vendor != POLARIS_VENDOR_ID) return -1;
    
    screen_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    screen_print("Polaris: Found device ");
    char hex[] = "0123456789ABCDEF";
    char buf[5];
    buf[0] = hex[(dev_id >> 12) & 0xF];
    buf[1] = hex[(dev_id >> 8) & 0xF];
    buf[2] = hex[(dev_id >> 4) & 0xF];
    buf[3] = hex[dev_id & 0xF];
    buf[4] = '\0';
    screen_print(buf);
    screen_print("\n");
    
    uint32_t mmio_bar = pci_read_bar(bus, device, func, POLARIS_BAR_MMIO);
    uint32_t mmio_base = mmio_bar & ~0xF;
    dev->mmio_virt = 0xFFFF800100000000ULL;
    vmm_map_region(dev->mmio_virt, (uint64_t)mmio_base, POLARIS_MMIO_SIZE, VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    
    screen_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    screen_print("Polaris: MMIO mapped\n");
    
    polaris_init_vram(dev);
    screen_print("Polaris: VRAM mapped (");
    screen_print(buf);
    screen_print(" MB)\n");
    
    polaris_soft_reset(dev);
    polaris_enable_fb(dev);
    
    uint32_t vram_mb = polaris_mmio_read(dev, CONFIG_MEMSIZE);
    screen_print("Polaris: VRAM size from CONFIG_MEMSIZE: ");
    buf[0] = hex[(vram_mb >> 12) & 0xF];
    buf[1] = hex[(vram_mb >> 8) & 0xF];
    buf[2] = hex[(vram_mb >> 4) & 0xF];
    buf[3] = hex[vram_mb & 0xF];
    buf[4] = '\0';
    screen_print(buf);
    screen_print(" MB\n");
    
    dev->width = 1920;
    dev->height = 1080;
    dev->bpp = 32;
    dev->pitch = dev->width * (dev->bpp / 8);
    
    return 0;
}

int polaris_set_mode(struct polaris_dev *dev, uint32_t width, uint32_t height, uint32_t bpp) {
    serial_print("ApolloOS: polaris_set_mode called\n");
    dev->width = width;
    dev->height = height;
    dev->bpp = bpp;
    dev->pitch = width * (bpp / 8);
    
    polaris_mmio_write(dev, DCE_CRTC_CONTROL, 0);
    polaris_wait_idle(dev);
    
    /* Program DCE CRTC timing for 1920x1080@60 */
    /* VESA 1920x1080@60: H_TOTAL=2200, H_DISP=1920, H_SYNC_START=2008, H_SYNC_END=2052
       V_TOTAL=1125, V_DISP=1080, V_SYNC_START=1084, V_SYNC_END=1089 */
    polaris_mmio_write(dev, DCE_CRTC0_H_TOTAL, ((2200 - 1) << 16) | (1920 - 1));
    polaris_mmio_write(dev, DCE_CRTC0_H_BLANK_START_END, ((2200 - 1) << 16) | (1920 - 1));
    polaris_mmio_write(dev, DCE_CRTC0_H_SYNC_A, ((2052 - 1) << 16) | (2008 - 1));
    
    polaris_mmio_write(dev, DCE_CRTC0_V_TOTAL, ((1125 - 1) << 16) | (1080 - 1));
    polaris_mmio_write(dev, DCE_CRTC0_V_BLANK_START_END, ((1125 - 1) << 16) | (1080 - 1));
    polaris_mmio_write(dev, DCE_CRTC0_V_SYNC_A, ((1089 - 1) << 16) | (1084 - 1));
    
    polaris_mmio_write(dev, DCE_CRTC0_OFFSET, 0);
    polaris_mmio_write(dev, DCE_CRTC0_PITCH, dev->pitch);
    
    /* Enable CRTC */
    polaris_mmio_write(dev, DCE_CRTC_CONTROL, 0x00000001);
    polaris_wait_idle(dev);
    
    polaris_mmio_write(dev, HDP_HOST_PATH_CNTL, 0x00000001);
    polaris_mmio_write(dev, HDP_FLUSH_INVALIDATE, 0x00000001);
    polaris_wait_idle(dev);
    
    screen_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    screen_print("Polaris: Mode set ");
    char hex[] = "0123456789ABCDEF";
    char buf[5];
    buf[0] = hex[(width >> 12) & 0xF];
    buf[1] = hex[(width >> 8) & 0xF];
    buf[2] = hex[(width >> 4) & 0xF];
    buf[3] = hex[width & 0xF];
    buf[4] = '\0';
    screen_print(buf);
    screen_print("x");
    buf[0] = hex[(height >> 12) & 0xF];
    buf[1] = hex[(height >> 8) & 0xF];
    buf[2] = hex[(height >> 4) & 0xF];
    buf[3] = hex[height & 0xF];
    buf[4] = '\0';
    screen_print(buf);
    screen_print("x");
    buf[0] = hex[(bpp >> 4) & 0xF];
    buf[1] = hex[bpp & 0xF];
    buf[2] = '\0';
    screen_print(buf);
    screen_print("bpp\n");
    
    return 0;
}

void polaris_fill_rect(struct polaris_dev *dev, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (x + w > dev->width) w = dev->width - x;
    if (y + h > dev->height) h = dev->height - y;
    
    uint32_t *fb = (uint32_t *)dev->vram_virt;
    uint32_t pitch_pixels = dev->pitch / 4;
    
    for (uint32_t yy = y; yy < y + h; yy++) {
        uint32_t *line = fb + yy * pitch_pixels + x;
        for (uint32_t xx = 0; xx < w; xx++) {
            line[xx] = color;
        }
    }
}

void polaris_test_pattern(struct polaris_dev *dev) {
    serial_print("ApolloOS: polaris_test_pattern start\n");
    uint32_t *fb = (uint32_t *)dev->vram_virt;
    uint32_t pitch_pixels = dev->pitch / 4;
    
    // Full screen gradient
    for (uint32_t y = 0; y < dev->height; y++) {
        for (uint32_t x = 0; x < dev->width; x++) {
            uint32_t r = (x * 255) / dev->width;
            uint32_t g = (y * 255) / dev->height;
            uint32_t b = ((x + y) * 255) / (dev->width + dev->height);
            uint32_t color = (r << 16) | (g << 8) | b;
            fb[y * pitch_pixels + x] = color;
        }
    }
    serial_print("ApolloOS: polaris_test_pattern done\n");
}