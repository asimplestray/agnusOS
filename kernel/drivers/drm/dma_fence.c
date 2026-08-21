#include <drm/dma_fence.h>
#include <stdlib.h>
#include <string.h>
#include <kheap.h>
#include <workqueue.h>
#include <spinlock.h>
#include <timer.h>
#include <serial.h>
#include <stdio.h>

/* Default fence operations */
static const char *dma_fence_default_get_driver_name(struct dma_fence *fence)
{
    (void)fence;
    return "default";
}

static const char *dma_fence_default_get_timeline_name(struct dma_fence *fence)
{
    (void)fence;
    return "default";
}

static bool dma_fence_default_enable_signaling(struct dma_fence *fence)
{
    (void)fence;
    return true;
}

static bool dma_fence_default_signaled(struct dma_fence *fence)
{
    return dma_fence_is_signaled(fence);
}

static int dma_fence_default_wait(struct dma_fence *fence, bool intr, int64_t timeout_ns)
{
    (void)intr;
    
    if (dma_fence_is_signaled(fence))
        return 0;
    
    if (timeout_ns == 0)
        return -ETIMEDOUT;
    
    uint64_t start = timer_get_ticks();
    uint64_t timeout_ticks = (timeout_ns > 0) ? ((uint64_t)timeout_ns / 10000000ULL) : UINT64_MAX;
    
    while (!dma_fence_is_signaled(fence)) {
        if (timeout_ns > 0) {
            uint64_t elapsed = timer_get_ticks() - start;
            if (elapsed >= timeout_ticks)
                return -ETIMEDOUT;
        }
        
        __asm__ volatile("pause");
    }
    
    return 0;
}

static void dma_fence_default_release(struct dma_fence *fence)
{
    kfree(fence);
}

static void dma_fence_default_fill_driver_data(struct dma_fence *fence, void *data)
{
    (void)fence;
    (void)data;
}

static int dma_fence_default_fence_value_str(struct dma_fence *fence, char *str, int size)
{
    return snprintf(str, size, "%llu", (unsigned long long)fence->seqno);
}

static int dma_fence_default_timeline_value_str(struct dma_fence *fence, char *str, int size)
{
    return snprintf(str, size, "%llu", (unsigned long long)fence->context);
}

const struct dma_fence_ops dma_fence_default_ops = {
    .get_driver_name = dma_fence_default_get_driver_name,
    .get_timeline_name = dma_fence_default_get_timeline_name,
    .enable_signaling = dma_fence_default_enable_signaling,
    .signaled = dma_fence_default_signaled,
    .wait = dma_fence_default_wait,
    .release = dma_fence_default_release,
    .fill_driver_data = dma_fence_default_fill_driver_data,
    .fence_value_str = dma_fence_default_fence_value_str,
    .timeline_value_str = dma_fence_default_timeline_value_str,
};

void dma_fence_init(struct dma_fence *fence, const struct dma_fence_ops *ops,
                    spinlock_irq_t *lock, uint64_t context, uint64_t seqno)
{
    if (!fence || !ops || !lock)
        return;
    
    fence->refcount = 1;
    fence->ops = ops;
    fence->lock = *lock;
    fence->context = context;
    fence->seqno = seqno;
    fence->flags = 0;
    fence->timestamp = 0;
    fence->error = 0;
    fence->cb_list = NULL;
    
    spinlock_init(&fence->lock.lock);
}

struct dma_fence *dma_fence_get(struct dma_fence *fence)
{
    if (!fence)
        return NULL;
    
    __sync_fetch_and_add(&fence->refcount, 1);
    return fence;
}

void dma_fence_put(struct dma_fence *fence)
{
    if (!fence)
        return;
    
    if (__sync_sub_and_fetch(&fence->refcount, 1) == 0) {
        if (fence->ops && fence->ops->release)
            fence->ops->release(fence);
    }
}

static void dma_fence_run_callbacks(struct dma_fence *fence)
{
    struct dma_fence_cb *cb = fence->cb_list;
    fence->cb_list = NULL;
    
    while (cb) {
        struct dma_fence_cb *next = cb->next;
        cb->next = NULL;
        if (cb->func)
            cb->func(fence, cb);
        cb = next;
    }
}

bool dma_fence_signal(struct dma_fence *fence)
{
    if (!fence)
        return false;
    
    unsigned long flags;
    spin_lock_irqsave(&fence->lock, &flags);
    
    if (fence->flags & DMA_FENCE_FLAG_SIGNALED) {
        spin_unlock_irqrestore(&fence->lock, flags);
        return false;
    }
    
    fence->flags |= DMA_FENCE_FLAG_SIGNALED;
    fence->timestamp = timer_get_ticks();
    
    spin_unlock_irqrestore(&fence->lock, flags);
    
    dma_fence_run_callbacks(fence);
    
    return true;
}

bool dma_fence_signal_timestamp(struct dma_fence *fence, uint64_t timestamp)
{
    if (!fence)
        return false;
    
    unsigned long flags;
    spin_lock_irqsave(&fence->lock, &flags);
    
    if (fence->flags & DMA_FENCE_FLAG_SIGNALED) {
        spin_unlock_irqrestore(&fence->lock, flags);
        return false;
    }
    
    fence->flags |= DMA_FENCE_FLAG_SIGNALED;
    fence->timestamp = timestamp;
    
    spin_unlock_irqrestore(&fence->lock, flags);
    
    dma_fence_run_callbacks(fence);
    
    return true;
}

bool dma_fence_signal_error(struct dma_fence *fence, int error)
{
    if (!fence)
        return false;
    
    unsigned long flags;
    spin_lock_irqsave(&fence->lock, &flags);
    
    if (fence->flags & DMA_FENCE_FLAG_SIGNALED) {
        spin_unlock_irqrestore(&fence->lock, flags);
        return false;
    }
    
    fence->flags |= DMA_FENCE_FLAG_SIGNALED;
    fence->error = error;
    fence->timestamp = timer_get_ticks();
    
    spin_unlock_irqrestore(&fence->lock, flags);
    
    dma_fence_run_callbacks(fence);
    
    return true;
}

int dma_fence_wait(struct dma_fence *fence, bool intr)
{
    if (!fence)
        return -EINVAL;
    
    if (fence->ops && fence->ops->wait)
        return fence->ops->wait(fence, intr, -1);
    
    return dma_fence_default_wait(fence, intr, -1);
}

int dma_fence_wait_timeout(struct dma_fence *fence, bool intr, int64_t timeout_ns)
{
    if (!fence)
        return -EINVAL;
    
    if (fence->ops && fence->ops->wait)
        return fence->ops->wait(fence, intr, timeout_ns);
    
    return dma_fence_default_wait(fence, intr, timeout_ns);
}

int dma_fence_wait_any_timeout(struct dma_fence **fences, uint32_t count,
                               bool intr, int64_t timeout_ns, uint32_t *idx)
{
    (void)intr;
    
    if (!fences || count == 0)
        return -EINVAL;
    
    for (uint32_t i = 0; i < count; i++) {
        if (dma_fence_is_signaled(fences[i])) {
            if (idx)
                *idx = i;
            return 0;
        }
    }
    
    if (timeout_ns == 0)
        return -ETIMEDOUT;
    
    uint64_t start = timer_get_ticks();
    uint64_t timeout_ticks = (timeout_ns > 0) ? ((uint64_t)timeout_ns / 10000000ULL) : UINT64_MAX;
    
    while (1) {
        for (uint32_t i = 0; i < count; i++) {
            if (dma_fence_is_signaled(fences[i])) {
                if (idx)
                    *idx = i;
                return 0;
            }
        }
        
        if (timeout_ns > 0) {
            uint64_t elapsed = timer_get_ticks() - start;
            if (elapsed >= timeout_ticks)
                return -ETIMEDOUT;
        }
        
        __asm__ volatile("pause");
    }
}

int dma_fence_add_callback(struct dma_fence *fence, struct dma_fence_cb *cb,
                           void (*func)(struct dma_fence *, struct dma_fence_cb *))
{
    if (!fence || !cb || !func)
        return -EINVAL;
    
    unsigned long flags;
    spin_lock_irqsave(&fence->lock, &flags);
    
    if (fence->flags & DMA_FENCE_FLAG_SIGNALED) {
        spin_unlock_irqrestore(&fence->lock, flags);
        func(fence, cb);
        return 0;
    }
    
    cb->func = func;
    cb->next = fence->cb_list;
    fence->cb_list = cb;
    
    spin_unlock_irqrestore(&fence->lock, flags);
    
    return 0;
}

bool dma_fence_remove_callback(struct dma_fence *fence, struct dma_fence_cb *cb)
{
    if (!fence || !cb)
        return false;
    
    unsigned long flags;
    spin_lock_irqsave(&fence->lock, &flags);
    
    struct dma_fence_cb **prev = &fence->cb_list;
    struct dma_fence_cb *curr = fence->cb_list;
    
    while (curr) {
        if (curr == cb) {
            *prev = curr->next;
            curr->next = NULL;
            spin_unlock_irqrestore(&fence->lock, flags);
            return true;
        }
        prev = &curr->next;
        curr = curr->next;
    }
    
    spin_unlock_irqrestore(&fence->lock, flags);
    return false;
}

struct dma_fence *dma_fence_alloc_timeline(uint64_t context)
{
    spinlock_irq_t lock = {0};
    spinlock_init(&lock.lock);
    
    struct dma_fence *fence = kmalloc(sizeof(struct dma_fence));
    if (!fence)
        return NULL;
    
    dma_fence_init(fence, &dma_fence_default_ops, &lock, context, 0);
    
    return fence;
}

void dma_fence_free_timeline(struct dma_fence *timeline)
{
    if (timeline)
        dma_fence_put(timeline);
}

struct dma_fence *dma_fence_array_create(struct dma_fence **fences, uint32_t num_fences)
{
    if (!fences || num_fences == 0)
        return NULL;
    
    struct dma_fence_array *array = kmalloc(sizeof(struct dma_fence_array));
    if (!array)
        return NULL;
    
    array->fences = kmalloc(sizeof(struct dma_fence *) * num_fences);
    if (!array->fences) {
        kfree(array);
        return NULL;
    }
    
    for (uint32_t i = 0; i < num_fences; i++) {
        array->fences[i] = dma_fence_get(fences[i]);
    }
    
    array->num_fences = num_fences;
    array->signaled = false;
    
    spinlock_irq_t lock = {0};
    spinlock_init(&lock.lock);
    
    dma_fence_init(&array->base, &dma_fence_default_ops, &lock, 0, 0);
    
    return &array->base;
}

void dma_fence_array_free(struct dma_fence *fence)
{
    if (!fence)
        return;
    
    struct dma_fence_array *array = (struct dma_fence_array *)fence;
    
    for (uint32_t i = 0; i < array->num_fences; i++) {
        dma_fence_put(array->fences[i]);
    }
    
    kfree(array->fences);
    kfree(array);
}

const char *dma_fence_get_driver_name(struct dma_fence *fence)
{
    if (!fence || !fence->ops || !fence->ops->get_driver_name)
        return "unknown";
    return fence->ops->get_driver_name(fence);
}

const char *dma_fence_get_timeline_name(struct dma_fence *fence)
{
    if (!fence || !fence->ops || !fence->ops->get_timeline_name)
        return "unknown";
    return fence->ops->get_timeline_name(fence);
}