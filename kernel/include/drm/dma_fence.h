#ifndef _DRM_DMA_FENCE_H_
#define _DRM_DMA_FENCE_H_

#include <stdint.h>
#include <stdbool.h>
#include <spinlock.h>
#include <workqueue.h>

/* Error codes */
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

struct dma_fence_ops;
struct dma_fence_cb;

/**
 * struct dma_fence - synchronization primitive for GPU operations
 * @refcount: reference count for the fence
 * @ops: operations for this fence type
 * @lock: spinlock protecting the fence state
 * @context: execution context this fence belongs to
 * @seqno: sequence number of this fence
 * @flags: fence flags
 * @timestamp: when the fence was signaled
 * @error: error status if signaled with error
 * @cb_list: list of callbacks waiting for this fence
 */
struct dma_fence {
    int refcount;
    const struct dma_fence_ops *ops;
    spinlock_irq_t lock;
    uint64_t context;
    uint64_t seqno;
    unsigned long flags;
    uint64_t timestamp;
    int error;
    struct dma_fence_cb *cb_list;
};

/**
 * struct dma_fence_ops - operations for a dma_fence type
 * @get_driver_name: return driver name for debugging
 * @get_timeline_name: return timeline name for debugging
 * @enable_signaling: enable signaling on this fence
 * @signaled: check if fence is already signaled
 * @wait: wait for fence to be signaled
 * @release: release fence resources
 * @fill_driver_data: fill driver-specific debug data
 * @fence_value_str: convert seqno to string
 * @timeline_value_str: convert timeline value to string
 */
struct dma_fence_ops {
    const char * (*get_driver_name)(struct dma_fence *fence);
    const char * (*get_timeline_name)(struct dma_fence *fence);
    bool (*enable_signaling)(struct dma_fence *fence);
    bool (*signaled)(struct dma_fence *fence);
    int (*wait)(struct dma_fence *fence, bool intr, int64_t timeout_ns);
    void (*release)(struct dma_fence *fence);
    void (*fill_driver_data)(struct dma_fence *fence, void *data);
    int (*fence_value_str)(struct dma_fence *fence, char *str, int size);
    int (*timeline_value_str)(struct dma_fence *fence, char *str, int size);
};

/**
 * struct dma_fence_cb - callback for fence signaling
 * @node: list node
 * @func: callback function
 */
struct dma_fence_cb {
    struct dma_fence_cb *next;
    void (*func)(struct dma_fence *fence, struct dma_fence_cb *cb);
};

/**
 * struct dma_fence_chain - chain of fences for timeline
 * @fence: the fence
 * @prev: previous fence in chain
 * @next: next fence in chain
 */
struct dma_fence_chain {
    struct dma_fence fence;
    struct dma_fence *prev;
    struct dma_fence *next;
};

/* Fence flags */
#define DMA_FENCE_FLAG_SIGNALED_BIT    0
#define DMA_FENCE_FLAG_TIMESTAMP_BIT   1
#define DMA_FENCE_FLAG_ENABLE_SIGNAL_BIT 2
#define DMA_FENCE_FLAG_USER_BITS_SHIFT 3

#define DMA_FENCE_FLAG_SIGNALED       (1UL << DMA_FENCE_FLAG_SIGNALED_BIT)
#define DMA_FENCE_FLAG_TIMESTAMP      (1UL << DMA_FENCE_FLAG_TIMESTAMP_BIT)
#define DMA_FENCE_FLAG_ENABLE_SIGNAL  (1UL << DMA_FENCE_FLAG_ENABLE_SIGNAL_BIT)

/* Default timeline operations */
extern const struct dma_fence_ops dma_fence_default_ops;

/* Fence initialization */
void dma_fence_init(struct dma_fence *fence, const struct dma_fence_ops *ops,
                    spinlock_irq_t *lock, uint64_t context, uint64_t seqno);

/* Reference counting */
struct dma_fence *dma_fence_get(struct dma_fence *fence);
void dma_fence_put(struct dma_fence *fence);

/* Signaling */
bool dma_fence_signal(struct dma_fence *fence);
bool dma_fence_signal_timestamp(struct dma_fence *fence, uint64_t timestamp);
bool dma_fence_signal_error(struct dma_fence *fence, int error);

/* Waiting */
int dma_fence_wait(struct dma_fence *fence, bool intr);
int dma_fence_wait_timeout(struct dma_fence *fence, bool intr, int64_t timeout_ns);
int dma_fence_wait_any_timeout(struct dma_fence **fences, uint32_t count,
                               bool intr, int64_t timeout_ns, uint32_t *idx);

/* Callbacks */
int dma_fence_add_callback(struct dma_fence *fence, struct dma_fence_cb *cb,
                           void (*func)(struct dma_fence *, struct dma_fence_cb *));
bool dma_fence_remove_callback(struct dma_fence *fence, struct dma_fence_cb *cb);

/* Status checks */
static inline bool dma_fence_is_signaled(struct dma_fence *fence)
{
    return !!(fence->flags & DMA_FENCE_FLAG_SIGNALED);
}

static inline bool dma_fence_is_later(struct dma_fence *fence_a,
                                      struct dma_fence *fence_b)
{
    return (int64_t)(fence_a->seqno - fence_b->seqno) > 0;
}

/* Timeline management */
struct dma_fence *dma_fence_alloc_timeline(uint64_t context);
void dma_fence_free_timeline(struct dma_fence *timeline);

/* Fence array */
struct dma_fence_array {
    struct dma_fence base;
    struct dma_fence **fences;
    uint32_t num_fences;
    bool signaled;
};

struct dma_fence *dma_fence_array_create(struct dma_fence **fences, uint32_t num_fences);
void dma_fence_array_free(struct dma_fence *fence);

/* Debug */
const char *dma_fence_get_driver_name(struct dma_fence *fence);
const char *dma_fence_get_timeline_name(struct dma_fence *fence);

#endif /* _DRM_DMA_FENCE_H_ */