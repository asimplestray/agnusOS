#ifndef DMA_H
#define DMA_H

#include <stdint.h>
#include <stddef.h>

/* DMA direction */
#define DMA_BIDIRECTIONAL   0
#define DMA_TO_DEVICE       1
#define DMA_FROM_DEVICE     2
#define DMA_NONE            3

/* DMA attributes */
#define DMA_ATTR_SKIP_CPU_SYNC    (1 << 0)
#define DMA_ATTR_FORCE_CONTIGUOUS (1 << 1)
#define DMA_ATTR_ALLOC_SINGLE_PAGES (1 << 2)
#define DMA_ATTR_NO_KERNEL_MAPPING (1 << 3)
#define DMA_ATTR_WRITE_COMBINE    (1 << 4)
#define DMA_ATTR_NO_WARN          (1 << 5)

/* DMA coherent allocation flags */
#define DMA_COHERENT_FLAG_DEFAULT 0

/* Scatter-gather list */
struct scatterlist {
    uint64_t page;      /* Physical page address */
    size_t length;      /* Length in bytes */
    size_t offset;      /* Offset within page */
    uint32_t dma_address; /* DMA bus address */
    uint32_t dma_length;  /* DMA length */
};

struct sg_table {
    struct scatterlist *sgl;
    unsigned int nents;
    unsigned int orig_nents;
};

/* DMA operations structure */
struct dma_ops {
    void *(*alloc_coherent)(void *dev, size_t size, uint64_t *dma_handle, int flags);
    void (*free_coherent)(void *dev, size_t size, void *cpu_addr, uint64_t dma_handle);
    
    int (*map_page)(void *dev, uint64_t page, size_t offset, size_t size, int dir, uint64_t *dma_addr);
    void (*unmap_page)(void *dev, uint64_t dma_addr, size_t size, int dir);
    
    int (*map_sg)(void *dev, struct scatterlist *sg, int nents, int dir);
    void (*unmap_sg)(void *dev, struct scatterlist *sg, int nents, int dir);
    
    void (*sync_single_for_cpu)(void *dev, uint64_t dma_addr, size_t size, int dir);
    void (*sync_single_for_device)(void *dev, uint64_t dma_addr, size_t size, int dir);
    void (*sync_sg_for_cpu)(void *dev, struct scatterlist *sg, int nents, int dir);
    void (*sync_sg_for_device)(void *dev, struct scatterlist *sg, int nents, int dir);
    
    int (*dma_supported)(void *dev, uint64_t mask);
    int (*set_dma_mask)(void *dev, uint64_t mask);
};

/* Device DMA structure */
struct dma_dev {
    void *device;
    struct dma_ops *ops;
    uint64_t dma_mask;
    uint64_t coherent_dma_mask;
    struct sg_table *sg_table;
    struct dma_dev *next;
};

/* IOMMU operations */
struct iommu_ops {
    int (*init)(void);
    void (*exit)(void);
    int (*map)(void *dev, uint64_t iova, uint64_t paddr, size_t size, int prot);
    void (*unmap)(void *dev, uint64_t iova, size_t size);
    int (*domain_init)(void *domain);
    void (*domain_exit)(void *domain);
    int (*attach_device)(void *domain, void *dev);
    void (*detach_device)(void *domain, void *dev);
};

/* Global DMA API */
void dma_init(void);
void dma_exit(void);

int dma_set_mask(struct dma_dev *ddev, uint64_t mask);
int dma_set_coherent_mask(struct dma_dev *ddev, uint64_t mask);

void *dma_alloc_coherent(struct dma_dev *ddev, size_t size, uint64_t *dma_handle, int flags);
void dma_free_coherent(struct dma_dev *ddev, size_t size, void *cpu_addr, uint64_t dma_handle);

int dma_map_page(struct dma_dev *ddev, uint64_t page, size_t offset, size_t size, int dir, uint64_t *dma_addr);
void dma_unmap_page(struct dma_dev *ddev, uint64_t dma_addr, size_t size, int dir);

int dma_map_sg(struct dma_dev *ddev, struct scatterlist *sg, int nents, int dir);
void dma_unmap_sg(struct dma_dev *ddev, struct scatterlist *sg, int nents, int dir);

void dma_sync_single_for_cpu(struct dma_dev *ddev, uint64_t dma_addr, size_t size, int dir);
void dma_sync_single_for_device(struct dma_dev *ddev, uint64_t dma_addr, size_t size, int dir);
void dma_sync_sg_for_cpu(struct dma_dev *ddev, struct scatterlist *sg, int nents, int dir);
void dma_sync_sg_for_device(struct dma_dev *ddev, struct scatterlist *sg, int nents, int dir);

int dma_supported(struct dma_dev *ddev, uint64_t mask);
int dma_set_dma_mask(struct dma_dev *ddev, uint64_t mask);

/* Scatter-gather helpers */
struct scatterlist *sg_alloc_chain(int nents, int max_ents);
void sg_free_chain(struct scatterlist *sg);
void sg_init_table(struct scatterlist *sg, int nents);
void sg_set_page(struct scatterlist *sg, uint64_t page, size_t len, size_t offset);
void sg_mark_end(struct scatterlist *sg);
struct scatterlist *sg_next(struct scatterlist *sg);

/* Default DMA ops for systems without IOMMU */
extern struct dma_ops default_dma_ops;

/* IOMMU API */
int iommu_init(void);
void iommu_exit(void);
int iommu_map(void *dev, uint64_t iova, uint64_t paddr, size_t size, int prot);
void iommu_unmap(void *dev, uint64_t iova, size_t size);
int iommu_attach_device(void *dev);
void iommu_detach_device(void *dev);

/* PCI device DMA setup */
int pci_dma_init(void *pci_dev);
void pci_dma_exit(void *pci_dev);

#endif