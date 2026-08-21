#include <dma.h>
#include <kheap.h>
#include <pmm.h>
#include <vmm.h>
#include <screen.h>
#include <pci.h>
#include <string.h>

/* Simple memset */
static void simple_memset(void *dst, int val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)val;
}

/* Default DMA operations (identity mapping - no IOMMU) */
static void *default_alloc_coherent(void *dev, size_t size, uint64_t *dma_handle, int flags) {
    (void)dev; (void)flags;
    uint64_t phys = pmm_alloc_block();
    if (!phys) return NULL;
    
    void *virt = (void *)phys;  /* Identity mapping */
    if (dma_handle) *dma_handle = phys;
    return virt;
}

static void default_free_coherent(void *dev, size_t size, void *cpu_addr, uint64_t dma_handle) {
    (void)dev; (void)size;
    pmm_free_block((uint64_t)cpu_addr);
    (void)dma_handle;
}

static int default_map_page(void *dev, uint64_t page, size_t offset, size_t size, int dir, uint64_t *dma_addr) {
    (void)dev; (void)dir; (void)offset;
    if (dma_addr) *dma_addr = page;
    return 0;
}

static void default_unmap_page(void *dev, uint64_t dma_addr, size_t size, int dir) {
    (void)dev; (void)dma_addr; (void)size; (void)dir;
}

static int default_map_sg(void *dev, struct scatterlist *sg, int nents, int dir) {
    (void)dev; (void)dir;
    for (int i = 0; i < nents; i++) {
        sg[i].dma_address = (uint32_t)sg[i].page;
        sg[i].dma_length = (uint32_t)sg[i].length;
    }
    return nents;
}

static void default_unmap_sg(void *dev, struct scatterlist *sg, int nents, int dir) {
    (void)dev; (void)sg; (void)nents; (void)dir;
}

static void default_sync_single_for_cpu(void *dev, uint64_t dma_addr, size_t size, int dir) {
    (void)dev; (void)dma_addr; (void)size; (void)dir;
}

static void default_sync_single_for_device(void *dev, uint64_t dma_addr, size_t size, int dir) {
    (void)dev; (void)dma_addr; (void)size; (void)dir;
}

static void default_sync_sg_for_cpu(void *dev, struct scatterlist *sg, int nents, int dir) {
    (void)dev; (void)sg; (void)nents; (void)dir;
}

static void default_sync_sg_for_device(void *dev, struct scatterlist *sg, int nents, int dir) {
    (void)dev; (void)sg; (void)nents; (void)dir;
}

static int default_dma_supported(void *dev, uint64_t mask) {
    (void)dev;
    return (mask & 0xFFFFFFFFFFFFULL) != 0;
}

static int default_set_dma_mask(void *dev, uint64_t mask) {
    (void)dev; (void)mask;
    return 0;
}

/* Default DMA ops */
struct dma_ops default_dma_ops = {
    .alloc_coherent = default_alloc_coherent,
    .free_coherent = default_free_coherent,
    .map_page = default_map_page,
    .unmap_page = default_unmap_page,
    .map_sg = default_map_sg,
    .unmap_sg = default_unmap_sg,
    .sync_single_for_cpu = default_sync_single_for_cpu,
    .sync_single_for_device = default_sync_single_for_device,
    .sync_sg_for_cpu = default_sync_sg_for_cpu,
    .sync_sg_for_device = default_sync_sg_for_device,
    .dma_supported = default_dma_supported,
    .set_dma_mask = default_set_dma_mask,
};

/* Global DMA state */
static struct dma_dev *dma_devices = NULL;

void dma_init(void) {
    /* Initialize default DMA ops for all devices */
    screen_log("OK", COLOR_LIGHT_GREEN, "DMA subsystem initialized (default ops)");
}

void dma_exit(void) {
    /* Cleanup */
}

/* Device DMA management */
struct dma_dev *dma_dev_get(void *device) {
    struct dma_dev *ddev = dma_devices;
    while (ddev) {
        if (ddev->device == device) return ddev;
        ddev = ddev->next;
    }
    return NULL;
}

struct dma_dev *dma_dev_alloc(void *device) {
    struct dma_dev *ddev = kmalloc(sizeof(struct dma_dev));
    if (!ddev) return NULL;
    
    ddev->device = device;
    ddev->ops = &default_dma_ops;
    ddev->dma_mask = 0xFFFFFFFFFFFFULL;
    ddev->coherent_dma_mask = 0xFFFFFFFFFFFFULL;
    ddev->sg_table = NULL;
    ddev->next = dma_devices;
    dma_devices = ddev;
    
    return ddev;
}

void dma_dev_free(struct dma_dev *ddev) {
    if (!ddev) return;
    
    /* Remove from list */
    struct dma_dev **pp = &dma_devices;
    while (*pp) {
        if (*pp == ddev) {
            *pp = ddev->next;
            break;
        }
        pp = &(*pp)->next;
    }
    
    kfree(ddev);
}

int dma_set_mask(struct dma_dev *ddev, uint64_t mask) {
    if (!ddev || !ddev->ops->set_dma_mask) return -1;
    int ret = ddev->ops->set_dma_mask(ddev->device, mask);
    if (ret == 0) ddev->dma_mask = mask;
    return ret;
}

int dma_set_coherent_mask(struct dma_dev *ddev, uint64_t mask) {
    if (!ddev) return -1;
    ddev->coherent_dma_mask = mask;
    return 0;
}

void *dma_alloc_coherent(struct dma_dev *ddev, size_t size, uint64_t *dma_handle, int flags) {
    if (!ddev || !ddev->ops->alloc_coherent) return NULL;
    return ddev->ops->alloc_coherent(ddev->device, size, dma_handle, flags);
}

void dma_free_coherent(struct dma_dev *ddev, size_t size, void *cpu_addr, uint64_t dma_handle) {
    if (!ddev || !ddev->ops->free_coherent) return;
    ddev->ops->free_coherent(ddev->device, size, cpu_addr, dma_handle);
}

int dma_map_page(struct dma_dev *ddev, uint64_t page, size_t offset, size_t size, int dir, uint64_t *dma_addr) {
    if (!ddev || !ddev->ops->map_page) return -1;
    return ddev->ops->map_page(ddev->device, page, offset, size, dir, dma_addr);
}

void dma_unmap_page(struct dma_dev *ddev, uint64_t dma_addr, size_t size, int dir) {
    if (!ddev || !ddev->ops->unmap_page) return;
    ddev->ops->unmap_page(ddev->device, dma_addr, size, dir);
}

int dma_map_sg(struct dma_dev *ddev, struct scatterlist *sg, int nents, int dir) {
    if (!ddev || !ddev->ops->map_sg) return -1;
    return ddev->ops->map_sg(ddev->device, sg, nents, dir);
}

void dma_unmap_sg(struct dma_dev *ddev, struct scatterlist *sg, int nents, int dir) {
    if (!ddev || !ddev->ops->unmap_sg) return;
    ddev->ops->unmap_sg(ddev->device, sg, nents, dir);
}

void dma_sync_single_for_cpu(struct dma_dev *ddev, uint64_t dma_addr, size_t size, int dir) {
    if (!ddev || !ddev->ops->sync_single_for_cpu) return;
    ddev->ops->sync_single_for_cpu(ddev->device, dma_addr, size, dir);
}

void dma_sync_single_for_device(struct dma_dev *ddev, uint64_t dma_addr, size_t size, int dir) {
    if (!ddev || !ddev->ops->sync_single_for_device) return;
    ddev->ops->sync_single_for_device(ddev->device, dma_addr, size, dir);
}

void dma_sync_sg_for_cpu(struct dma_dev *ddev, struct scatterlist *sg, int nents, int dir) {
    if (!ddev || !ddev->ops->sync_sg_for_cpu) return;
    ddev->ops->sync_sg_for_cpu(ddev->device, sg, nents, dir);
}

void dma_sync_sg_for_device(struct dma_dev *ddev, struct scatterlist *sg, int nents, int dir) {
    if (!ddev || !ddev->ops->sync_sg_for_device) return;
    ddev->ops->sync_sg_for_device(ddev->device, sg, nents, dir);
}

int dma_supported(struct dma_dev *ddev, uint64_t mask) {
    if (!ddev || !ddev->ops->dma_supported) return 0;
    return ddev->ops->dma_supported(ddev->device, mask);
}

int dma_set_dma_mask(struct dma_dev *ddev, uint64_t mask) {
    if (!ddev || !ddev->ops->set_dma_mask) return -1;
    return ddev->ops->set_dma_mask(ddev->device, mask);
}

/* Scatter-gather helpers */
struct scatterlist *sg_alloc_chain(int nents, int max_ents) {
    (void)max_ents;
    struct scatterlist *sg = kmalloc(nents * sizeof(struct scatterlist));
    if (!sg) return NULL;
    simple_memset(sg, 0, nents * sizeof(struct scatterlist));
    return sg;
}

void sg_free_chain(struct scatterlist *sg) {
    kfree(sg);
}

void sg_init_table(struct scatterlist *sg, int nents) {
    for (int i = 0; i < nents; i++) {
        sg[i].page = 0;
        sg[i].length = 0;
        sg[i].offset = 0;
        sg[i].dma_address = 0;
        sg[i].dma_length = 0;
    }
}

void sg_set_page(struct scatterlist *sg, uint64_t page, size_t len, size_t offset) {
    sg->page = page;
    sg->length = len;
    sg->offset = offset;
}

void sg_mark_end(struct scatterlist *sg) {
    sg->page = 0;
    sg->length = 0;
    sg->offset = 0;
}

struct scatterlist *sg_next(struct scatterlist *sg) {
    return sg + 1;
}

/* IOMMU stub implementation */
int iommu_init(void) {
    screen_log("INFO", COLOR_LIGHT_CYAN, "IOMMU: Not present, using identity mapping");
    return 0;
}

void iommu_exit(void) {
}

int iommu_map(void *dev, uint64_t iova, uint64_t paddr, size_t size, int prot) {
    (void)dev; (void)iova; (void)paddr; (void)size; (void)prot;
    return 0;  /* Identity mapping */
}

void iommu_unmap(void *dev, uint64_t iova, size_t size) {
    (void)dev; (void)iova; (void)size;
}

int iommu_attach_device(void *dev) {
    (void)dev;
    return 0;
}

void iommu_detach_device(void *dev) {
    (void)dev;
}

/* PCI device DMA setup */
int pci_dma_init(void *pci_dev) {
    struct dma_dev *ddev = dma_dev_alloc(pci_dev);
    if (!ddev) return -1;
    return 0;
}

void pci_dma_exit(void *pci_dev) {
    struct dma_dev *ddev = dma_dev_get(pci_dev);
    if (ddev) dma_dev_free(ddev);
}