#ifndef _IRQRETURN_T_DEFINED
#define _IRQRETURN_T_DEFINED
typedef enum {
    IRQ_NONE = 0,
    IRQ_HANDLED = 1,
} irqreturn_t;
#endif

#if !defined(PCI_ANY_ID)
#define PCI_ANY_ID    (~0)
#endif
#include <stddef.h>
#define __init
#define __exit

#include <drm/drm_device.h>
#include <drm/drm_driver.h>
#include "../../../include/pci.h"
#include "../../../include/workqueue.h"
#include "../../../include/firmware.h"
#include "../../../include/pmm.h"
#include "../../../include/apollo_drv.h"
#include "polaris.h"
#include "../../../include/serial.h"

// Apollo GPU driver private structure
#include "../../../include/screen.h"
#include <string.h>
#include <kheap.h>

#define ENOMEM 12
#define EINVAL 22
#define ENODEV 19
#define ENOSPC 28
#define ETIME 62

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

/* Simple inline hex printer to serial (avoids snprintf dependency) */
static void drm_hex32(uint32_t val, char *out) {
    const char *hex = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) {
        out[7 - i] = hex[(val >> (i * 4)) & 0xF];
    }
    out[8] = '\0';
}
static void drm_hex64(uint64_t val, char *out) {
    const char *hex = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) {
        out[15 - i] = hex[(val >> (i * 4)) & 0xF];
    }
    out[16] = '\0';
}

#define module_init(x)
#define module_exit(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_AUTHOR(x)
#define MODULE_LICENSE(x)
struct apollo_gpu_priv {
    struct drm_device *base;
    
    // Chip-specific info
    uint32_t chip_family;
    uint32_t chip_generation;
    uint32_t chip_revision;
    bool is_polaris30;  // Flag for Polaris 30 specific handling
    
    // Memory domains
    struct {
        uint64_t vram_base;
        size_t vram_size;
        uint64_t gart_base;
        size_t gart_size;
    } mem;
    
    // Buffer object manager
    struct {
        void *bo_list;          // List of buffer objects
        spinlock_irq_t lock;
    } bo;
    
    // Command submission
    struct {
        void *ring_buffer;      // GPUCMD ring buffer
        size_t ring_size;
        uint32_t *wptr;         // Write pointer (CPU side)
        volatile uint32_t *rptr; // Read pointer (GPU side)
        uint32_t ib_size;       // Indirect buffer size
        struct work_struct ib_work; // Work for IB submission
        struct workqueue_struct *wq;
    } ring;
    
    // Fence management
    struct {
        uint32_t seqno;         // Current fence sequence
        uint32_t signaled_seqno; // Last signaled fence
        uint32_t waited_seqno;  // Last waited fence
        spinlock_irq_t lock;
        struct workqueue_struct *wq;
        struct work_struct timeout_work;
        uint64_t addr;          // VRAM address for fence status
        size_t addr_size;       // Size of fence status area
    } fence;
    
    // Interrupts
    struct {
        bool enabled;
        void (*handler)(struct apollo_gpu_priv *priv);
    } irq;
    
    // Display/KMS
    struct {
        struct fb_info *fbdev;  // Framebuffer device
        struct {
            uint32_t crtc_id;
            uint32_t encoder_id;
            uint32_t connector_id;
        } mode;
    } kms;
    
    // Power management
    struct {
        bool initialized;
        uint32_t current_power_state;
    } pm;
};

// Chip families
#define APOLLO_CHIP_FAMILY_POLARIS    0x10
#define APOLLO_CHIP_FAMILY_KEPLER     0x20
#define APOLLO_CHIP_FAMILY_XE         0x30

// Chip generations (within family)
#define APOLLO_CHIP_GEN_POLARIS30     0x30  // RX 590 GME
#define APOLLO_CHIP_GEN_KEPLER        0x00  // GK208/GK107/etc
#define APOLLO_CHIP_GEN_XE_LP         0x00  // Intel Xe-LP

// Register offsets (simplified - would be chip-specific in reality)
#define APOLLO_REG_GPU_STATUS         0x0000
#define APOLLO_REG_GPU_CONTROL        0x0004
#define APOLLO_REG_RING_WPTR          0x0300
#define APOLLO_REG_RING_RPTR          0x0304
#define APOLLO_REG_FENCE_STATUS       0x0400
#define APOLLO_REG_IRQ_STATUS         0x0500
#define APOLLO_REG_IRQ_MASK           0x0504

// GPU status bits
#define APOLLO_STATUS_GPU_IDLE        (1 << 0)
#define APOLLO_STATUS_RING_FULL       (1 << 1)
#define APOLLO_STATUS_IRQ_PENDING     (1 << 2)

// GPU control bits
#define APOLLO_CTRL_ENABLE           (1 << 0)
#define APOLLO_CTRL_RESET            (1 << 1)
#define APOLLO_CTRL_IRQ_ENABLE       (1 << 2)

// Forward declarations
static int apollo_gpu_init_memory(struct apollo_gpu_priv *priv);
static int apollo_gpu_init_ring(struct apollo_gpu_priv *priv);
static int apollo_gpu_init_fence(struct apollo_gpu_priv *priv);
static int apollo_gpu_init_irq(struct apollo_gpu_priv *priv);
static void apollo_gpu_fini_irq(struct apollo_gpu_priv *priv);
static void apollo_gpu_fini_fence(struct apollo_gpu_priv *priv);
static void apollo_gpu_fini_ring(struct apollo_gpu_priv *priv);
static void apollo_gpu_fini_memory(struct apollo_gpu_priv *priv);
static void apollo_gpu_ring_submit_work(struct work_struct *work);
static void apollo_gpu_fence_timeout_work(struct work_struct *work);
static irqreturn_t apollo_gpu_irq_handler(int irq, void *dev_id);

// Driver callbacks
static int apollo_driver_load(struct drm_device *dev);
static int apollo_driver_unload(struct drm_device *dev);
static int apollo_driver_open(struct drm_device *dev, void *file_private);
static int apollo_driver_release(struct drm_device *dev, void *file_private);
static int apollo_driver_gem_create_object(struct drm_device *dev, size_t size, void **obj);
static void apollo_driver_gem_free_object(struct drm_device *dev, void *obj);
static int apollo_driver_submit_command(struct drm_device *dev, void *cmd, size_t cmd_size);
static int apollo_driver_wait_for_fence(struct drm_device *dev, uint32_t seqno, uint64_t timeout_ns);
static void apollo_driver_signal_fence(struct drm_device *dev, uint32_t seqno);
static int apollo_driver_mode_set(struct drm_device *dev, uint32_t width, uint32_t height, uint32_t refresh_rate);
static irqreturn_t apollo_driver_irq_handler(struct drm_device *dev);
static const char *apollo_driver_get_name(struct drm_device *dev);
static const char *apollo_driver_get_desc(struct drm_device *dev);

// Driver version
static struct drm_driver_version apollo_driver_version = {
    .major = 0,
    .minor = 1,
    .patch = 0,
    .name = "apollogpu",
    .date = "2026-08-14",
    .desc = "ApolloOS Universal GPU Driver"
};

// PCI device ID table
static struct drm_pci_id apollo_pci_id_table[] = {
    // AMD Polaris 30 (RX 590 GME)
    {0x1002, 0x67df, PCI_ANY_ID, PCI_ANY_ID, APOLLO_CHIP_FAMILY_POLARIS | APOLLO_CHIP_GEN_POLARIS30},
    
    // Other Polaris variants (for testing)
    {0x1002, 0x67ef, PCI_ANY_ID, PCI_ANY_ID, APOLLO_CHIP_FAMILY_POLARIS | 0x31}, // RX 580
    {0x1002, 0x67ff, PCI_ANY_ID, PCI_ANY_ID, APOLLO_CHIP_FAMILY_POLARIS | 0x32}, // RX 570
    
    // NVIDIA Kepler (GK208 etc) - for testing/extensibility
    {0x10de, 0x1140, PCI_ANY_ID, PCI_ANY_ID, APOLLO_CHIP_FAMILY_KEPLER | APOLLO_CHIP_GEN_KEPLER}, // GK208
    {0x10de, 0x0fc0, PCI_ANY_ID, PCI_ANY_ID, APOLLO_CHIP_FAMILY_KEPLER | 0x01}, // GK107
    
    // Intel Xe-LP (for testing/extensibility)
    {0x8086, 0x4565, PCI_ANY_ID, PCI_ANY_ID, APOLLO_CHIP_FAMILY_XE | APOLLO_CHIP_GEN_XE_LP}, // Iris Plus G4
    
    // Terminator
    {0, 0, 0, 0, 0}
};

// Driver structure
static struct drm_driver apollo_driver = {
    .version = &apollo_driver_version,
    .load = apollo_driver_load,
    .unload = apollo_driver_unload,
    .open = apollo_driver_open,
    .release = apollo_driver_release,
    .gem_create_object = apollo_driver_gem_create_object,
    .gem_free_object = apollo_driver_gem_free_object,
    .submit_command = apollo_driver_submit_command,
    .wait_for_fence = apollo_driver_wait_for_fence,
    .signal_fence = apollo_driver_signal_fence,
    .mode_set = apollo_driver_mode_set,
    .irq_handler = apollo_driver_irq_handler,
    .get_name = apollo_driver_get_name,
    .get_desc = apollo_driver_get_desc,
};

// Module entry points
static int __init apollo_driver_init(void)
{
    screen_log("INFO", COLOR_LIGHT_GREEN, "ApolloOS GPU Driver: Loading");
    return drm_register_driver(&apollo_driver, apollo_pci_id_table);
}

static void __exit apollo_driver_exit(void)
{
    screen_log("INFO", COLOR_LIGHT_GREEN, "ApolloOS GPU Driver: Unloading");
    drm_unregister_driver(&apollo_driver);
}

// Driver implementation

static const char *apollo_driver_get_name(struct drm_device *dev)
{
    return apollo_driver_version.name;
}

static const char *apollo_driver_get_desc(struct drm_device *dev)
{
    return apollo_driver_version.desc;
}

static int apollo_driver_open(struct drm_device *dev, void *file_private)
{
    screen_log("DEBUG", COLOR_CYAN, "ApolloOS GPU: Device opened");
    return 0;
}

static int apollo_driver_release(struct drm_device *dev, void *file_private)
{
    screen_log("DEBUG", COLOR_CYAN, "ApolloOS GPU: Device released");
}

static int apollo_driver_gem_create_object(struct drm_device *dev, size_t size, void **obj)
{
    struct apollo_gpu_priv *priv = dev->dev_private;
    void *mem;
    
    // Align to page boundary
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    
    // Try to allocate from VRAM first, fall back to system memory
    void *mem_raw = kmalloc(size + PAGE_SIZE - 1);
    if (!mem_raw) {
        screen_log("ERROR", COLOR_RED, "ApolloOS GPU: Failed to allocate GEM object");
        return -ENOMEM;
    }
    mem = (void*)(((uintptr_t)mem_raw + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    
    memset(mem, 0, size);
    *obj = mem;
    
    screen_log("DEBUG", COLOR_CYAN, "ApolloOS GPU: Created GEM object");
    return 0;
}

static void apollo_driver_gem_free_object(struct drm_device *dev, void *obj)
{
    screen_log("DEBUG", COLOR_CYAN, "ApolloOS GPU: Freeing GEM object");
    kfree(obj);
}

static int apollo_driver_load(struct drm_device *dev)
{
    serial_print("ApolloOS: apollo_driver_load ENTRY\n");
    struct apollo_gpu_priv *priv;
    int ret;
    
    screen_log("INFO", COLOR_LIGHT_GREEN, "ApolloOS GPU: Loading driver for device");
    
    // Get PCI location from drm_device
    uint8_t bus = dev->pci_bus;
    uint8_t dev_id = dev->pci_dev;
    uint8_t func = dev->pci_func;
    
    // Allocate private structure
    priv = kmalloc(sizeof(struct apollo_gpu_priv));
    memset(priv, 0, sizeof(struct apollo_gpu_priv));
    if (!priv) {
        screen_log("ERROR", COLOR_RED, "ApolloOS GPU: Failed to allocate private structure");
        return -ENOMEM;
    }
    
    priv->base = dev;
    dev->dev_private = priv;
    
    // Initialize spinlocks
    spinlock_init(&priv->bo.lock.lock);
    spinlock_init(&priv->fence.lock.lock);
    
    // Detect chip type from PCI device ID
    uint16_t vendor = pci_read_word(bus, dev_id, func, PCI_CONFIG_VENDOR_ID); // Would need actual bus/dev/func
    uint16_t device = pci_read_word(bus, dev_id, func, PCI_CONFIG_DEVICE_ID);
    
    // In reality, we'd get bus/dev/func from the PCI device structure
    // For now, we'll extract from driver_data
    priv->chip_family = (unsigned long)dev->dev_private & 0xFF00;
    priv->chip_generation = (unsigned long)dev->dev_private & 0x00FF;
    
    if (vendor == 0x1002 && device == 0x67df) { // RX 590 GME (Polaris 30)
        priv->chip_family = APOLLO_CHIP_FAMILY_POLARIS;
        priv->chip_generation = APOLLO_CHIP_GEN_POLARIS30;
        priv->chip_revision = 0x0;
        priv->is_polaris30 = true;
        screen_log("INFO", COLOR_LIGHT_GREEN, "ApolloOS GPU: Detected RX 590 GME (Polaris 30/GFX8)");
    } else if (vendor == 0x1002 && (device == 0x67ef || device == 0x67df || device == 0x67ff)) { // RX 480/580/570 (Polaris 10/20)
        priv->chip_family = APOLLO_CHIP_FAMILY_POLARIS;
        priv->chip_generation = APOLLO_CHIP_GEN_POLARIS20;
        priv->chip_revision = 0x0;
        priv->is_polaris30 = false;
        screen_log("INFO", COLOR_LIGHT_GREEN, "ApolloOS GPU: Detected RX 480/580/570 (Polaris 20/GFX8)");
    } else if (vendor == 0x10de && (device == 0x1140 || device == 0x0fc0)) { // NVIDIA Kepler
        priv->chip_family = APOLLO_CHIP_FAMILY_KEPLER;
        priv->chip_generation = APOLLO_CHIP_GEN_KEPLER;
        priv->chip_revision = 0x0;
        priv->is_polaris30 = false;
        screen_log("INFO", COLOR_LIGHT_GREEN, "ApolloOS GPU: Detected NVIDIA Kepler GPU");
    } else if (vendor == 0x8086 && device == 0x4565) { // Intel Xe-LP
        priv->chip_family = APOLLO_CHIP_FAMILY_XE;
        priv->chip_generation = APOLLO_CHIP_GEN_XE_LP;
        priv->chip_revision = 0x0;
        priv->is_polaris30 = false;
        screen_log("INFO", COLOR_LIGHT_GREEN, "ApolloOS GPU: Detected Intel Xe-LP GPU");
    } else {
        screen_log("WARN", COLOR_BROWN, "ApolloOS GPU: Unknown chip detected");
        kfree(priv);
        dev->dev_private = NULL;
        return -ENODEV;
    }
    
    // Initialize memory subsystem
    ret = apollo_gpu_init_memory(priv);
    if (ret) {
        screen_log("ERROR", COLOR_RED, "ApolloOS GPU: Failed to initialize memory");
        goto err_free_priv;
    }
    
    serial_print("ApolloOS: apollo_driver_load - about to init Polaris\n");
    
    // Initialize Polaris-specific hardware (for RX 480/580/570)
    if (priv->chip_family == APOLLO_CHIP_FAMILY_POLARIS && 
        (priv->chip_generation == APOLLO_CHIP_GEN_POLARIS20 || 
         priv->chip_generation == APOLLO_CHIP_GEN_POLARIS30)) {
        serial_print("ApolloOS: apollo_driver_load - calling polaris_init\n");
        struct polaris_dev pdev;
        if (polaris_init(&pdev, dev->pci_bus, dev->pci_dev, dev->pci_func) == 0) {
            serial_print("ApolloOS: apollo_driver_load - polaris_init OK, calling polaris_set_mode\n");
            polaris_set_mode(&pdev, 1920, 1080, 32);
            polaris_test_pattern(&pdev);
            screen_log("OK", COLOR_LIGHT_GREEN, "Polaris hardware initialized (1920x1080x32).");
        } else {
            serial_print("ApolloOS: apollo_driver_load - polaris_init FAILED\n");
        }
    } else {
        serial_print("ApolloOS: apollo_driver_load - NOT Polaris (family=");
        // Can't easily print, just note
        serial_print("ApolloOS: apollo_driver_load - chip_family check failed\n");
    }
    
    // Initialize command ring
    ret = apollo_gpu_init_ring(priv);
    if (ret) {
        screen_log("ERROR", COLOR_RED, "ApolloOS GPU: Failed to initialize ring buffer");
        goto err_fini_mem;
    }
    
    // Initialize fence manager
    ret = apollo_gpu_init_fence(priv);
    if (ret) {
        screen_log("ERROR", COLOR_RED, "ApolloOS GPU: Failed to initialize fence manager");
        goto err_fini_fence;
    }
    
    // Initialize interrupts
    ret = apollo_gpu_init_irq(priv);
    if (ret) {
        screen_log("ERROR", COLOR_RED, "ApolloOS GPU: Failed to initialize interrupts");
        goto err_fini_fence;
    }
    
    // Initialize KMS (if applicable)
    // apollo_gpu_init_kms(priv);
    
    screen_log("INFO", COLOR_LIGHT_GREEN, "ApolloOS GPU: Driver loaded successfully");
    return 0;
    
err_fini_irq:
    apollo_gpu_fini_irq(priv);
err_fini_fence:
    apollo_gpu_fini_fence(priv);
err_fini_ring:
    apollo_gpu_fini_ring(priv);
err_fini_mem:
    apollo_gpu_fini_memory(priv);
err_free_priv:
    kfree(priv);
    dev->dev_private = NULL;
    return ret;
}

static int apollo_driver_unload(struct drm_device *dev)
{
    struct apollo_gpu_priv *priv = dev->dev_private;
    
    if (!priv)
        return 0;
    
    screen_log("INFO", COLOR_LIGHT_GREEN, "ApolloOS GPU: Unloading driver");
    
    // FIXME: Wait for pending operations to complete
    
    apollo_gpu_fini_irq(priv);
    apollo_gpu_fini_fence(priv);
    apollo_gpu_fini_ring(priv);
    apollo_gpu_fini_memory(priv);
    
    kfree(priv);
    dev->dev_private = NULL;
    
    screen_log("INFO", COLOR_LIGHT_GREEN, "ApolloOS GPU: Driver unloaded");
}

static int apollo_driver_submit_command(struct drm_device *dev, void *cmd, size_t cmd_size)
{
    struct apollo_gpu_priv *priv = dev->dev_private;
    uint32_t *ring;
    uint32_t wptr, rptr;
    size_t cmd_dwords;
    size_t space;
    
    if (!priv || !priv->ring.ring_buffer)
        return -EINVAL;
    
    // Convert bytes to DWORDs
    cmd_dwords = (cmd_size + 3) / 4;
    
    ring = priv->ring.ring_buffer;
    
    // Read pointers (need to handle properly synchronized access)
    wptr = *priv->ring.wptr;
    rptr = *priv->ring.rptr;
    
    // Calculate available space in ring buffer (in DWORDs)
    if (wptr >= rptr)
        space = priv->ring.ring_size - (wptr - rptr) - 1; // Leave one DWORD empty to distinguish full/empty
    else
        space = rptr - wptr - 1;
    
    if (space < cmd_dwords) {
        // Ring buffer full - in a real driver we'd wait or submit as IB
        screen_log("WARN", COLOR_BROWN, "ApolloOS GPU: Ring buffer full");
        return -ENOSPC;
    }
    
    // Polaris 30 specific: Validate PM4 packets for GFX8
    if (priv->is_polaris30) {
        for (size_t i = 0; i < cmd_dwords; i++) {
            uint32_t pkt = ((uint32_t*)cmd)[i];
            // Check for NOP packet (0xBF000000)
            if ((pkt & 0xFFFF0000) == 0xBF000000) {
                // Valid NOP packet, continue
                continue;
            }
            // TODO: Add more GFX8 specific packet validation as needed
            // For now, accept any packet as valid for testing
        }
    }
    
    // Copy command to ring buffer
    for (size_t i = 0; i < cmd_dwords; i++) {
        if (wptr >= priv->ring.ring_size)
            wptr = 0; // Wrap around
            
        ring[wptr++] = ((uint32_t*)cmd)[i];
    }
    
    // Update write pointer
    if (wptr >= priv->ring.ring_size)
        wptr = 0;
        
    *priv->ring.wptr = wptr;
    
    // Kick the GPU to start processing
    // writel(wptr, priv->registers + APOLLO_REG_RING_WPTR);
    
    // Queue work to process the ring (in real hardware, this would be done by IRQ or polling)
    queue_work(priv->ring.wq, &priv->ring.ib_work);
    
    return 0;
}

static int apollo_driver_wait_for_fence(struct drm_device *dev, uint32_t seqno, uint64_t timeout_ns)
{
    struct apollo_gpu_priv *priv = dev->dev_private;
    uint64_t end_time;
    int ret;
    
    if (!priv)
        return -EINVAL;
    
    // In a real implementation, we'd check the hardware fence status
    // For now, we'll just return immediately if seqno is already signaled
    
    unsigned long flags; spin_lock_irqsave(&priv->fence.lock, &flags);
    if (seqno <= priv->fence.signaled_seqno) {
        spin_unlock_irqrestore(&priv->fence.lock, flags);
        return 0; // Already signaled
    }
    spin_unlock_irqrestore(&priv->fence.lock, flags);
    
    // For simplicity in this initial implementation, we'll just return -ETIME
    // A real implementation would wait for interrupt or poll with timeout
    screen_log("WARN", COLOR_BROWN, "ApolloOS GPU: Fence wait not fully implemented");
    return -ETIME;
}

static void apollo_driver_signal_fence(struct drm_device *dev, uint32_t seqno)
{
    struct apollo_gpu_priv *priv = dev->dev_private;
    
    if (!priv)
        return;
    
    unsigned long flags; spin_lock_irqsave(&priv->fence.lock, &flags);
    if (seqno > priv->fence.signaled_seqno)
        priv->fence.signaled_seqno = seqno;
    spin_unlock_irqrestore(&priv->fence.lock, flags);
    
    // Signal any waiters
    // In a real implementation, we'd wake up waiting threads
}

static int apollo_driver_mode_set(struct drm_device *dev, uint32_t width, uint32_t height, uint32_t refresh_rate)
{
    // FIXME: Implement actual mode setting
    screen_log("INFO", COLOR_LIGHT_GREEN, "ApolloOS GPU: Setting display mode");
    return 0;
}

static irqreturn_t apollo_driver_irq_handler(struct drm_device *dev)
{
    struct apollo_gpu_priv *priv = dev->dev_private;
    uint32_t status;
    
    if (!priv)
        return IRQ_NONE;
    
    // Read interrupt status register
    // status = readl(priv->registers + APOLLO_REG_IRQ_STATUS);
    status = 0; // Placeholder
    
    if (!status)
        return IRQ_NONE;
    
    // Acknowledge interrupts
    // writel(status, priv->registers + APOLLO_REG_IRQ_STATUS);
    
    // Handle different interrupt sources
    if (status & 0x1) { // Vertical blank
        // Handle vblank
    }
    
    if (status & 0x2) { // Fence completed
        // Update fence status
        // priv->fence.signaled_seqno = readl(priv->registers + APOLLO_REG_FENCE_STATUS);
        // Wake up waiters
    }
    
    if (status & 0x4) { // Command buffer finished
        // Process completed commands
    }
    
    return IRQ_HANDLED;
}

// Memory initialization
static int apollo_gpu_init_memory(struct apollo_gpu_priv *priv)
{
    struct drm_device *dev = priv->base;
    uint8_t bus = dev->pci_bus;
    uint8_t dev_id = dev->pci_dev;
    uint8_t func = dev->pci_func;
    
    if (priv->is_polaris30) {
        // Polaris 30: VRAM typically in BAR1, GART in BAR2
        uint32_t bar1_start = pci_read_dword(bus, dev_id, func, PCI_CONFIG_BAR1);
        uint32_t bar2_start = pci_read_dword(bus, dev_id, func, PCI_CONFIG_BAR2);
        
        // Mask out flags (lower 4 bits)
        priv->mem.vram_base = bar1_start & ~0xF;
        priv->mem.gart_base = bar2_start & ~0xF;
        
        // Get sizes (simplified - in reality we'd decode the BARs properly)
        // For now, use placeholders based on known RX 590 GME specs
        priv->mem.vram_size = 8ULL * 1024 * 1024 * 1024; // 8GB
        priv->mem.gart_size = 512ULL * 1024 * 1024;     // 512MB
        
        screen_log("INFO", COLOR_LIGHT_GREEN, "ApolloOS GPU: Polaris 30 - VRAM/GART initialized");
        // Existing generic memory setup for other chips
        priv->mem.vram_base = 0x00000000;
        priv->mem.vram_size = 256 * 1024 * 1024; // 256MB placeholder
        priv->mem.gart_base = 0x10000000;
        priv->mem.gart_size = 64 * 1024 * 1024;  // 64MB placeholder
    }
    
    return 0;
    return 0;
}

// Memory cleanup
static void apollo_gpu_fini_memory(struct apollo_gpu_priv *priv)
{
    // Cleanup memory resources
    priv->mem.vram_size = 0;
    priv->mem.gart_size = 0;
}

// Ring buffer initialization
// Ring buffer initialization
static int apollo_gpu_init_ring(struct apollo_gpu_priv *priv)
{
    // Allocate ring buffer
    priv->ring.ring_size = 64 * 1024; // 64KB ring buffer
    void *ring_raw = kmalloc(priv->ring.ring_size + PAGE_SIZE - 1);
    if (!ring_raw) {
        screen_log("ERROR", COLOR_RED, "ApolloOS GPU: Failed to allocate ring buffer");
        return -ENOMEM;
    }
    priv->ring.ring_buffer = (void*)(((uintptr_t)ring_raw + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    if (!priv->ring.ring_buffer) {
        kfree(ring_raw);
        return -ENOMEM;
    }
    
    memset(priv->ring.ring_buffer, 0, priv->ring.ring_size);
    
    // Allocate fake pointers (in real hardware, these would be mapped registers)
    priv->ring.wptr = kmalloc(sizeof(uint32_t));
    priv->ring.rptr = kmalloc(sizeof(uint32_t));
    if (!priv->ring.wptr || !priv->ring.rptr) {
        kfree(priv->ring.wptr);
        kfree((void*)priv->ring.rptr);
        kfree(priv->ring.ring_buffer);
        priv->ring.ring_buffer = NULL;
        return -ENOMEM;
    }
    
    *priv->ring.wptr = 0;
    *priv->ring.rptr = 0;
    
    // Polaris 30 specific: IB size
    priv->ring.ib_size = 4 * 1024; // 4KB max for IBs in Polaris
    
    // Create workqueue for ring processing
    priv->ring.wq = alloc_workqueue("apollo_ring", 0);
    if (!priv->ring.wq) {
        screen_log("ERROR", COLOR_RED, "ApolloOS GPU: Failed to allocate workqueue");
        kfree(priv->ring.wptr);
        kfree((void*)priv->ring.rptr);
        kfree(priv->ring.ring_buffer);
        priv->ring.ring_buffer = NULL;
        return -ENOMEM;
    }
    
    // Initialize work struct for indirect buffer submission
    INIT_WORK(&priv->ring.ib_work, apollo_gpu_ring_submit_work);
    
    screen_log("INFO", COLOR_LIGHT_GREEN, "ApolloOS GPU: Ring buffer initialized");
    return 0;
}
static void apollo_gpu_fini_ring(struct apollo_gpu_priv *priv)
{
    if (priv->ring.wq) {
        flush_workqueue(priv->ring.wq);
        destroy_workqueue(priv->ring.wq);
    }
    
    if (priv->ring.ib_work.func) {
        cancel_work_sync(&priv->ring.ib_work);
    }
    
    kfree(priv->ring.wptr);
    kfree((void*)priv->ring.rptr);
    
    if (priv->ring.ring_buffer) {
        kfree(priv->ring.ring_buffer);
        priv->ring.ring_buffer = NULL;
    }
    
    priv->ring.ring_size = 0;
}

// Fence manager initialization
static int apollo_gpu_init_fence(struct apollo_gpu_priv *priv)
{
    priv->fence.seqno = 0;
    priv->fence.signaled_seqno = 0;
    priv->fence.waited_seqno = 0;
    
    if (priv->is_polaris30) {
        // Reserve space in VRAM for fence status
        priv->fence.addr = priv->mem.vram_base + 0x1000; // 4KB offset
        priv->fence.addr_size = 8; // 64-bit sequence number
    }
    
    // Create workqueue for fence timeout handling
    priv->fence.wq = alloc_workqueue("apollo_fence", 0);
    if (!priv->fence.wq) {
        screen_log("ERROR", COLOR_RED, "ApolloOS GPU: Failed to allocate fence workqueue");
        return -ENOMEM;
    }
    
    INIT_WORK(&priv->fence.timeout_work, apollo_gpu_fence_timeout_work);
    
    screen_log("INFO", COLOR_LIGHT_GREEN, "ApolloOS GPU: Fence manager initialized");
    return 0;
}
// Fence cleanup
static void apollo_gpu_fini_fence(struct apollo_gpu_priv *priv)
{
    if (priv->fence.wq) {
        flush_workqueue(priv->fence.wq);
        destroy_workqueue(priv->fence.wq);
    }

    cancel_work_sync(&priv->fence.timeout_work);

    priv->fence.seqno = 0;
    priv->fence.signaled_seqno = 0;
    priv->fence.waited_seqno = 0;
}


// Interrupt initialization
static int apollo_gpu_init_irq(struct apollo_gpu_priv *priv)
{
    // In a real implementation, we'd request the IRQ from the PCI device
    // and register our interrupt handler
    
    priv->irq.enabled = false;
    priv->irq.handler = NULL;
    
    screen_log("INFO", COLOR_LIGHT_GREEN, "ApolloOS GPU: Interrupts initialized");
    return 0;
}

static void apollo_gpu_fini_irq(struct apollo_gpu_priv *priv)
{
    // In a real implementation, we'd free the IRQ
    priv->irq.enabled = false;
    priv->irq.handler = NULL;
}

// Work function for ring processing
static void apollo_gpu_ring_submit_work(struct work_struct *work)
{
    struct apollo_gpu_priv *priv = container_of(work, struct apollo_gpu_priv, ring.ib_work);
    
    if (!priv)
        return;
    
    // In a real implementation, we'd:
    // 1. Check if there's work in the ring buffer
    // 2. Submit it to the GPU
    // 3. Update the read pointer as the GPU consumes commands
    
    screen_log("DEBUG", COLOR_CYAN, "ApolloOS GPU: Processing ring buffer work");
    // Placeholder implementation
}

// Work function for fence timeout
static void apollo_gpu_fence_timeout_work(struct work_struct *work)
{
    struct apollo_gpu_priv *priv = container_of(work, struct apollo_gpu_priv, fence.timeout_work);
    
    if (!priv)
        return;
    
    screen_log("WARN", COLOR_BROWN, "ApolloOS GPU: Fence timeout occurred");
    // Handle timeout - maybe reset GPU or signal error
}

// PCI device probe function to be called from PCI subsystem
int apollo_gpu_pci_probe(uint8_t bus, uint8_t dev, uint8_t func)
{
    return drm_pci_device_probe(bus, dev, func, &apollo_driver, apollo_pci_id_table);
}

void apollo_gpu_pci_remove(uint8_t bus, uint8_t dev, uint8_t func)
{
    drm_pci_device_remove(bus, dev, func, &apollo_driver);
}

// Module initialization
module_init(apollo_driver_init);
module_exit(apollo_driver_exit);

MODULE_DESCRIPTION("ApolloOS Universal GPU Driver")
MODULE_AUTHOR("ApolloOS Developers")
MODULE_LICENSE("GPL")
