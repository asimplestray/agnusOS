#ifndef _DRM_DMA_RESV_H_
#define _DRM_DMA_RESV_H_

#include <stdint.h>
#include <stdbool.h>
#include <spinlock.h>
#include "dma_fence.h"

/* Error codes (duplicate from dma_fence.h for standalone compilation) */
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif

#ifndef EINVAL
#define EINVAL 22
#endif

#ifndef ENOMEM
#define ENOMEM 12
#endif

#ifndef EBUSY
#define EBUSY 16
#endif

/* Maximum number of fences in a reservation object */
#define DMA_RESV_MAX_FENCES 32

/* Reservation object usage flags */
#define DMA_RESV_USAGE_BOOKKEEP  (1 << 0)  /* Internal bookkeeping */
#define DMA_RESV_USAGE_READ      (1 << 1)  /* Read access */
#define DMA_RESV_USAGE_WRITE     (1 << 2)  /* Write access */
#define DMA_RESV_USAGE_KERNEL    (1 << 3)  /* Kernel-only access */

/**
 * struct dma_fence_list - list of fences for a specific usage
 * @fences: array of fences
 * @count: number of fences
 * @shared_count: number of shared (read) fences
 */
struct dma_fence_list {
    struct dma_fence **fences;
    uint32_t count;
    uint32_t shared_count;
};

/**
 * struct dma_resv - reservation object for buffer synchronization
 * @lock: spinlock protecting this reservation object
 * @fence: current exclusive fence (writer)
 * @fences: array of shared fences (readers)
 * @num_fences: number of shared fences
 * @max_fences: maximum number of shared fences
 * @seq: sequence number for validation
 * @owner: pointer to owning BO (for debugging)
 */
struct dma_resv {
    spinlock_irq_t lock;
    struct dma_fence *fence;           /* Exclusive fence (1 writer) */
    struct dma_fence **fences;         /* Shared fences (N readers) */
    uint32_t num_fences;
    uint32_t max_fences;
    uint64_t seq;
    void *owner;                       /* Owning BO for debugging */
};

/* Reservation object initialization */
void dma_resv_init(struct dma_resv *obj);
void dma_resv_fini(struct dma_resv *obj);

/* Locking */
void dma_resv_lock(struct dma_resv *obj);
void dma_resv_unlock(struct dma_resv *obj);
int dma_resv_lock_interruptible(struct dma_resv *obj);
int dma_resv_trylock(struct dma_resv *obj);

/* Fence management */
int dma_resv_add_fence(struct dma_resv *obj, struct dma_fence *fence,
                       unsigned int usage);
int dma_resv_add_excl_fence(struct dma_resv *obj, struct dma_fence *fence);
int dma_resv_add_shared_fence(struct dma_resv *obj, struct dma_fence *fence);
void dma_resv_replace_fences(struct dma_resv *obj, struct dma_fence *fence);

/* Waiting */
int dma_resv_wait_timeout(struct dma_resv *obj, bool intr,
                          int64_t timeout_ns, unsigned int usage);

/* Fence retrieval */
struct dma_fence *dma_resv_get_excl(struct dma_resv *obj);
struct dma_fence **dma_resv_get_shared(struct dma_resv *obj, uint32_t *count);

/* Copy fences from src to dst */
int dma_resv_copy_fences(struct dma_resv *dst, struct dma_resv *src);

/* Reservation object test status */
bool dma_resv_test_signaled(struct dma_resv *obj, unsigned int usage);

/* Debug */
void dma_resv_debug_dump(struct dma_resv *obj, const char *prefix);

#endif /* _DRM_DMA_RESV_H_ */